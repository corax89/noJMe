/*
 * pathguard.c — [PATHGUARD] v36.51 (см. pathguard.h). Компилируется в те
 * же цели, что switch_trace.c (NRO + switchui-verify); в headless/app/
 * libretro файл НЕ линкуется, а вызывающие места гейтятся тем же
 * условием, что и сам trace-слой.
 *
 * Регионы скана [PTRSCAN]:
 *   - все зарегистрированные slab-копии путей (статический пул .bss,
 *     персистентный — см. примечание об арене умершего потока ниже);
 *   - TLS-буфер libnx __nx_dev_path_buf (только реальный NRO) — туда
 *     fsdev_fixpath кладёт КАЖДЫЙ путь перед syscall;
 *   - .bss образа (слоты в наших глобалах: g_settings и пр.);
 *   - 8 КБ стека НАД текущим кадром (более старые кадры вызывающих).
 * Классификация попадания по адресу A (LE64(A) == needle):
 *   memcmp(A-21, path, len+1) == 0  -> "str"  — сама строка лежит на месте
 *                                       (безобидное хранилище копии);
 *   копия без «sdmc:» (игла на A-(21-pfx), fsdev срезает префикс
 *                       устройства) -> "str-raw" — тоже не слот (v36.52);
 *   иначе                            -> "SLOT" — ядовитая копия байт в
 *                                       позиции, читаемой как указатель.
 */
#include "switch/pathguard.h"

#include <dirent.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/stat.h>

#if defined(__SWITCH__) || defined(NOJME_SWITCH_TRACE)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#include "switch/switch_trace.h"

/* ---- build id (weak — тест-бинарь может не давать символа) ---- */
extern const char* j2me_core_build_id(void) __attribute__((weak));
const char* nojme_pg_build_id(void) {
    if (j2me_core_build_id) return j2me_core_build_id();
    return "test";
}

/* ---- libnx TLS path buffer (реальное железо/NRO) ---- */
#if defined(__SWITCH__) && (defined(__aarch64__) || defined(__arm__))
extern char __thread __nx_dev_path_buf[1025]; /* PATH_MAX+1 */
#define PG_HAVE_NX_TLS 1
#endif

/* ---- .bss образа: NRO даёт __bss_start__/__bss_end__; glibc —
 * __bss_start + _end (__bss_end там НЕ определяется). Оба — weak:
 * что разрешилось, то и сканируем. ---- */
extern char __bss_start__[] __attribute__((weak));
extern char __bss_end__[] __attribute__((weak));
extern char __bss_start[] __attribute__((weak));
extern char __bss_end[] __attribute__((weak));
extern char _end[] __attribute__((weak));

/* Потолок стека ТЕКУЩЕГО потока: __builtin_frame_address(0) самого
 * ВЫСОКОГО (первого/мелкого) вызова обёртки на этом потоке (главный
 * поток — стартовый nojme_pg_mkdir прямо из main: fp = дно кадра
 * main). Скан идёт только [пол .. потолок]: пол = fp ТЕКУЩЕЙ обёртки
 * (кадры прибора ниже пола исключены — самонаводка), от пола до
 * потолка — живые кадры вызывающих, отображены ПО ПОСТРОЕНИЮ;
 * выше потолка (вершина стека/env) не читаем НИКОГДА. */
static __thread char* t_pg_stack_ceiling = NULL;

/* ПОЛ стека для СКАНА: кадр самой ВЕРХНЕЙ (мелкой) обёртки на текущем
 * вызове. Всё НИЖЕ него — кадры самого прибора (обёртка → beacon →
 * needle → ptrscan), в том числе МЁРТВЫЕ кадры nojme_pg_needle/pg_emit:
 * их локалы ГАРАНТИРОВАННО содержат иглу в двоичном виде — на джорнее
 * это давало систематическую самонаводку «stack+0x87d SLOT» на каждом
 * file-op. Сканируем только НАД полом = кадры вызывающих (меню/мейн/
 * игровой поток) — там и только там живёт настоящий яд. */
static __thread char* t_pg_stack_floor = NULL;

/* ---- канал отчёта (ОБЪЯВЛЕН РАНЬШЕ ПОЛЬЗОВАТЕЛЕЙ) ---- */
static pthread_mutex_t g_pg_mu = PTHREAD_MUTEX_INITIALIZER;
static nojme_pg_report_fn g_pg_report = NULL; /* NULL = канал по умолчанию */

/* Запись потолка на уровне ОБЁРТКИ (fp = верх её кадра = дно кадра
 * вызывающего): выше может быть только живой кадр вызывающего. */
