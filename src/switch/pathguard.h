/*
 * pathguard.h — [PATHGUARD] v36.51 полевой прибор для класса InvalidAccess
 * "байты пути как указатель" (все три адреса из логов Ryu — 0x6E692E73676E6974
 * "tings.in", 0x7365 "es", 0x20697465592F7365 "es/Yeti " — это LE-байты
 * 21..28 строк «sdmc:/switch/j2me/…»).
 *
 * Механика прибора:
 *   1) каждое файловое обращение фронтенда идёт через ПЕРСИСТЕНТНУЮ
 *      slab-копию пути (СТАТИЧЕСКИЙ пул в .bss, 0xA5-канарейка, никогда
 *      не освобождается и не переиспользуется; именно .bss, НЕ куча:
 *      slab в glibc-арене потока умирает вместе с потоком — поймано
 *      SEGV на джорнее v36.51, фаза A) — адресация «яда» меняется: если
 *      слот жил в стеке/BSS/.rodata-копии, передаваемой в fopen/opendir,
 *      признаки изменятся или исчезнут;
 *   2) [FILEOP]-бейдж печатает ОЖИДАЕМОЕ ядовитое значение n21 =
 *      LE64(path[21..28]) в stdout+stderr+log.txt: в логе Ryu строка
 *      [FILEOP] ложится РЯДОМ с InvalidAccess — совпадение n21 с адресом
 *      ошибки называет ОПЕРАЦИЮ и ПУТЬ, которые её породили;
 *   3) [PTRSCAN] после каждой операции ищет по slab/BSS/TLS/libnx/стеку
 *      МЕСТА, реально содержащие эти 8 байт, и отличает строку-на-месте
 *      (по адресу A-21 лежит префикс пути — это просто копия строки) от
 *      СЛОТА (ядовитая копия байт в позиции указателя) — слот и есть
 *      источник InvalidAccess: его регион+смещение = имя виновника.
 *      Стек сканируется только НАД кадрами самого прибора (пол = fp
 *      обёртки) — иначе локалы beacon/needle дают самонаводку; BSS
 *      сканируется минус собственный slab-пул. Каждое уникальное место
 *      репортится ОДИН раз за сессию (дедуп; выключение — env
 *      NOJME_PG_NODEDUP=1 или nojme_pg_set_dedup(0)); SLOT-строка несёт
 *      ctx= — 24 hex-байта вокруг попадания: хвост строки-сиблинга
 *      («…/saves» имеет общую с «…/games» иглу «es»; .jar/.jad — тоже)
 *      отличим от голого слота в канареечной зоне глазами.
 *
 * Подключается только там же, где switch_trace.c (NRO + verify).
 */
#ifndef NOJME_SWITCH_PATHGUARD_H
#define NOJME_SWITCH_PATHGUARD_H

#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>

#if defined(__SWITCH__) || defined(NOJME_SWITCH_TRACE)

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Обёртки файловых вызовов (slab + бейдж + скан) ---- */

FILE* nojme_pg_fopen(const char* tag, const char* path, const char* mode);
void* nojme_pg_opendir(const char* tag, const char* path);
int   nojme_pg_mkdir(const char* tag, const char* path, int mode);
int   nojme_pg_stat(const char* tag, const char* path, struct stat* st);
int   nojme_pg_rename(const char* tag, const char* oldp, const char* newp);
int   nojme_pg_remove(const char* tag, const char* path);

/* ---- Утилиты прибора (экспортированы для теста) ---- */

/* Ожидаемое «ядовитое» значение: LE64 байт 21..28 пути (0 за NUL).
 * Три полевых эталона:
 *   sdmc:/switch/j2me/settings.ini                    -> 0x6E692E73676E6974
 *   sdmc:/switch/j2me/games | saves                   -> 0x0000000000007365
 *   sdmc:/switch/j2me/{games,saves}/Yeti Sports ...   -> 0x20697465592F7365 */
unsigned long long nojme_pg_needle(const char* path);

/* Скан регионов на предмет слотов, содержащих needle(path). */
void nojme_pg_ptrscan(const char* tag, const char* path);

/* Скан ВСЕХ зарегистрированных slab-путей (периодический вызов из
 * главного цикла фронтенда — ловит слоты, появившиеся вне файловых оп). */
void nojme_pg_scan_all(const char* tag);

/* v36.60 [PSCAN-FLAG]: 1 = PTRSCAN-проходы включены (env NOJME_PG_SCAN=1
 * или файл-флаг sdmc:/switch/j2me/pg.flag). По умолчанию ВЫКЛ — проход
 * побайтовый по .bss/стеку/slab и на устройстве стоил 1-3+ с блокировки
 * главного потока (лаги меню и пропажи коротких нажатий после игровой
 * сессии). [FILEOP]-бейджи остаются безусловными. */
int nojme_pg_scan_enabled(void);

/* [WATCH]: зарегистрировать ПРОИЗВОЛЬНЫЙ буфер (например, стековый
 * jar_path вызывающего) как регион скана — ключ по имени, повторный
 * вызов обновляет адрес/длину. */
void nojme_pg_watch(const char* name, const void* addr, size_t len);

/* Канарейка: все ли slab-копии целы (детектор внешних ЗАПИСЕЙ в путь). */
void nojme_pg_slab_check(const char* tag);

/* Дедупликация PTRSCAN-попаданий: 1 (умолчание) = каждая уникальная
 * точка репортится один раз за сессию; 0 = репортить каждое сканирование
 * (env-эквивалент выключения: NOJME_PG_NODEDUP=1). */
void nojme_pg_set_dedup(int enable);

/* Тестовый хук канала отчёта: по умолчанию строка уходит в printf(stdout)
 * + fprintf(stderr) + sw_trace_force (log.txt). Тест подменяет хук. */
typedef void (*nojme_pg_report_fn)(const char* line);
void nojme_pg_set_report_hook(nojme_pg_report_fn hook);

/* Сборка ядра — для бейджа (weak: тест-бинарь может не давать символа). */
const char* nojme_pg_build_id(void);

#ifdef __cplusplus
}
#endif

#else /* !( __SWITCH__ || NOJME_SWITCH_TRACE ): тонкие макро-обёртки.
 * Общие исходники (main.c, rms.c) зовут обёртки БЕЗ гейта — на десктопе/
 * libretro они разворачиваются в прямые вызовы, файл pathguard.c туда
 * вообще не линкуется. */

#define nojme_pg_fopen(tag, path, mode)   fopen((path), (mode))
#define nojme_pg_opendir(tag, path)       opendir(path)
#define nojme_pg_mkdir(tag, path, mode)   mkdir((path), (mode))
#define nojme_pg_stat(tag, path, st)      stat((path), (st))
#define nojme_pg_rename(tag, oldp, newp)  rename((oldp), (newp))
#define nojme_pg_remove(tag, path)        remove(path)
#define nojme_pg_needle(path)             (0ULL)
#define nojme_pg_ptrscan(tag, path)       ((void)0)
#define nojme_pg_scan_all(tag)            ((void)0)
#define nojme_pg_scan_enabled()           (0)
#define nojme_pg_watch(name, addr, len)   ((void)0)
#define nojme_pg_slab_check(tag)          ((void)0)
#define nojme_pg_set_dedup(enable)        ((void)0)

#endif /* gate */

#endif