static void pg_note_ceiling(void) {
    char* fp = (char*)__builtin_frame_address(0);
    if (!t_pg_stack_ceiling || fp > t_pg_stack_ceiling)
        t_pg_stack_ceiling = fp;
}

/* ---- [WATCH]: явные регионы наблюдения (стековые буферы путей у
 * вызывающих: jar_path в main, br.cwd в меню, filepath/tmppath в RMS)
 * — сканируются наравне со slab/BSS/TLS/стеком. ---- */
#define PG_WATCH_MAX 24
typedef struct {
    const void* addr;
    size_t len;
    pthread_t owner; /* смотреть может ТОЛЬКО поток-владелец: стек
                      * мёртвого потока уничтожен — чужие записи
                      * пропускаем, а не разыменовываем */
    char name[24];
} PGWatch;
static PGWatch g_pg_watch[PG_WATCH_MAX];
static int g_pg_watch_n = 0;

void nojme_pg_watch(const char* name, const void* addr, size_t len) {
    if (!name || !addr || !len) return;
    pthread_mutex_lock(&g_pg_mu);
    int i;
    for (i = 0; i < g_pg_watch_n; i++)
        if (strcmp(g_pg_watch[i].name, name) == 0) break;
    if (i >= PG_WATCH_MAX) i = PG_WATCH_MAX - 1; /* вытесняем последний */
    if (i >= g_pg_watch_n) g_pg_watch_n = i + 1;
    g_pg_watch[i].addr = addr;
    g_pg_watch[i].len = len > 4096 ? 4096 : len;
    g_pg_watch[i].owner = pthread_self();
    snprintf(g_pg_watch[i].name, sizeof(g_pg_watch[i].name), "%s", name);
    pthread_mutex_unlock(&g_pg_mu);
}

/* ---- канал отчёта: реализация ---- */
static void pg_emit(const char* fmt, ...) {
    char line[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (g_pg_report) {
        g_pg_report(line);
        return;
    }
    /* канал по умолчанию: stdout (лог Ryu показывает stdout homebrew
     * рядом с InvalidAccess), stderr, log.txt (sw_trace_force; при
     * выключенном «Лог» строка уходит только в два первых). */
    printf("%s\n", line);
    fflush(stdout);
    fprintf(stderr, "%s\n", line);
    fflush(stderr);
    sw_trace_force("%s", line);
}

void nojme_pg_set_report_hook(nojme_pg_report_fn hook) {
    pthread_mutex_lock(&g_pg_mu);
    g_pg_report = hook;
    pthread_mutex_unlock(&g_pg_mu);
}

/* ---- slab-реестр ----
 * Буферы живут в СТАТИЧЕСКОМ пуле .bss (НЕ куча): malloc с игрового
 * потока попадает в его glibc-арену, которая УНИЧТОЖАЕТСЯ при смерти
 * потока — персистентные slab-указатели повисали бы (полевой класс
 * «InvalidAccess по байтам пути»: поймано SEGV в скане на джорнее
 * v36.51, фаза A). Пул в .bss переживает всё. */
#define PG_SLAB_BYTES 320
#define PG_MAX        96
typedef struct {
    char   buf[PG_SLAB_BYTES];
    size_t len;      /* strlen пути в момент регистрации; 0 = слот свободен */
    char   tag[20];
    unsigned seq;
} PGEntry;
static PGEntry g_pg[PG_MAX];
static unsigned g_pg_seq = 0;
static int g_pg_banner = 0;

/* v36.60 [PSCAN-FLAG]: РЕШЕНИЕ полевой задачи «после выхода из мидлета
 * меню сильно тормозит и пропускает нажатия». Корень оказался в этом
 * самом приборе: [PTRSCAN]-проход — это ПОБАЙТОВЫЙ (шаг 1, а не 8 —
 * игла-строка может лежать на любом смещении) просмотр slab-пула
 * (30 КБ) + TLS + .bss образа (~2 МБ) + стека (до 256 КБ) — и этот
 * проход запускался ПОСЛЕ КАЖДОЙ файловой операции, а из цикла меню —
 * ещё и раз в 2 с ДЛЯ ВСЕХ зарегистрированных (tag,path)-слотов (до 96;
 * после игровой сессии их десятки: jar/pergame/RMS/.bak/.tmp/каталоги).
 * Итого на устройстве: раз в 2 с — 1-3+ с блокировки главного потока,
 * а на Switch-SDL2 события геймпада ГЕНЕРИРУЮТСЯ только внутри
 * SDL_PumpEvents/PollEvent — нажатие-отпускание, целиком попавшее в
 * «слепое» окно, НЕ становится событием ВООБЩЕ (в полевом логе это
 * видно как пропажи [KEYIN] между живыми сериями). Расследование
 * InvalidAccess, ради которого всё это было построено, ЗАВЕРШЕНО
 * (два полевых лога: яда нет, все попадания — 4 известных безобидных
 * класса), поэтому сканер становится ОПЦИОНАЛЬНЫМ:
 *   - по умолчанию — ВЫКЛ (нулевая цена, нулевой спам в log.txt);
 *   - env NOJME_PG_SCAN=1 (хост/тесты) или пустой файл-флаг
 *     sdmc:/switch/j2me/pg.flag (DIAG-FLAGS в main.c, устройство) —
 *     ВКЛ на всю сессию; диагностический режим может подтормаживать —
 *     это осознанно.
 * [FILEOP]-бейджи (одна строка на операцию) остаются БЕЗУСЛОВНО — они
 * дёшевы и остаются коррелятом «n21 полевого адреса = операция+путь»,
 * если InvalidAccess вернётся. Проверка getenv — БЕЗ кэша: DIAG-FLAGS
 * в main.c ставит env ПОСЛЕ первых операций старта (settings-load),
 * закэшированное «выкл» осталось бы навсегда. */
static int pg_scan_on(void) {
    const char* e = getenv("NOJME_PG_SCAN");
    int on = (e && e[0] && e[0] != '0') ? 1 : 0;
    if (on) {
        /* один раз за процесс: доказательство режима в log.txt.
         * ОТДЕЛЬНЫЙ флаг (не g_pg_banner!): баннер «[PATHGUARD] build=»
         * первого beacon-а не должен глушиться (test: бейдж сборки на
         * первой операции). */
        static int s_scan_banner = 0;
        if (!s_scan_banner) {
            s_scan_banner = 1;
            pg_emit("[PATHGUARD] build=%s PTRSCAN ON (NOJME_PG_SCAN/pg.flag) — "
                    "диагностический режим, возможны паузы",
                    nojme_pg_build_id());
        }
    }
    return on;
}

int nojme_pg_scan_enabled(void) { return pg_scan_on(); }

/* Игла: LE64(path[21..28]), 0 за NUL (БЕЗ чтения за конец строки —
 * путь «…/games» (23 байта) даёт иглу 0x7365 из байт 'e','s' + нули). */
unsigned long long nojme_pg_needle(const char* path) {
    unsigned long long v = 0;
    if (!path) return 0;
    size_t len = 0;
    while (len < 4096 && path[len]) len++;
    for (int i = 0; i < 8; i++) {
        unsigned char c = 0;
        if ((size_t)(21 + i) < len) c = (unsigned char)path[21 + i];
        v |= (unsigned long long)c << (8 * i);
    }
    return v;
}

/* Копия пути в персистентный slab (реестр под мьютексом). */
static const char* pg_slab_get(const char* tag, const char* path) {
    if (!path || !path[0]) return path;
    size_t len = strlen(path);
    if (len >= PG_SLAB_BYTES - 32) return path; /* патологический — как есть */

    pthread_mutex_lock(&g_pg_mu);
    /* реюз по (tag, path) */
    for (int i = 0; i < PG_MAX; i++) {
        if (g_pg[i].len && strcmp(g_pg[i].tag, tag) == 0 &&
            strcmp(g_pg[i].buf, path) == 0) {
            pthread_mutex_unlock(&g_pg_mu);
            return g_pg[i].buf;
        }
    }
    int slot = -1;
    for (int i = 0; i < PG_MAX; i++)
        if (!g_pg[i].len) { slot = i; break; }
    if (slot < 0) { /* реестр полон — переиспользуем самый старый */
        unsigned oldest = ~0u; slot = 0;
        for (int i = 0; i < PG_MAX; i++)
            if (g_pg[i].seq < oldest) { oldest = g_pg[i].seq; slot = i; }
    }
    char* b = g_pg[slot].buf;
    memset(b, 0xA5, PG_SLAB_BYTES);
    memcpy(b, path, len + 1);
    snprintf(g_pg[slot].tag, sizeof(g_pg[slot].tag), "%s", tag);
    g_pg[slot].len = len;
    g_pg[slot].seq = ++g_pg_seq;
    pthread_mutex_unlock(&g_pg_mu);
    return b;
}

/* Канарейка: байты после NUL должны остаться 0xA5 (детектор внешних
 * ЗАПИСЕЙ в slab-копию пути). Вызывается БЕЗ g_pg_mu. */
void nojme_pg_slab_check(const char* tag) {
    (void)tag;
    pthread_mutex_lock(&g_pg_mu);
    for (int i = 0; i < PG_MAX; i++) {
        if (!g_pg[i].len) continue;
        for (size_t j = g_pg[i].len + 1; j < PG_SLAB_BYTES; j++) {
            if ((unsigned char)g_pg[i].buf[j] != 0xA5) {
                pg_emit("[PATHGUARD] SLAB-WRITE tag=%.19s slab=0x%lx off=%lu "
                        "now=0x%02x (внешняя запись в slab-копию пути)",
                        g_pg[i].tag, (unsigned long)(uintptr_t)g_pg[i].buf,
                        (unsigned long)j, (unsigned char)g_pg[i].buf[j]);
                pthread_mutex_unlock(&g_pg_mu);
                return; /* одна строка на slab достаточно */
            }
        }
    }
    pthread_mutex_unlock(&g_pg_mu);
}

/* ---- дедупликация [PTRSCAN]-попаданий (v36.51, полевая доработка) ----
 * Один и тот же слот/хранилище при каждом file-op и периодическом скане
 * репортится заново — на джорнее одна фаза давала 5000+ строк и топила
 * сигнал. Теперь каждая УНИКАЛЬНАЯ точка (регион-имя + смещение + игла +
 * класс) репортится ОДИН РАЗ за сессию. Выключается env NOJME_PG_NODEDUP=1
 * или nojme_pg_set_dedup(0). */
#define PG_SEEN_MAX 512
static uint64_t g_pg_seen[PG_SEEN_MAX];
static unsigned g_pg_seen_n = 0;
static unsigned g_pg_seen_next = 0;
static int g_pg_dedup = -1; /* -1 = env ещё не читан */

void nojme_pg_set_dedup(int enable) {
    pthread_mutex_lock(&g_pg_mu);
    g_pg_dedup = enable ? 1 : 0;
    pthread_mutex_unlock(&g_pg_mu);
}

/* Ключ попадания: FNV-1a по (имя региона, смещение, игла, класс). */
static uint64_t pg_hit_key(const char* name, size_t off,
                           unsigned long long needle, int benign) {
    uint64_t h = 1469598103934665603ULL;
    for (int k = 0; k < 24 && name[k]; k++) {
        h ^= (unsigned char)name[k]; h *= 1099511628211ULL;
    }
    h ^= (uint64_t)off;       h *= 1099511628211ULL;
    h ^= needle;              h *= 1099511628211ULL;
    h ^= (uint64_t)(benign ? 1 : 0); h *= 1099511628211ULL;
    return h;
}

/* 1 = это место уже репортили (подавить). */
static int pg_seen(uint64_t key) {
    pthread_mutex_lock(&g_pg_mu);
    if (g_pg_dedup < 0) {
        const char* e = getenv("NOJME_PG_NODEDUP");
        g_pg_dedup = !(e && e[0] && strcmp(e, "0") != 0);
    }
    if (!g_pg_dedup) { /* дедуп выключен — не подавляем ничего */
        pthread_mutex_unlock(&g_pg_mu);
        return 0;
    }
    int dup = 0;
    unsigned n = g_pg_seen_n < PG_SEEN_MAX ? g_pg_seen_n : PG_SEEN_MAX;
    for (unsigned k = 0; k < n; k++)
        if (g_pg_seen[k] == key) { dup = 1; break; }
    if (!dup) {
        g_pg_seen[g_pg_seen_next] = key;
        g_pg_seen_next = (g_pg_seen_next + 1) % PG_SEEN_MAX;
        if (g_pg_seen_n < PG_SEEN_MAX) g_pg_seen_n++;
    }
    pthread_mutex_unlock(&g_pg_mu);
    return dup;
}

/* ---- [PTRSCAN]: поиск слотов со значением needle ----
 * Сканер СОЗНАТЕЛЬНО читает «чужую» память (кадры вызывающих, весь
 * .bss, TLS libnx) — в production всё отображено; под ASan такие
 * сырые чтения дают ложные stack/global-buffer-overflow, поэтому
 * сканер помечен no_sanitize_address. */
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define PG_NOASAN __attribute__((no_sanitize_address))
#  endif
#endif
#ifndef PG_NOASAN
#  define PG_NOASAN
#endif

/* Hex-контекст попадания: 24 байта [i-16 .. i+8) с клампом к границам
 * региона — по контексту читатель мгновенно отличает хвост строки-сиблинга
 * (…2f 73 61 76 65 73 00 = «…/saves», игла «es» обчая с games) от голого
 * слота в канареечной зоне (a5 a5 … 65 73 00 …). */

/* v36.52 [NO-SPILL]: игла в hex без vsnprintf-вараргов — «%016llx» клал
 * сырой qword в рег-сейв-зону callee, и ПОГИБШИЕ кадры более мелких
 * вызовов всплывали в следующих (более глубоких) сканах как ложные
 * «SLOT {rc,len,n21}» (полевой лог yetifix8: ui:cwd+0x160/0x170/0x3a0,
 * где тройка БАЙТ-В-БАЙТ повторяла rc/len только что выполненной
 * операции). Теперь n21 печатается ТОЛЬКО из строки, собранной байт-в-
 * байт без передачи иглы как аргумента. */
static void pg_hex16(char* out, unsigned long long v) {
    static const char hexd[] = "0123456789abcdef";
    for (int k = 15; k >= 0; k--) { out[k] = hexd[v & 15]; v >>= 4; }
    out[16] = 0;
}
static void pg_ctx_hex(const unsigned char* base, size_t size, size_t i,
                       char* out, size_t outsz) {
    static const char hexd[] = "0123456789abcdef";
    size_t start = i >= 16 ? i - 16 : 0;
    size_t end = i + 8;
    if (end > size) end = size;
    size_t pos = 0;
    for (size_t k = start; k < end && pos + 2 < outsz; k++) {
        out[pos++] = hexd[base[k] >> 4];
        out[pos++] = hexd[base[k] & 15];
    }
    out[pos] = 0;
}

static PG_NOASAN void pg_scan_region(const char* tag, const char* name,
                           const unsigned char* base, size_t size,
                           const char* path, size_t plen,
                           unsigned long long needle, int* hits, int cap) {
    if (!base || size < 8) return;
    if (size > 8u * 1024u * 1024u) size = 8u * 1024u * 1024u; /* щит: 8 МБ */
    for (size_t i = 0; i + 8 <= size && *hits < cap; i++) {
        unsigned long long v;
        memcpy(&v, base + i, 8);
        if (v != needle) continue;
        /* строка-на-месте? (по A-21 начинается полная копия пути) */
        int benign = 0;
        if (i >= 21) {
            size_t start = i - 21;
            if (start + plen + 1 <= size &&
                memcmp(base + start, path, plen + 1) == 0)
                benign = 1;
        }
        /* v36.52 [STR-RAW]: копия БЕЗ префикса устройства («sdmc:» срезает
         * fsdev при заходе в devoptab — такой вид лежит в __nx_dev_path_buf
         * и libc-кадрах самой операции; в полевом логе yetifix8 это давало
         * систематические двойные ложные «nx_tls_pathbuf+0x10 SLOT» и
         * «bss+0x16da8 SLOT»). В сырой строке игла стоит на (21-pfx).
         * Распознаём и помечаем — это тоже хранилище строки, не слот. */
        if (!benign) {
            size_t pfx = 0;
            for (size_t k = 0; k < 16 && k < plen; k++)
                if (path[k] == ':') { pfx = k + 1; break; }
            if (pfx && pfx <= 21 && plen > pfx) {
                size_t npos = 21 - pfx;       /* позиция иглы в сырой копии */
                size_t rlen = plen - pfx;     /* длина сырой строки без NUL */
                if (i >= npos && (i - npos) + rlen + 1 <= size &&
                    memcmp(base + (i - npos), path + pfx, rlen + 1) == 0)
                    benign = 2;
            }
        }
        /* дедуп: уникальная точка репортится один раз за сессию */
        if (pg_seen(pg_hit_key(name, i, needle, benign))) continue;
        char nbuf[24];
        nbuf[0] = '0'; nbuf[1] = 'x';        /* «0x» + 16 hex, без вараргов */
        pg_hex16(nbuf + 2, needle);
        if (benign == 1) {
            pg_emit("[PTRSCAN] tag=%s region=%s+0x%lx str-in-place n21=%s "
                    "(хранилище самой строки — не слот)",
                    tag, name, (unsigned long)i, nbuf);
        } else if (benign == 2) {
            pg_emit("[PTRSCAN] tag=%s region=%s+0x%lx str-raw n21=%s "
                    "(копия без префикса устройства — не слот)",
                    tag, name, (unsigned long)i, nbuf);
        } else {
            char ctx[64];
            pg_ctx_hex(base, size, i, ctx, sizeof(ctx));
            pg_emit("[PTRSCAN] tag=%s region=%s+0x%lx SLOT n21=%s "
                    "addr=0x%lx ctx=%s (ЯДОВИТЫЙ СЛОТ: байты пути в позиции "
                    "указателя; ctx = 24 байта вокруг)",
                    tag, name, (unsigned long)i, nbuf,
                    (unsigned long)(uintptr_t)(base + i), ctx);
        }
        (*hits)++;
    }
}

void nojme_pg_ptrscan(const char* tag, const char* path) {
    if (!pg_scan_on()) return; /* v36.60 [PSCAN-FLAG] */
    if (!path || !path[0]) return;
    unsigned long long needle = nojme_pg_needle(path);
    if (!needle) return; /* путь короче 22 байт — иглы нет */
    size_t plen = strlen(path);
    int hits = 0;
    const int cap = 24;

    /* v36.52 [FLOOR-UP]: пол стека вычисляем ОДИН раз в самом верху —
     * он нужен и WATCH-проходу. Владелец WATCH-региона мог ВЕРНУТЬ кадр
     * (меню закрылось до запуска игры): его диапазон на стеке теперь —
     * территория кадров ТЕКУЩЕЙ цепочки прибора, и без клампа прибор
     * находил там собственные записи (полевой лог yetifix8: серии
     * «rms:tmppath+0x8/0xd0/0x110/0x1d8 SLOT» поверх кадров сканера).
     * Стек-регионы ниже пола не читаем; куча/BSS (далеко от стека)
     * не затрагиваются. */
    char* floor = (char*)__builtin_frame_address(0);
    if (t_pg_stack_floor && t_pg_stack_floor > floor)
        floor = t_pg_stack_floor;
    if (!t_pg_stack_ceiling)
        t_pg_stack_ceiling = floor;

    /* снимок slab-адресов под мьютексом (буферы персистентны, содержимое
     * читаем напрямую — запись идёт только при регистрации) */
    const unsigned char* slabs[PG_MAX];
    const char* tags[PG_MAX];
    int n = 0;
    pthread_mutex_lock(&g_pg_mu);
    for (int i = 0; i < PG_MAX && n < PG_MAX; i++)
        if (g_pg[i].len) {
            slabs[n] = (const unsigned char*)g_pg[i].buf;
            tags[n] = g_pg[i].tag;
            n++;
        }
    pthread_mutex_unlock(&g_pg_mu);

    for (int i = 0; i < n && hits < cap; i++)
        pg_scan_region(tag, tags[i], slabs[i], PG_SLAB_BYTES,
                       path, plen, needle, &hits, cap);

    {   /* [WATCH]-регионы (снимок под мьютексом) */
        const void* wa[PG_WATCH_MAX];
        size_t wl[PG_WATCH_MAX];
        const char* wn[PG_WATCH_MAX];
        int wn_n = 0;
        pthread_t self = pthread_self();
        pthread_mutex_lock(&g_pg_mu);
        for (int i = 0; i < g_pg_watch_n && wn_n < PG_WATCH_MAX; i++) {
            if (!pthread_equal(g_pg_watch[i].owner, self)) continue;
            wa[wn_n] = g_pg_watch[i].addr;
            wl[wn_n] = g_pg_watch[i].len;
            wn[wn_n] = g_pg_watch[i].name;
            wn_n++;
        }
        pthread_mutex_unlock(&g_pg_mu);
        for (int i = 0; i < wn_n && hits < cap; i++) {
            const char* a = (const char*)wa[i];
            size_t l = wl[i];
            /* v36.52 [WATCH-CLAMP]: stack-регион НИЖЕ пола скана — кадры
             * текущей цепочки прибора / мёртвый буфер поверх них — не
             * читаем (полевой лог yetifix8 ловил там собственные записи:
             * «rms:tmppath+0x8/0xd0/0x110/0x1d8», «ui:cwd+0x160/0x170/
             * 0x3a0» с тройками {rc,len,n21}). Дискриминатор: адрес под
             * потолком стека потока И не глубже 256 КБ под полом;
             * куча/BSS далеко от стека — не затрагиваются (test 6). */
            if (a < (const char*)t_pg_stack_ceiling && a < floor &&
                (size_t)(floor - a) <= 256u * 1024u) {
                size_t drop = (size_t)(floor - a);
                if (drop >= l) continue;      /* целиком ниже пола */
                a = (const char*)floor;       /* хвост над полом — можно */
                l -= drop;
            }
            pg_scan_region(tag, wn[i], (const unsigned char*)a, l,
                           path, plen, needle, &hits, cap);
        }
    }

#if defined(PG_HAVE_NX_TLS)
    pg_scan_region(tag, "nx_tls_pathbuf",
                   (const unsigned char*)__nx_dev_path_buf, 1025,
                   path, plen, needle, &hits, cap);
#endif
    {
        const unsigned char* b = NULL;
        size_t sz = 0;
        if ((const void*)__bss_start__ && (const void*)__bss_end__ &&
            &__bss_end__[0] > &__bss_start__[0]) {
            b = (const unsigned char*)__bss_start__;
            sz = (size_t)(&__bss_end__[0] - &__bss_start__[0]);
        } else if ((const void*)__bss_start && (const void*)__bss_end &&
                   &__bss_end[0] > &__bss_start[0]) {
            b = (const unsigned char*)__bss_start;
            sz = (size_t)(&__bss_end[0] - &__bss_start[0]);
        } else if ((const void*)__bss_start && (const void*)_end &&
                   &_end[0] > &__bss_start[0]) {
            b = (const unsigned char*)__bss_start;
            sz = (size_t)(&_end[0] - &__bss_start[0]);
        }
        if (b) {
            /* исключаем СОБСТВЕННЫЙ slab-пул g_pg: его буфы сканируют
             * отдельные проходы (region=<tag>), а хвосты служебных полей
             * (tag-строки «mkdir-games», len/seq) дают ложные SLOT —
             * прибор не должен находить собственные структуры. */
            char* pg_lo = (char*)g_pg;
            char* pg_hi = (char*)g_pg + sizeof(g_pg);
            char* lo = (char*)b;
            char* hi = (char*)b + sz;
            if (pg_hi <= lo || pg_lo >= hi) {
                pg_scan_region(tag, "bss", b, sz, path, plen, needle, &hits, cap);
            } else {
                if (pg_lo > lo)
                    pg_scan_region(tag, "bss", (const unsigned char*)lo,
                                   (size_t)(pg_lo - lo), path, plen,
                                   needle, &hits, cap);
                if (pg_hi < hi)
                    pg_scan_region(tag, "bss", (const unsigned char*)pg_hi,
                                   (size_t)(hi - pg_hi), path, plen,
                                   needle, &hits, cap);
            }
        }
    }
    {   /* стек: только [ПОЛ .. ПОТОЛОК] потока. ПОЛ вычислен вверху
         * (FLOOR-UP): fp ТЕКУЩЕЙ обёртки/ptrscan — ниже него кадры
         * прибора с иглой в двоичном виде (самонаводка), выше — живые
         * кадры вызывающих (меню/мейн/игровой поток) — единственное
         * место, где hunt. Потолок = fp самого мелкого вызова обёртки
         * за жизнь потока; вершину стека/env не пересекаем никогда. */
        size_t span = (size_t)(t_pg_stack_ceiling - floor);
        if (span > 256u * 1024u) span = 0; /* чужой потолок — щит */
        if (span > 8)
            pg_scan_region(tag, "stack", (const unsigned char*)floor,
                           span, path, plen, needle, &hits, cap);
    }
}

void nojme_pg_scan_all(const char* tag) {
    if (!pg_scan_on()) return; /* v36.60 [PSCAN-FLAG]: цикл меню каждые 2 с — бесплатно */
    const char* paths[PG_MAX];
    int n = 0;
    pthread_mutex_lock(&g_pg_mu);
    for (int i = 0; i < PG_MAX; i++)
        if (g_pg[i].len) { paths[n] = g_pg[i].buf; n++; }
    pthread_mutex_unlock(&g_pg_mu);
    for (int i = 0; i < n; i++)
        nojme_pg_ptrscan(tag, paths[i]);
}

/* ---- бейдж операции ---- */
static void pg_beacon(const char* tag, const char* op, const char* path,
                      long rc, int scan) {
    if (!g_pg_banner) {
        g_pg_banner = 1;
        pg_emit("[PATHGUARD] build=%s (slab+canary+n21 beacon on)",
                nojme_pg_build_id());
    }
    /* v36.52 [NO-SPILL]: n21 печатаем из строки, собранной байт-в-байт —
     * варарг-спилл vsnprintf с иглой давал в ПОГИБШИХ кадрах ложные
     * SLOT в следующих сканах (полевой лог yetifix8: ui:cwd+0x160/0x170/
     * 0x3a0, тройки {rc,len,n21} байт-в-байт повторяли rc/len операции). */
    char nbuf[24];
    nbuf[0] = '0'; nbuf[1] = 'x';
    pg_hex16(nbuf + 2, nojme_pg_needle(path));
    pg_emit("[FILEOP] tag=%s op=%s rc=%ld len=%d n21=%s path=%.64s",
            tag, op, rc, path ? (int)strlen(path) : 0, nbuf,
            path ? path : "(null)");
    if (scan) nojme_pg_ptrscan(tag, path);
    nojme_pg_slab_check(tag);
}

/* ---- обёртки ----
 * Каждая на ВХОДЕ фиксирует ПОЛ скана = собственный fp (выше — только
 * кадры вызывающего), на ВЫХОДЕ восстанавливает прежний: кадры прибора
 * (beacon/needle/ptrscan/slab_get и мёртвые libc-кадры самой операции)
 * никогда не попадают в стековый регион скана. */
FILE* nojme_pg_fopen(const char* tag, const char* path, const char* mode) {
    char* saved_floor_ = t_pg_stack_floor;
    t_pg_stack_floor = (char*)__builtin_frame_address(0);
    pg_note_ceiling();
    const char* p = pg_slab_get(tag, path);
    FILE* f = fopen(p, mode);
    pg_beacon(tag, "fopen", p, f ? 0 : -1, 1);
    t_pg_stack_floor = saved_floor_;
    return f;
}

void* nojme_pg_opendir(const char* tag, const char* path) {
    char* saved_floor_ = t_pg_stack_floor;
    t_pg_stack_floor = (char*)__builtin_frame_address(0);
    pg_note_ceiling();
    const char* p = pg_slab_get(tag, path);
    void* d = opendir(p);
    pg_beacon(tag, "opendir", p, d ? 0 : -1, 1);
    t_pg_stack_floor = saved_floor_;
    return d;
}

int nojme_pg_mkdir(const char* tag, const char* path, int mode) {
    char* saved_floor_ = t_pg_stack_floor;
    t_pg_stack_floor = (char*)__builtin_frame_address(0);
    pg_note_ceiling();
    const char* p = pg_slab_get(tag, path);
    int r = mkdir(p, mode);
    pg_beacon(tag, "mkdir", p, r, 1);
    t_pg_stack_floor = saved_floor_;
    return r;
}

int nojme_pg_stat(const char* tag, const char* path, struct stat* st) {
    char* saved_floor_ = t_pg_stack_floor;
    t_pg_stack_floor = (char*)__builtin_frame_address(0);
    pg_note_ceiling();
    const char* p = pg_slab_get(tag, path);
    int r = stat(p, st);
    pg_beacon(tag, "stat", p, r, 0); /* stat — часто; бейдж без скана */
    t_pg_stack_floor = saved_floor_;
    return r;
}

int nojme_pg_rename(const char* tag, const char* oldp, const char* newp) {
    char* saved_floor_ = t_pg_stack_floor;
    t_pg_stack_floor = (char*)__builtin_frame_address(0);
    pg_note_ceiling();
    const char* a = pg_slab_get(tag, oldp);
    const char* b = pg_slab_get(tag, newp);
    /* v36.52 [SIM-EEXIST]: NOJME_PG_RENAME_EEXIST=1 — симуляция семантики
     * HOS/LibHac на POSIX-хосте (RenameFile НЕ заменяет существующего
     * получателя; именно из-за этого все rms-rename в полевом логе
     * yetifix8 вернули rc=-1): если получатель существует, реальный
     * rename не вызывается, rc=-1/errno=EEXIST. Для проверки полного
     * фоллбек-пути rms_portable_rename (bak→retry→copy) на хосте;
     * на устройстве выключен по умолчанию. */
    static int s_sim = -1;
    if (s_sim < 0) {
        const char* e = getenv("NOJME_PG_RENAME_EEXIST");
        s_sim = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    int r;
    struct stat sim_st;
    if (s_sim && b && stat(b, &sim_st) == 0) {
        errno = EEXIST;
        r = -1;
    } else {
        r = rename(a, b);
    }
    int e_ = errno;
    pg_beacon(tag, "rename", b, r, 1);
    if (r != 0)
        pg_emit("[FILEOP] tag=%s op=rename-why errno=%d old=%.64s new=%.64s",
                tag, e_, a ? a : "(null)", b ? b : "(null)");
    t_pg_stack_floor = saved_floor_;
    return r;
}

int nojme_pg_remove(const char* tag, const char* path) {
    char* saved_floor_ = t_pg_stack_floor;
    t_pg_stack_floor = (char*)__builtin_frame_address(0);
    pg_note_ceiling();
    const char* p = pg_slab_get(tag, path);
    int r = remove(p);
    pg_beacon(tag, "remove", p, r, 0);
    t_pg_stack_floor = saved_floor_;
    return r;
}

#else /* !( __SWITCH__ || NOJME_SWITCH_TRACE ): пустой TU — на этих целях
       * файл не линкуется, а обёртки в pathguard.h уже макро-пасс-through. */

#endif
