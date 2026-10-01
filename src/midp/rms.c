/*
 * J2ME Emulator - MIDP2 RMS (Record Management System)
 *
 * v17 OVERHAUL (spec-compliance pass):
 *  - Thread safety: one recursive lock (CRITICAL_SECTION on Windows /
 *    PTHREAD_MUTEX_RECURSIVE elsewhere) guards every entry point. Listener
 *    callbacks run Java code which may re-enter RMS on the same thread -
 *    the recursive lock keeps that legal.
 *  - Record IDs are strictly monotonic per store (never reused), matching
 *    RecordStore.getNextRecordID() semantics.
 *  - Dynamic limits: store count 256, records per store unbounded (dynamic
 *    array), max record size 512 KB (was 64 stores / 1024 records / 64 KB).
 *  - Exception contracts enforced per MIDP javadoc:
 *      addRecord/setRecord/deleteRecord/getRecord* - RecordStoreNotOpenException
 *      when closed, InvalidRecordIDException when id does not exist,
 *      RecordStoreFullException when out of space, NullPointerException for
 *      null data, ArrayIndexOutOfBoundsException for bad offset/length.
 *    deleteRecordStore - RecordStoreNotFoundException when missing,
 *      RecordStoreException when the store is still open.
 *    closeRecordStore/getSize/getSizeAvailable/getVersion/getLastModified -
 *      RecordStoreNotOpenException when closed.
 *  - Zero-length records are legal (spec: addRecord with numBytes==0 adds
 *      an empty record).
 *  - setMode(authmode, writable) is honored: invalid authmode throws
 *      IllegalArgumentException; writes to a read-only store throw
 *      RecordStoreException. Mode is persisted.
 *  - RecordListener support: add/remove + recordAdded/recordChanged/
 *      recordDeleted delivered synchronously after each successful mutation.
 *  - RecordEnumeration: keepUpdated(true) registers the enumeration for
 *      automatic refresh on store mutations; rebuild() re-runs filter +
 *      comparator; isKeptUpdated reflects the flag.
 *  - getSize()/getSizeAvailable() report real byte usage (records + 16-byte
 *      per-record overhead), not constants.
 *  - version / lastModified / authmode / writable are persisted in the save
 *      file (format v2; v1 files still load).
 *  - listRecordStores() scans the save directory, so stores written by
 *      previous sessions are visible without opening them first.
 *  - Persistence works in ALL builds: libretro (retro save dir) plus a
 *      default per-user directory for the standalone SDL app and the
 *      headless runner (see midp_rms_default_save_path(), NOJME_RMS_DIR
 *      environment override).
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include "debug.h"
#include "debug_macros.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h> /* v36.05: rms_trace_op varargs */
#include <sys/stat.h>

#include "switch/pathguard.h" /* [PATHGUARD] v36.51: обёртки файловых путей
                                * (на не-switch целях — макро-пасс-through) */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#define mkdir_p(path) _mkdir(path)
#else
#include <unistd.h>
#include <pthread.h>
#include <dirent.h>
#define mkdir_p(path) nojme_pg_mkdir("rms-mkdir", (path), 0755)
#endif

#include "midp.h"
#include "jvm.h"
#include "native.h"
#include "heap.h"
#include "opcodes.h"

/* Maximum record stores (handle table is static, records are dynamic) */
#define MAX_RECORD_STORES 256
/* Max size of a single record: 512 KB (MIDP spec minimum is 32 KB;
 * real devices shipped 64 KB-1 MB, we allow a generous 512 KB) */
#define MAX_RECORD_SIZE (512 * 1024)
/* Per-record storage overhead accounted in getSize() (matches typical
 * MIDP implementations that include bookkeeping in the reported size) */
#define RMS_RECORD_OVERHEAD 16
/* Max listeners per store */
#define RMS_MAX_LISTENERS 8
/* Max live kept-updated enumerations tracked */
#define RMS_MAX_ENUMS 64

/* Record structure */
typedef struct {
    int id;                /* monotonic record ID, > 0 */
    uint8_t* data;         /* owned; NULL only possible when size==0 */
    int size;
    bool valid;
} Record;

/* Record store structure */
typedef struct {
    char* name;
    Record* records;       /* dynamic array, indexed by slot not by id */
    int record_count;      /* entries used in records[] */
    int record_capacity;
    int next_id;           /* monotonic: next ID handed out by addRecord */
    bool open;
    int ref_count;
    int version;           /* Incremented on every add/set/delete operation */
    jlong last_modified;   /* Timestamp of last mutation (ms since epoch) */
    int authmode;          /* RecordStore.AUTHMODE_PRIVATE(0) / AUTHMODE_ANY(1) */
    bool writable;
    JavaObject* listeners[RMS_MAX_LISTENERS]; /* GC-rooted */
    /* v35.09 DEFERRED SAVE: a full store rewrite costs fopen+fwrite+fsync+
     * rename = 100-800 ms on SD (v41 PERF-DIAG note below). Games writing a
     * series of records in one go (asia rally's exit save writes longs one
     * by one) paid that PER RECORD. Mutations now only mark the store dirty;
     * the actual disk write happens at close/unload, or after
     * RMS_DEFERRED_SAVE_MAX_MS of continued dirt (bounds crash loss). */
    bool dirty;
    jlong dirty_since;
} NativeRecordStore;

/* Forward declaration */
static jlong rms_current_time_ms(void);

/* Record stores (static table - handles are indexes and must stay stable) */
static NativeRecordStore record_stores[MAX_RECORD_STORES];
static int record_store_count = 0;
static bool rms_initialized = false;

/* v36.05 [RMS]: throttled forced-trace of the RMS operations that matter
 * for the "game reads its save on relaunch" class of field bugs. The
 * v36.04 trace (Doom RPG [Rus], 2nd launch) proved a game can die INSIDE
 * its save-parsing path (c.toString() NPE in the catch handler) with the
 * log unable to say WHICH store it opened or WHAT it read. These lines go
 * through sw_trace_force = they reach log.txt on the Switch even with the
 * logging toggle off. Throttled: the first 24 ops per process verbatim
 * (a save read is always early), then at most one line per 10 s. */
static void rms_trace_op(const char* fmt, ...) {
    static int trace_n = 0;
    static long long trace_last_s = -100;
    int allow;
    if (trace_n < 24) {
        allow = 1;
    } else {
        time_t now_s = time(NULL);
        allow = ((long long)now_s - trace_last_s) >= 10;
        if (allow) trace_last_s = (long long)now_s;
    }
    if (!allow) return;
    if (trace_n < 24) trace_n++;
    char body[176];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    char out[208];
    snprintf(out, sizeof(out), "[RMS] %s", body);
    extern void sw_trace_force(const char* fmt, ...) __attribute__((weak));
    if (&sw_trace_force && sw_trace_force) {
        sw_trace_force("%s", out);
    } else {
        fprintf(stderr, "%s\n", out);
    }
}

/* ============================================
 * Global RMS lock (recursive)
 * ============================================ */

#ifdef _WIN32
static CRITICAL_SECTION g_rms_cs;
static volatile LONG g_rms_cs_init = 0;
static INIT_ONCE g_rms_cs_once = INIT_ONCE_STATIC_INIT;

/* v36.34: rms_mono_ms is used by the shared flush-timing path
 * (rms_flush_dirty_stores, [RMS-SLOW]); on Windows it must exist too —
 * the v36.31 timing edit referenced the POSIX-only helper and broke the
 * MinGW build (implicit declaration). */
static long long rms_mono_ms(void) {
    return (long long)GetTickCount64();
}

static BOOL CALLBACK rms_cs_init_once(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_rms_cs);
    return TRUE;
}
static void rms_lock(void) {
    InitOnceExecuteOnce(&g_rms_cs_once, rms_cs_init_once, NULL, NULL);
    EnterCriticalSection(&g_rms_cs);
}
static void rms_unlock(void) {
    LeaveCriticalSection(&g_rms_cs);
}
#else
static pthread_mutex_t g_rms_mutex;
/* v35.00 KILL-ONCE: pthread_once removed from the RMS acquire path, same
 * rationale as mobile3d.c (the devkitA64 pthread_once blocked silently
 * forever in the v34.99 field trace; if its implementation uses a global
 * internal lock, ANY once call in the process can wedge ALL of them).
 * CAS-based one-shot, bounded + loud wait. State: 0=uninit 1=init 2=ready. */
static volatile int g_rms_lock_state = 0;

static long long rms_mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void rms_mutex_init_once(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_rms_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}
static void rms_ensure_lock_init(void) {
    if (__atomic_load_n(&g_rms_lock_state, __ATOMIC_ACQUIRE) == 2) return;
    long long t0 = rms_mono_ms();
    if (__sync_bool_compare_and_swap(&g_rms_lock_state, 0, 1)) {
        rms_mutex_init_once();
        __atomic_store_n(&g_rms_lock_state, 2, __ATOMIC_RELEASE);
        return;
    }
    while (__atomic_load_n(&g_rms_lock_state, __ATOMIC_ACQUIRE) != 2) {
        long long w = rms_mono_ms() - t0;
        if (w > 3000) {
            LOG_SAFE("[RMS-INIT-STALL] rms lock not ready after %lld ms (initializer wedged) — taking over\n", w);
            rms_mutex_init_once();
            __atomic_store_n(&g_rms_lock_state, 2, __ATOMIC_RELEASE);
            break;
        }
        {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 500000 };
            nanosleep(&ts, NULL);
        }
    }
}
static void rms_lock(void) {
    rms_ensure_lock_init();
    pthread_mutex_lock(&g_rms_mutex);
}
static void rms_unlock(void) {
    pthread_mutex_unlock(&g_rms_mutex);
}
#endif

/* ============================================
 * RMS Disk Persistence
 * ============================================ */

/* RMS save directory and game name */
static char rms_save_dir[512] = {0};   /* e.g. "/home/user/.config/retroarch/saves/J2ME" */
static char rms_game_name[256] = {0};  /* e.g. "game" (from game.jar filename) */

/* Magic bytes and version for RMS save file format.
 * v1: magic, ver=1, next_id, count, records{id,size,data}...
 * v2: magic, ver=2, next_id, count, store_version, last_modified(u64),
 *     authmode, writable, records{id,size,data}... */
#define RMS_FILE_MAGIC  0x524D5300  /* "RMS\0" */
#define RMS_FILE_VERSION 2

/* v36.52 [RMS-DIR-MEMO]: mkdir на СУЩЕСТВУЮЩЕМ каталоге возвращает
 * EEXIST(-1), и старый rms_ensure_dir принимал это за «надо строить
 * родителей» — на каждом сохранении гулял каскад из ~10 mkdir по
 * всей цепочке (полевой лог yetifix8: 7556-7609, повторяется перед
 * каждым rms-save, ×2 стора). Теперь: (а) мемо каталогов, подтверждён-
 * ных в этой сессии; (б) stat-фастпас — каталог существует, каскад
 * не нужен. При первом запуске (каталогов нет) поведение прежнее. */
#define RMS_DIR_MEMO 8
static char rms_dir_memo[RMS_DIR_MEMO][512];
static int rms_dir_memo_n = 0;
static int rms_dir_memo_next = 0;

static int rms_dir_known(const char* dir) {
    for (int i = 0; i < rms_dir_memo_n; i++)
        if (rms_dir_memo[i][0] && strcmp(rms_dir_memo[i], dir) == 0)
            return 1;
    return 0;
}

static void rms_dir_remember(const char* dir) {
    if (!dir || !dir[0] || rms_dir_known(dir)) return;
    if (strlen(dir) >= sizeof(rms_dir_memo[0])) return;
    int slot;
    if (rms_dir_memo_n < RMS_DIR_MEMO) {
        slot = rms_dir_memo_n++;
    } else { /* кольцевое вытеснение */
        slot = rms_dir_memo_next;
        rms_dir_memo_next = (rms_dir_memo_next + 1) % RMS_DIR_MEMO;
    }
    strcpy(rms_dir_memo[slot], dir);
}

/* Helper: create directory tree recursively */
static void rms_ensure_dir(const char* dir) {
    if (!dir || !dir[0]) return;

    if (rms_dir_known(dir)) return; /* [RMS-DIR-MEMO] v36.52 */

    /* Каталог уже существует? — каскад mkdir не нужен */
    struct stat st;
    if (nojme_pg_stat("rms-dir-stat", dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        rms_dir_remember(dir);
        return;
    }

    /* Try creating the directory - if it exists, that's fine */
    if (mkdir_p(dir) == 0) {
        rms_dir_remember(dir);
        return;
    }

    /* If it failed, try creating parent directories */
    char tmp[512];
    strncpy(tmp, dir, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    /* Find last separator and try creating parent */
    for (int i = strlen(tmp) - 1; i > 0; i--) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            tmp[i] = '\0';
            rms_ensure_dir(tmp);
            break;
        }
    }

    /* Try again */
    if (mkdir_p(dir) == 0 ||
        (nojme_pg_stat("rms-dir-stat2", dir, &st) == 0 && S_ISDIR(st.st_mode)))
        rms_dir_remember(dir);
}

/* Build full path for a record store file: <save_dir>/<game_name>/<store_name>.rms */
/* v18 (audit M-9): sanitize the STORE NAME (never the assembled path!) —
 * a hostile name like "../../etc/cron.d/evil" must not escape the save
 * directory. Path separators, drive letters and parent components are
 * replaced; the result is a plain flat filename. */
static void rms_sanitize_store_name(const char* in, char* out, int out_size) {
    int o = 0;
    if (out_size > 0) out[0] = '\0';
    if (!in) return;
    for (int i = 0; in[i] != '\0' && o < out_size - 1; i++) {
        char c = in[i];
        if (c == '/' || c == '\\' || c == ':' || c == '.') {
            /* keep single inner dots of ordinary names like "save.1",
             * but never allow leading dots ("..") to survive */
            if (c == '.' && (i == 0 || in[i - 1] == '.')) {
                c = '_';
            }
        }
        if ((unsigned char)c < 0x20) c = '_';
        out[o++] = c;
    }
    out[o] = '\0';
    if (out[0] == '\0') {
        snprintf(out, out_size, "unnamed");
    }
}

/* FIX (audit R-2, v18): portable rename used for atomic store persistence */
/* v36.52 [RMS-REPLACE]: HOS/LibHac/Ryujinx IFileSystem.RenameFile НЕ
 * заменяет существующего получателя (POSIX rename на Linux-хосте —
 * заменяет, поэтому все host-прогоны это маскировали). Полевой лог
 * yetifix8: ВСЕ 6 rms-rename rc=-1 → tmp удалялся → каждая запись
 * RMS терялась при сохранении поверх существующего .rms (в сессии
 * это маскирует RAM-кэш стора — «RAM reuse»). Стратегия замены:
 *   1) rename как есть (POSIX — по-прежнему атомарно);
 *   2) при отказе и существующем получателе: старый ДБ уводим в
 *      <имя>.bak, повторяем rename; при успехе .bak удаляем, при
 *      отказе — откатываем .bak на место (старый ДБ не теряем);
 *   3) последний рубеж: поэлементное копирование (не атомарно,
 *      но данные переживают даже двойной отказ rename).
 * Проверка на хосте: NOJME_PG_RENAME_EEXIST=1 (симуляция семантики
 * HOS в приборе) — фоллбек проходит end-to-end на Linux. */
static int rms_copy_file_content(const char* from, const char* to) {
    FILE* src = nojme_pg_fopen("rms-copy-src", from, "rb");
    if (!src) return -1;
    FILE* dst = nojme_pg_fopen("rms-copy-dst", to, "wb");
    if (!dst) { fclose(src); return -1; }
    char buf[4096];
    size_t n;
    int ok = 0;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) { ok = -1; break; }
    }
    if (ferror(src)) ok = -1;
    fflush(dst);
#ifdef _WIN32
    _commit(_fileno(dst));
#else
    fsync(fileno(dst));
#endif
    fclose(src);
    fclose(dst);
    return ok;
}

static int rms_portable_rename(const char* old_path, const char* new_path) {
    int r = nojme_pg_rename("rms-rename", old_path, new_path);
    if (r == 0) return 0;

    struct stat st;
    int old_ok = (stat(old_path, &st) == 0);
    int new_exists = (stat(new_path, &st) == 0);
    if (!old_ok) return r; /* источника нет — получателя не трогаем */

    if (new_exists) {
        /* получатель есть (семантика HOS): старый ДБ — в резерв */
        char bak[1100];
        snprintf(bak, sizeof(bak), "%s.bak", new_path);
        nojme_pg_rename("rms-bak", new_path, bak); /* [PATHGUARD] v36.52 */
        r = nojme_pg_rename("rms-rename2", old_path, new_path);
        if (r == 0) {
            nojme_pg_remove("rms-delbak", bak); /* [PATHGUARD] v36.52 */
            return 0;
        }
        /* откат: старый ДБ возвращается на место */
        nojme_pg_rename("rms-unbak", bak, new_path);
    } else {
        r = nojme_pg_rename("rms-rename2", old_path, new_path);
        if (r == 0) return 0;
    }

    /* последний рубеж: копия содержимым */
    if (rms_copy_file_content(old_path, new_path) == 0) {
        nojme_pg_remove("rms-clean-tmp", old_path); /* [PATHGUARD] v36.52 */
        return 0;
    }
    return r;
}

static void rms_build_filepath(const char* store_name, char* out, int out_size) {
    if (out_size > 0) out[0] = '\0';
    if (!rms_save_dir[0] || !rms_game_name[0]) return;

    /* v18 (audit M-9): sanitize the name BEFORE assembling the path */
    char safe_name[256];
    rms_sanitize_store_name(store_name, safe_name, sizeof(safe_name));

    /* v36.32 [RMS-PATH-FIX]: assemble the path from ALREADY-sanitized
     * components and keep the directory verbatim. The old final whole-path
     * sweep rewrote ':' -> '_' INSIDE the assembled string — on the Switch
     * the settings base is "sdmc:/switch/j2me/saves", so fopen silently
     * targeted the nonexistent "sdmc_/switch/..." tree while mkdir (via
     * rms_build_dirpath) created "sdmc:/switch/...". Result: every store
     * write failed to open its temp file and ALL RMS saves were lost since
     * v34.92 (the "sdmc_" prefix in the field logs is this bug's
     * fingerprint). rms_sanitize_store_name already strips '/', '\\', ':',
     * control chars and leading dots from the FILE name; game_name is
     * sanitized once at set time; save_dir is the user's configured base
     * ("sdmc:..." on HOS) and must survive untouched — libnx stdio accepts
     * the "sdmc:" prefix (same convention as mkdir/log paths in main.c). */
    snprintf(out, out_size, "%s/%s/%s.rms", rms_save_dir, rms_game_name, safe_name);
}

/* Build the per-game directory path: <save_dir>/<game_name> */
static void rms_build_dirpath(char* out, int out_size) {
    if (out_size > 0) out[0] = '\0';
    if (!rms_save_dir[0] || !rms_game_name[0]) return;
    snprintf(out, out_size, "%s/%s", rms_save_dir, rms_game_name);
}

/* Write a 32-bit little-endian value */
static void rms_write_u32(FILE* f, uint32_t val) {
    uint8_t buf[4] = {
        (uint8_t)(val & 0xFF),
        (uint8_t)((val >> 8) & 0xFF),
        (uint8_t)((val >> 16) & 0xFF),
        (uint8_t)((val >> 24) & 0xFF)
    };
    fwrite(buf, 1, 4, f);
}

/* Read a 32-bit little-endian value */
static uint32_t rms_read_u32(FILE* f) {
    uint8_t buf[4];
    if (fread(buf, 1, 4, f) != 4) return 0;
    return buf[0] | (buf[1] << 8) | (buf[2] << 16) | ((uint32_t)(buf[3]) << 24);
}

/* Write a 64-bit little-endian value */
static void rms_write_u64(FILE* f, uint64_t val) {
    rms_write_u32(f, (uint32_t)(val & 0xFFFFFFFFu));
    rms_write_u32(f, (uint32_t)(val >> 32));
}

/* Read a 64-bit little-endian value */
static uint64_t rms_read_u64(FILE* f) {
    uint64_t lo = rms_read_u32(f);
    uint64_t hi = rms_read_u32(f);
    return lo | (hi << 32);
}

/* Save a single record store to disk */
static void rms_save_store_to_disk(int handle) {
    if (handle < 0 || handle >= MAX_RECORD_STORES) return;
    if (!rms_save_dir[0] || !rms_game_name[0]) return;

    NativeRecordStore* store = &record_stores[handle];
    if (!store->name) return;

    /* v41 PERF-DIAG: NOJME_RMSSTAMP=1 — one line per disk save (fopen+
     * fwrite+fsync+rename can cost 100-800ms on SD storage; suspected
     * source of periodic in-game stalls on libretro). */
    {
        static int s_on = -1;
        if (s_on < 0) {
            const char* e = getenv("NOJME_RMSSTAMP");
            s_on = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        if (s_on) {
            char line[288];
            int ln = snprintf(line, sizeof(line), "[RMSSTAMP] save store='%s' records=%d\n",
                             store->name, store->record_count);
            if (ln > 0) fwrite(line, 1, (size_t)ln, stderr);
        }
    }

    /* Build file path */
    char filepath[1024];
    rms_build_filepath(store->name, filepath, sizeof(filepath));
    if (!filepath[0]) return;

    /* v36.31 HOST-REPRO KNOB: NOJME_RMS_DELAY_MS=N — sleep N ms inside the
     * disk save (before the atomic rename, holding the RMS lock). Simulates
     * the device's slow-SD flush (100-800 ms per store measured in the
     * field) so the host sandbox can reproduce exit-race windows where the
     * v36.30 EXIT-FENCE 2.5 s cap is blown by a long destroyApp RMS write
     * and the VM runners resume mid-destroyApp. Default 0 (no sleep). */
    {
        static int s_delay_ms = -1;
        if (s_delay_ms < 0) {
            const char* e = getenv("NOJME_RMS_DELAY_MS");
            s_delay_ms = (e && e[0]) ? atoi(e) : 0;
            if (s_delay_ms < 0) s_delay_ms = 0;
        }
        if (s_delay_ms > 0) {
            RMS_DEBUG("RMS DELAY: simulating slow SD flush %d ms (store '%s')",
                      s_delay_ms, store->name);
            struct timespec ts = { .tv_sec = s_delay_ms / 1000,
                                   .tv_nsec = (long)(s_delay_ms % 1000) * 1000000L };
            nanosleep(&ts, NULL);
        }
    }

    /* Ensure directory exists */
    char dirpath[1024];
    rms_build_dirpath(dirpath, sizeof(dirpath));
    nojme_pg_watch("rms:dirpath", dirpath, sizeof(dirpath)); /* [PATHGUARD] v36.51 */
    rms_ensure_dir(dirpath);

    /* Open file for writing — v18 (audit R-2): write to a TEMPORARY file,
     * flush all the way down, then rename atomically. Opening the real file
     * with "wb" truncates it immediately, so a crash or power loss mid-write
     * destroyed ALL saves of the store. */
    /* v34.74: +16 headroom so appending ".tmp" can never truncate
     * (-Wformat-truncation); the rename pair stays within one dir entry. */
    char tmppath[1040];
    snprintf(tmppath, sizeof(tmppath), "%s.tmp", filepath);
    nojme_pg_watch("rms:tmppath", tmppath, sizeof(tmppath)); /* [PATHGUARD] v36.51 */
    FILE* f = nojme_pg_fopen("rms-save", tmppath, "wb");
    if (!f) {
        RMS_DEBUG("RMS SAVE: Failed to open temp file for writing: %s", tmppath);
        return;
    }

    /* Write header (v2) */
    rms_write_u32(f, RMS_FILE_MAGIC);
    rms_write_u32(f, RMS_FILE_VERSION);
    rms_write_u32(f, (uint32_t)store->next_id);

    /* Count records */
    uint32_t record_count = 0;
    for (int i = 0; i < store->record_count; i++) {
        if (store->records[i].valid) record_count++;
    }
    rms_write_u32(f, record_count);

    /* v2 metadata: version counter, lastModified, authmode, writable */
    rms_write_u32(f, (uint32_t)store->version);
    rms_write_u64(f, (uint64_t)store->last_modified);
    rms_write_u32(f, (uint32_t)store->authmode);
    rms_write_u32(f, store->writable ? 1u : 0u);

    /* Write each record */
    for (int i = 0; i < store->record_count; i++) {
        if (!store->records[i].valid) continue;

        rms_write_u32(f, (uint32_t)store->records[i].id);
        rms_write_u32(f, (uint32_t)store->records[i].size);

        if (store->records[i].size > 0 && store->records[i].data) {
            fwrite(store->records[i].data, 1, store->records[i].size, f);
        }
    }

    /* v18 (audit R-2): flush everything to disk, then swap atomically */
    fflush(f);
#if defined(_WIN32)
    _commit(_fileno(f));
#else
    fsync(fileno(f));
#endif
    fclose(f);

    if (rms_portable_rename(tmppath, filepath) != 0) {
        RMS_DEBUG("RMS SAVE: atomic rename failed (%s -> %s)", tmppath, filepath);
        nojme_pg_remove("rms-clean-tmp", tmppath); /* [PATHGUARD] v36.51 */
        return;
    }

    /* v35.09: this write is the flush of a deferred-save series */
    store->dirty = false;
    store->dirty_since = 0;

    RMS_DEBUG("RMS SAVE: Saved store '%s' with %u records to %s",
            store->name, record_count, filepath);
}

/* v35.09 DEFERRED SAVE: mark instead of write. The disk write happens at
 * close/unload or once the store has been dirty for
 * RMS_DEFERRED_SAVE_MAX_MS (whichever comes first). Bursts of mutations
 * (games saving many records in a loop — asia rally's exit writes longs
 * one by one) collapse into ONE full-file rewrite+fsync instead of one per
 * record; at most RMS_DEFERRED_SAVE_MAX_MS of record changes are at risk
 * on a power loss, and the durable temp+fsync+rename per actual write is
 * untouched (v18 audit R-2 semantics preserved). */
#define RMS_DEFERRED_SAVE_MAX_MS 3000
static void rms_mark_store_dirty(int handle) {
    if (handle < 0 || handle >= MAX_RECORD_STORES) return;
    NativeRecordStore* store = &record_stores[handle];
    if (!store->name) return;
    jlong now = rms_current_time_ms();
    if (!store->dirty) {
        store->dirty = true;
        store->dirty_since = now;
    }
    if (now - store->dirty_since >= RMS_DEFERRED_SAVE_MAX_MS) {
        /* Continued dirt past the bound: flush now (the cost amortizes to
         * at most one write per window for constantly-writing games). */
        rms_save_store_to_disk(handle);
    }
}

/* Flush every dirty store (close / game unload / session teardown).
 * v36.31: each store flushes UNDER the RMS lock now — the sweep used to
 * walk and write unlocked while a VM runner could concurrently mutate
 * the same store (addRecord in destroyApp on one thread vs the teardown
 * flush on another). The mutex is recursive, so nested save paths that
 * already hold it are unaffected. Each save is timed: a flush slower
 * than 300 ms gets a permanent log line (the field "slow SD" evidence —
 * that latency is what stretches destroyApp past the fence cap). */
static void rms_flush_dirty_stores(void) {
    for (int i = 0; i < MAX_RECORD_STORES; i++) {
        if (record_stores[i].name && record_stores[i].dirty) {
            long long t0 = rms_mono_ms();
            rms_lock();
            /* Re-check under the lock: the runner may have flushed it
             * between our unlocked check and the lock acquisition. */
            if (record_stores[i].name && record_stores[i].dirty) {
                rms_save_store_to_disk(i);
            }
            rms_unlock();
            long long dt = rms_mono_ms() - t0;
            if (dt > 300) {
                LOG_SAFE("[RMS-SLOW] dirty flush of store %d took %lld ms (slow SD?)\n",
                         i, dt);
            }
        }
    }
}

/* Load a single record store from disk (accepts v1 and v2 formats) */
static bool rms_load_store_from_disk(NativeRecordStore* store) {
    if (!store || !store->name) return false;
    if (!rms_save_dir[0] || !rms_game_name[0]) {
        /* v36.28 DIAG (sandbox RMS persistence debugging): the launcher sets
         * both before the midlet runs — a miss here means an init-order bug. */
        rms_trace_op("load '%s' SKIP: save_dir/game_name empty (dir='%s' game='%s')",
                     store->name, rms_save_dir, rms_game_name);
        return false;
    }

    /* Build file path */
    char filepath[1024];
    rms_build_filepath(store->name, filepath, sizeof(filepath));
    if (!filepath[0]) return false;
    nojme_pg_watch("rms:loadpath", filepath, sizeof(filepath)); /* [PATHGUARD] v36.51 */

    /* Open file for reading */
    FILE* f = nojme_pg_fopen("rms-load", filepath, "rb");
    if (!f) {
        RMS_DEBUG("RMS LOAD: No saved file for store '%s' at %s", store->name, filepath);
        rms_trace_op("load '%s': no file at %s", store->name, filepath);
        return false;
    }

    /* Read and verify header */
    uint32_t magic = rms_read_u32(f);
    if (magic != RMS_FILE_MAGIC) {
        RMS_DEBUG("RMS LOAD: Bad magic in %s (got 0x%08X)", filepath, magic);
        rms_trace_op("load '%s': BAD MAGIC 0x%08X in %s", store->name, magic, filepath);
        fclose(f);
        return false;
    }

    uint32_t version = rms_read_u32(f);
    if (version < 1 || version > RMS_FILE_VERSION) {
        RMS_DEBUG("RMS LOAD: Unsupported version %u in %s", version, filepath);
        rms_trace_op("load '%s': BAD VERSION %u in %s", store->name, version, filepath);
        fclose(f);
        return false;
    }

    uint32_t next_id = rms_read_u32(f);
    uint32_t record_count = rms_read_u32(f);

    /* v2 metadata (defaults keep current values for v1 files) */
    if (version >= 2) {
        store->version = (int)rms_read_u32(f);
        store->last_modified = (jlong)rms_read_u64(f);
        store->authmode = (int)rms_read_u32(f);
        store->writable = rms_read_u32(f) != 0;
    }

    RMS_DEBUG("RMS LOAD: Store '%s': next_id=%u, records=%u (fmt v%u)",
            store->name, next_id, record_count, version);

    /* Read records */
    uint32_t loaded = 0;
    for (uint32_t r = 0; r < record_count; r++) {
        uint32_t rec_id = rms_read_u32(f);
        uint32_t rec_size = rms_read_u32(f);

        if (rec_id < 1 || rec_size > MAX_RECORD_SIZE) {
            RMS_DEBUG("RMS LOAD: Skipping bad record id=%u size=%u", rec_id, rec_size);
            /* Try to skip the data */
            if (rec_size <= MAX_RECORD_SIZE) {
                fseek(f, rec_size, SEEK_CUR);
            }
            continue;
        }

        /* Allocate and read data */
        uint8_t* data = NULL;
        if (rec_size > 0) {
            data = (uint8_t*)malloc(rec_size);
            if (!data) {
                fseek(f, rec_size, SEEK_CUR);
                continue;
            }
            if (fread(data, 1, rec_size, f) != rec_size) {
                free(data);
                continue;
            }
        }

        /* Replace existing record with the same id (if any) */
        Record* existing = NULL;
        for (int i = 0; i < store->record_count; i++) {
            if (store->records[i].valid && store->records[i].id == (int)rec_id) {
                existing = &store->records[i];
                break;
            }
        }

        if (existing) {
            free(existing->data);
            existing->data = data;
            existing->size = (int)rec_size;
        } else {
            /* Append a new slot */
            if (store->record_count >= store->record_capacity) {
                int new_cap = store->record_capacity ? store->record_capacity * 2 : 64;
                Record* nr = (Record*)realloc(store->records, new_cap * sizeof(Record));
                if (!nr) {
                    free(data);
                    continue;
                }
                store->records = nr;
                store->record_capacity = new_cap;
            }
            Record* rec = &store->records[store->record_count++];
            rec->id = (int)rec_id;
            rec->data = data;
            rec->size = (int)rec_size;
            rec->valid = true;
        }

        loaded++;
    }

    fclose(f);

    /* Monotonic next_id: always keep the maximum seen */
    if ((int)next_id > store->next_id) {
        store->next_id = (int)next_id;
    }

    RMS_DEBUG("RMS LOAD: Loaded %u records for store '%s'", loaded, store->name);
    return loaded > 0;
}

/* Delete a record store file from disk */
static void rms_delete_store_file(const char* name) {
    if (!name || !rms_save_dir[0] || !rms_game_name[0]) return;

    char filepath[1024];
    rms_build_filepath(name, filepath, sizeof(filepath));
    if (filepath[0]) {
        nojme_pg_remove("rms-del", filepath); /* [PATHGUARD] v36.51 */
        RMS_DEBUG("RMS: Deleted file %s", filepath);
    }
}

/* ============================================
 * Save directory enumeration (listRecordStores)
 * ============================================ */

/* Callback invoked for each "*.rms" file found in the save directory */
typedef void (*rms_dir_cb)(const char* store_name, void* ud);

static void rms_scan_save_dir(rms_dir_cb cb, void* ud) {
    char dirpath[1024];
    rms_build_dirpath(dirpath, sizeof(dirpath));
    if (!dirpath[0]) return;

#ifdef _WIN32
    char pattern[1100];
    snprintf(pattern, sizeof(pattern), "%s\\*.rms", dirpath);
    WIN32_FIND_DATAA find_data;
    HANDLE h = FindFirstFileA(pattern, &find_data);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        size_t len = strlen(find_data.cFileName);
        if (len > 4 && strcasecmp(find_data.cFileName + len - 4, ".rms") == 0) {
            char name[256];
            size_t copy = len - 4;
            if (copy >= sizeof(name)) copy = sizeof(name) - 1;
            memcpy(name, find_data.cFileName, copy);
            name[copy] = '\0';
            cb(name, ud);
        }
    } while (FindNextFileA(h, &find_data));
    FindClose(h);
#else
    DIR* dir = (DIR*)nojme_pg_opendir("rms-list", dirpath);
    if (!dir) return;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len > 4 && strcasecmp(entry->d_name + len - 4, ".rms") == 0) {
            char name[256];
            size_t copy = len - 4;
            if (copy >= sizeof(name)) copy = sizeof(name) - 1;
            memcpy(name, entry->d_name, copy);
            name[copy] = '\0';
            cb(name, ud);
        }
    }
    closedir(dir);
#endif
}

/* Default save directory for builds that have no libretro frontend:
 * - $NOJME_RMS_DIR if set (wins over everything)
 * - Windows: %APPDATA%\nojme\rms, fallback %USERPROFILE%\nojme\rms, fallback "rms"
 * - POSIX: $XDG_DATA_HOME/nojme/rms, fallback $HOME/.local/share/nojme/rms, fallback "rms"
 */
void midp_rms_default_save_path(const char* game_name) {
    rms_lock();

    const char* env_dir = getenv("NOJME_RMS_DIR");
    if (env_dir && env_dir[0]) {
        strncpy(rms_save_dir, env_dir, sizeof(rms_save_dir) - 1);
        rms_save_dir[sizeof(rms_save_dir) - 1] = '\0';
    } else {
#ifdef _WIN32
        const char* appdata = getenv("APPDATA");
        if (appdata && appdata[0]) {
            snprintf(rms_save_dir, sizeof(rms_save_dir), "%s\\nojme\\rms", appdata);
        } else {
            const char* up = getenv("USERPROFILE");
            if (up && up[0]) {
                snprintf(rms_save_dir, sizeof(rms_save_dir), "%s\\nojme\\rms", up);
            } else {
                snprintf(rms_save_dir, sizeof(rms_save_dir), "rms");
            }
        }
#else
        const char* xdg = getenv("XDG_DATA_HOME");
        if (xdg && xdg[0]) {
            snprintf(rms_save_dir, sizeof(rms_save_dir), "%s/nojme/rms", xdg);
        } else {
            const char* home = getenv("HOME");
            if (home && home[0]) {
                snprintf(rms_save_dir, sizeof(rms_save_dir), "%s/.local/share/nojme/rms", home);
            } else {
                snprintf(rms_save_dir, sizeof(rms_save_dir), "rms");
            }
        }
#endif
    }

    if (game_name && game_name[0]) {
        /* v36.32 [RMS-PATH-FIX]: sanitize game_name ONCE at set time so the
         * mkdir path (rms_build_dirpath) and the fopen path
         * (rms_build_filepath) always agree on the same bytes */
        rms_sanitize_store_name(game_name, rms_game_name, sizeof(rms_game_name));
    }

    /* Create <save_dir>/<game_name> eagerly so listRecordStores sees it */
    char dirpath[1024];
    rms_build_dirpath(dirpath, sizeof(dirpath));
    if (dirpath[0]) rms_ensure_dir(dirpath);

    RMS_DEBUG("RMS: default save path: dir='%s', game='%s'",
            rms_save_dir, rms_game_name);
    rms_unlock();
}

/* Set the save directory and game name (called from libretro core) */
void midp_rms_set_save_path(const char* save_dir, const char* game_name) {
    rms_lock();
    if (save_dir) {
        strncpy(rms_save_dir, save_dir, sizeof(rms_save_dir) - 1);
        rms_save_dir[sizeof(rms_save_dir) - 1] = '\0';
    }
    if (game_name) {
        /* v36.32 [RMS-PATH-FIX]: sanitize here too — midp_rms_set_save_path
         * (libretro) and midp_rms_default_save_path must behave alike */
        rms_sanitize_store_name(game_name, rms_game_name, sizeof(rms_game_name));
    }
    RMS_DEBUG("RMS: save_dir='%s', game_name='%s'", rms_save_dir, rms_game_name);
    rms_unlock();
}

/* Save all record stores to disk (called on game unload) */
void midp_rms_save_all(void) {
    if (!rms_save_dir[0]) return;

    rms_lock();
    int saved = 0;
    for (int i = 0; i < MAX_RECORD_STORES; i++) {
        if (record_stores[i].name) {
            rms_save_store_to_disk(i);
            saved++;
        }
    }
    rms_unlock();

    RMS_DEBUG("RMS: Saved %d stores to disk", saved);
}

/* Convert hex string to bytes */
static int hex_to_bytes(const char* hex, uint8_t* out, int max_len) {
    int len = 0;
    while (hex[0] && hex[1] && len < max_len) {
        if (hex[0] == ' ' || hex[0] == '\t') { hex++; continue; }
        int val = 0;
        for (int i = 0; i < 2; i++) {
            val <<= 4;
            char c = hex[i];
            if (c >= '0' && c <= '9') val |= (c - '0');
            else if (c >= 'a' && c <= 'f') val |= (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') val |= (c - 'A' + 10);
            else return len;
        }
        out[len++] = val;
        hex += 2;
    }
    return len;
}

/* Pre-loaded records for protection bypass */
typedef struct {
    char store_name[64];
    int record_id;
    uint8_t data[256];
    int data_len;
    bool valid;
} PreloadedRecord;

#define MAX_PRELOADED 32
static PreloadedRecord preloaded_records[MAX_PRELOADED];
static int preloaded_count = 0;

/* Load preloaded records from config file */
static void load_preloaded_records(void) {
    FILE* f = nojme_pg_fopen("rms-bypass", "rms_bypass.conf", "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f) && preloaded_count < MAX_PRELOADED) {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        // Parse: store_name.record_id = hex_data
        char* eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char* store_part = line;
        char* hex_data = eq + 1;

        // Trim whitespace
        while (*store_part == ' ' || *store_part == '\t') store_part++;
        while (*hex_data == ' ' || *hex_data == '\t') hex_data++;

        // Find the dot separator
        char* dot = strchr(store_part, '.');
        if (!dot) continue;

        *dot = '\0';
        char* id_str = dot + 1;

        // Remove trailing whitespace/newlines from hex_data
        char* end = hex_data + strlen(hex_data) - 1;
        while (end > hex_data && (*end == '\n' || *end == '\r' || *end == ' ')) *end-- = '\0';

        // Store the record
        PreloadedRecord* pr = &preloaded_records[preloaded_count];
        {
            /* Explicit bounded copy: strncpy()-truncation triggers -Wstringop-truncation */
            size_t n = strlen(store_part);
            if (n >= sizeof(pr->store_name)) n = sizeof(pr->store_name) - 1;
            memcpy(pr->store_name, store_part, n);
            pr->store_name[n] = '\0';
        }
        pr->record_id = atoi(id_str);
        pr->data_len = hex_to_bytes(hex_data, pr->data, sizeof(pr->data));
        pr->valid = true;

        RMS_DEBUG("Preloaded: store='%s' id=%d len=%d",
                pr->store_name, pr->record_id, pr->data_len);

        preloaded_count++;
    }

    fclose(f);
    RMS_DEBUG("Loaded %d preloaded records from config", preloaded_count);
}

/* Initialize RMS */
static void rms_init(void) {
    if (rms_initialized) return;

    memset(record_stores, 0, sizeof(record_stores));
    load_preloaded_records();
    rms_initialized = true;
}

/* ============================================
 * Exception helper
 * ============================================ */

static void rms_throw(JVM* jvm, JavaThread* thread, const char* cls, const char* msg) {
    jvm_throw_by_name(jvm, cls, msg);
    if (thread) thread->pending_exception = jvm_exception_pending(jvm);
}

/* ============================================
 * Record lookup helpers (dynamic records array)
 * ============================================ */

/* Find the slot of record with the given id, or -1 */
static int rms_find_record_slot(NativeRecordStore* store, int record_id) {
    if (!store || record_id < 1) return -1;
    for (int i = 0; i < store->record_count; i++) {
        if (store->records[i].valid && store->records[i].id == record_id) return i;
    }
    return -1;
}

/* Total data bytes used by valid records (excludes overhead) */
static int rms_used_bytes(NativeRecordStore* store) {
    int used = 0;
    for (int i = 0; i < store->record_count; i++) {
        if (store->records[i].valid) {
            used += store->records[i].size + RMS_RECORD_OVERHEAD;
        }
    }
    return used;
}

/* Number of valid records */
static int rms_num_records(NativeRecordStore* store) {
    int count = 0;
    for (int i = 0; i < store->record_count; i++) {
        if (store->records[i].valid) count++;
    }
    return count;
}

/* Append a new record slot with the given id/data (capacity grows as needed) */
static Record* rms_alloc_record_slot(NativeRecordStore* store, int id, uint8_t* data, int size) {
    if (store->record_count >= store->record_capacity) {
        int new_cap = store->record_capacity ? store->record_capacity * 2 : 64;
        Record* nr = (Record*)realloc(store->records, new_cap * sizeof(Record));
        if (!nr) return NULL;
        store->records = nr;
        store->record_capacity = new_cap;
    }
    Record* rec = &store->records[store->record_count++];
    rec->id = id;
    rec->data = data;
    rec->size = size;
    rec->valid = true;
    return rec;
}

/* Remove a record slot (swap-with-last keeps the array compact) */
static void rms_remove_record_slot(NativeRecordStore* store, int slot) {
    if (slot < 0 || slot >= store->record_count) return;
    free(store->records[slot].data);
    store->records[slot] = store->records[store->record_count - 1];
    store->record_count--;
}

/* ============================================
 * Record listeners (GC-rooted, delivered synchronously)
 * ============================================ */

/* Register a listener; returns slot index or -1 (duplicate or full) */
static int rms_listener_add(JVM* jvm, NativeRecordStore* store, JavaObject* listener) {
    if (!listener) return -1;
    /* Duplicates: same listener registered twice is stored once (spec silent,
     * but firing twice per event is never wanted) */
    for (int i = 0; i < RMS_MAX_LISTENERS; i++) {
        if (store->listeners[i] == listener) return i;
    }
    for (int i = 0; i < RMS_MAX_LISTENERS; i++) {
        if (!store->listeners[i]) {
            store->listeners[i] = listener;
            /* Keep the listener alive while registered: register a GC root
             * pointing at the stable slot inside the static store table. */
            gc_add_root(jvm, (void**)&store->listeners[i]);
            return i;
        }
    }
    RMS_DEBUG("listener table full for store '%s'", store->name ? store->name : "?");
    return -1;
}

static void rms_listener_remove(JVM* jvm, NativeRecordStore* store, JavaObject* listener) {
    if (!listener) return;
    for (int i = 0; i < RMS_MAX_LISTENERS; i++) {
        if (store->listeners[i] == listener) {
            gc_remove_root(jvm, (void**)&store->listeners[i]);
            store->listeners[i] = NULL;
            return;
        }
    }
}

/* Free listener roots when a store is deleted */
static void rms_listener_free_all(JVM* jvm, NativeRecordStore* store) {
    for (int i = 0; i < RMS_MAX_LISTENERS; i++) {
        if (store->listeners[i]) {
            gc_remove_root(jvm, (void**)&store->listeners[i]);
            store->listeners[i] = NULL;
        }
    }
}

/* Deliver recordAdded/recordChanged/recordDeleted to all registered listeners.
 * Called AFTER the mutation is applied and persisted. Re-entrant RMS calls
 * from inside a listener are safe (recursive lock). Dead listeners (collected
 * despite the root, e.g. after GC quirk) are pruned on sight. */
static void rms_notify_listeners(JVM* jvm, NativeRecordStore* store,
                                 JavaObject* rs_obj, const char* method, int record_id) {
    if (!jvm || !store || !rs_obj) return;

    for (int i = 0; i < RMS_MAX_LISTENERS; i++) {
        JavaObject* listener = store->listeners[i];
        if (!listener) continue;

        /* Validate against the heap: a stale pointer would corrupt the VM */
        if (!heap_java_object_valid(listener)) {
            RMS_DEBUG("pruning dead listener slot %d", i);
            gc_remove_root(jvm, (void**)&store->listeners[i]);
            store->listeners[i] = NULL;
            continue;
        }

        JavaValue cb_args[2];
        cb_args[0].ref = rs_obj;
        cb_args[1].i = record_id;
        JavaValue cb_result;
        memset(&cb_result, 0, sizeof(cb_result));

        int rc = jvm_invoke_virtual(jvm, listener, method,
                "(Ljavax/microedition/rms/RecordStore;I)V", cb_args, &cb_result);
        if (rc != 0) {
            RMS_DEBUG("listener callback %s failed (rc=%d)", method, rc);
        }
    }
}

/* ============================================
 * Kept-updated enumeration registry
 * ============================================ */

typedef struct {
    JavaObject* enum_obj;   /* GC-rooted */
    int store_handle;
    bool in_use;
} EnumEntry;

static EnumEntry g_rms_enums[RMS_MAX_ENUMS];

/* v34.74: forward declaration — the kept-updated rebuild (registry update)
 * runs before the id-array builder is defined. */
static JavaArray* rms_build_id_array(JVM* jvm, NativeRecordStore* store,
                                     JavaObject* filter, JavaObject* comparator);

/* Validate an entry: object must still be alive on the heap */
static bool rms_enum_entry_valid(EnumEntry* e) {
    if (!e->in_use || !e->enum_obj) return false;
    if (!heap_java_object_valid(e->enum_obj)) {
        /* Object was collected: drop the entry (and its stale root) */
        e->in_use = false;
        e->enum_obj = NULL;
        return false;
    }
    return true;
}

static void rms_enum_registry_update(JVM* jvm, int store_handle) {
    for (int i = 0; i < RMS_MAX_ENUMS; i++) {
        EnumEntry* e = &g_rms_enums[i];
        if (!e->in_use) continue;
        if (e->store_handle != store_handle) continue;
        if (!rms_enum_entry_valid(e)) continue;

        /* keepUpdated flag lives in fields[5] */
        JavaObject* enum_obj = e->enum_obj;
        if (enum_obj->fields[5].i) {
            /* Rebuild the id list from the current store content.
             * v34.74 (R-8): preserve the ITERATION POSITION — the WTK keeps
             * a kept-updated enumeration pointing at the same "last
             * returned" record across rebuilds instead of restarting it at
             * 0 (iterate-while-modifying loops used to restart from the
             * first record on every mutation = infinite head loop). */
            JavaArray* old_ids = (JavaArray*)enum_obj->fields[0].ref;
            jint old_cursor = enum_obj->fields[1].i;
            jint last_id = -1;
            if (old_ids && old_ids->element_type == T_INT && old_cursor > 0 &&
                old_cursor <= (jint)old_ids->length) {
                jint* od = (jint*)array_data(old_ids);
                last_id = od[old_cursor - 1];
            }

            NativeRecordStore* store = &record_stores[store_handle];
            JavaObject* filter = (JavaObject*)enum_obj->fields[3].ref;
            JavaObject* comparator = (JavaObject*)enum_obj->fields[4].ref;

            JavaArray* ids = rms_build_id_array(jvm, store, filter, comparator);
            if (ids) {
                enum_obj->fields[0].ref = ids;
                /* Reposition: after the last-returned record if it survived,
                 * else clamp the old cursor to the new bounds. */
                jint new_cursor = old_cursor;
                if (last_id >= 0) {
                    jint* nd = (jint*)array_data(ids);
                    jint found = -1;
                    for (jint i = 0; i < (jint)ids->length; i++) {
                        if (nd[i] == last_id) { found = i; break; }
                    }
                    if (found >= 0) new_cursor = found + 1;
                }
                if (new_cursor > (jint)ids->length) new_cursor = (jint)ids->length;
                if (new_cursor < 0) new_cursor = 0;
                enum_obj->fields[1].i = new_cursor;
            }

            RMS_DEBUG("keptUpdated enum for store %d rebuilt: %d records (cursor %d -> %d)",
                      store_handle, ids ? (int)ids->length : -1, (int)old_cursor,
                      (int)enum_obj->fields[1].i);
        }
    }
}

static void rms_enum_registry_add(JVM* jvm, JavaObject* enum_obj, int store_handle, bool keep_updated) {
    /* Prune dead entries first */
    for (int i = 0; i < RMS_MAX_ENUMS; i++) {
        if (g_rms_enums[i].in_use) rms_enum_entry_valid(&g_rms_enums[i]);
    }
    for (int i = 0; i < RMS_MAX_ENUMS; i++) {
        if (!g_rms_enums[i].in_use) {
            g_rms_enums[i].enum_obj = enum_obj;
            g_rms_enums[i].store_handle = store_handle;
            g_rms_enums[i].in_use = true;
            gc_add_root(jvm, (void**)&g_rms_enums[i].enum_obj);
            (void)keep_updated;
            return;
        }
    }
    RMS_DEBUG("enum registry full; enumeration will not auto-update");
}

/* ============================================
 * Store open / close / lookup
 * ============================================ */

/* Open/create record store - returns native handle.
 * Errors: -1 = not found (create==false), -2 = table full / OOM.
 * v34.74 (R-5): created_out reports whether THIS call allocated a fresh
 * store (in-memory miss AND no usable disk file) — the openRecordStore
 * authmode/writable arguments only apply in that case per JSR 118. */
static int rms_open(const char* name, bool create_if_necessary, bool* created_out) {
    rms_init();
    if (created_out) *created_out = false;

    if (!name) return -1;

    /* Find existing store */
    for (int i = 0; i < MAX_RECORD_STORES; i++) {
        if (record_stores[i].name && strcmp(record_stores[i].name, name) == 0) {
            record_stores[i].ref_count++;
            record_stores[i].open = true;
            RMS_DEBUG("Reopened existing store '%s' (handle=%d, refs=%d)",
                    name, i, record_stores[i].ref_count);
            /* v36.05: RAM-reuse path — the store table survives sessions;
             * log what the game is about to see. */
            rms_trace_op("open '%s' create=%d -> RAM reuse h=%d refs=%d records=%d next_id=%d",
                         name, create_if_necessary ? 1 : 0, i,
                         record_stores[i].ref_count,
                         record_stores[i].record_count, record_stores[i].next_id);
            return i;
        }
    }
    /* v58 TEMP-DIAG (gated): open-miss trace, NOJME_LOG=1 enables */
    RMS_DEBUG("open-miss name='%s' create=%d", name, create_if_necessary ? 1 : 0);

    if (!create_if_necessary) {
        /* v58 FIX (persistence bug): open(create=false) never consulted the
         * DISK — only the in-memory table. Stores saved by a previous
         * process were invisible: every boot threw RecordStoreNotFound-
         * Exception on the settings read, so games re-ran their first-boot
         * flow forever (3D Coaster Rush: "Enable sounds?" on every start,
         * each confirm append-addRecord'ing one more record because the
         * game's delete-then-recreate write pattern ran on top of the
         * freshly LOADED store). Try loading the store from disk into a
         * fresh slot; a file with >=1 record is a successful open. */
        int h = -1;
        for (int i = 0; i < MAX_RECORD_STORES; i++) {
            if (!record_stores[i].name) { h = i; break; }
        }
        if (h >= 0) {
            NativeRecordStore* st = &record_stores[h];
            st->name = strdup(name);
            st->records = NULL;
            st->record_count = 0;
            st->record_capacity = 0;
            st->next_id = 1;
            st->open = true;
            st->ref_count = 1;
            st->version = 0;
            st->last_modified = 0;
            st->authmode = 0;
            st->writable = true;
            memset(st->listeners, 0, sizeof(st->listeners));
            if (rms_load_store_from_disk(st)) {
                RMS_DEBUG("Loaded store '%s' from disk on open(create=false) (%d records)",
                        name, st->record_count);
                rms_trace_op("open '%s' create=0 -> DISK h=%d records=%d next_id=%d",
                             name, h, st->record_count, st->next_id);
                return h;
            }
            RMS_DEBUG("disk load FAILED for '%s' (no file or unusable)", name);
            /* No file (or unusable): undo the slot allocation. */
            free(st->name);
            st->name = NULL;
            st->open = false;
            st->ref_count = 0;
        }
        RMS_DEBUG("Store '%s' not found and create=false", name);
        rms_trace_op("open '%s' create=0 -> NOT FOUND", name);
        return -1;  /* RecordStoreNotFoundException */
    }

    /* Find a free slot (slots are never shifted, handles stay stable) */
    int handle = -1;
    for (int i = 0; i < MAX_RECORD_STORES; i++) {
        if (!record_stores[i].name) {
            handle = i;
            break;
        }
    }

    if (handle < 0) {
        RMS_DEBUG("Max stores reached (%d)", MAX_RECORD_STORES);
        return -2;
    }

    NativeRecordStore* store = &record_stores[handle];

    store->name = strdup(name);
    if (!store->name) {
        RMS_DEBUG("Out of memory for store name");
        return -2;
    }

    store->records = NULL;
    store->record_count = 0;
    store->record_capacity = 0;
    store->next_id = 1;
    store->open = true;
    store->ref_count = 1;
    store->version = 0;
    store->last_modified = 0;
    store->authmode = 0;      /* AUTHMODE_PRIVATE */
    store->writable = true;
    memset(store->listeners, 0, sizeof(store->listeners));

    /* Apply preloaded records for this store */
    for (int i = 0; i < preloaded_count; i++) {
        PreloadedRecord* pr = &preloaded_records[i];
        if (strcmp(pr->store_name, name) == 0 && pr->valid) {
            int rid = pr->record_id;
            if (rid > 0) {
                uint8_t* data = NULL;
                if (pr->data_len > 0) {
                    data = (uint8_t*)malloc(pr->data_len);
                    if (data) memcpy(data, pr->data, pr->data_len);
                }
                if (pr->data_len == 0 || data) {
                    Record* rec = rms_alloc_record_slot(store, rid, data, pr->data_len);
                    if (rec) {
                        RMS_DEBUG("Applied preloaded record: store='%s' id=%d len=%d",
                                name, rid, pr->data_len);
                    } else {
                        free(data);
                    }
                }
                /* Monotonic: preloaded ids push next_id forward */
                if (rid >= store->next_id) {
                    store->next_id = rid + 1;
                }
            }
        }
    }

    /* Load record store from disk if saved data exists.
     * Disk data takes priority over preloaded records.
     * v34.74 (R-5): a usable disk file means the store EXISTED — the
     * openRecordStore authmode/writable arguments must not apply. */
    bool loaded_existing = rms_load_store_from_disk(store);
    if (created_out) *created_out = !loaded_existing;

    /* Update count for high-water mark */
    if (handle >= record_store_count) {
        record_store_count = handle + 1;
    }

    RMS_DEBUG("Created new store '%s' with handle %d", name, handle);
    rms_trace_op("open '%s' create=1 -> %s h=%d records=%d next_id=%d",
                 name, loaded_existing ? "DISK" : "NEW", handle,
                 store->record_count, store->next_id);

    return handle;
}

/* Store state codes for get_native_store_ex */
#define RMS_STORE_OK       0
#define RMS_STORE_INVALID  1  /* stale handle / deleted store */
#define RMS_STORE_CLOSED   2  /* exists but not open */

static NativeRecordStore* get_native_store_ex(JavaObject* rs_obj, int* state_out) {
    if (state_out) *state_out = RMS_STORE_INVALID;
    if (!rs_obj) return NULL;
    if (!rs_obj->header.clazz) return NULL;

    JavaClass* clazz = rs_obj->header.clazz;

    for (int i = 0; i < clazz->fields_count; i++) {
        if (clazz->fields[i].name && strcmp(clazz->fields[i].name, "nativeHandle") == 0) {
            int handle = rs_obj->fields[i].i;

            if (handle >= 0 && handle < MAX_RECORD_STORES) {
                NativeRecordStore* store = &record_stores[handle];
                if (store->name && store->open) {
                    if (state_out) *state_out = RMS_STORE_OK;
                    return store;
                } else if (store->name) {
                    if (state_out) *state_out = RMS_STORE_CLOSED;
                    return NULL;
                }
            }
            return NULL;
        }
    }
    return NULL;
}

/* Get native record store from Java RecordStore object (legacy behavior) */
static NativeRecordStore* get_native_store(JavaObject* rs_obj) {
    return get_native_store_ex(rs_obj, NULL);
}

/* Get handle by store pointer */
static int rms_get_handle(NativeRecordStore* store) {
    if (!store) return -1;
    for (int i = 0; i < MAX_RECORD_STORES; i++) {
        if (&record_stores[i] == store) return i;
    }
    return -1;
}

/* Fill the nativeHandle field of a freshly created RecordStore object */
static void rms_set_native_handle(JavaObject* rs_obj, JavaClass* rs_class, int handle) {
    if (!rs_obj || !rs_class) return;
    for (int i = 0; i < rs_class->fields_count; i++) {
        if (rs_class->fields[i].name && strcmp(rs_class->fields[i].name, "nativeHandle") == 0) {
            rs_obj->fields[i].i = handle;
            RMS_DEBUG("Set nativeHandle=%d in field %d", handle, i);
            return;
        }
    }
    RMS_DEBUG("WARNING: nativeHandle field not found in RecordStore class");
}

/* Shared: construct the Java RecordStore object for a handle */
static JavaObject* rms_make_rs_object(JVM* jvm, int handle) {
    JavaClass* rs_class = jvm_load_class(jvm, "javax/microedition/rms/RecordStore");
    if (!rs_class) {
        RMS_DEBUG("Failed to load RecordStore class");
        return NULL;
    }
    JavaObject* rs_obj = jvm_new_object(jvm, rs_class);
    if (!rs_obj) {
        RMS_DEBUG("Failed to create RecordStore object");
        return NULL;
    }
    rms_set_native_handle(rs_obj, rs_class, handle);
    return rs_obj;
}

/*
 * Native methods for RecordStore
 */

/* RecordStore.openRecordStore(String, boolean) - returns RecordStore object */
static JavaValue native_rms_openRecordStore(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)arg_count;
    rms_lock();

    /* STATIC METHOD - no 'this' argument!
     * args[0] - name (String)
     * args[1] - create (boolean)
     */
    JavaString* name_str = (JavaString*)args[0].ref;
    jboolean create = args[1].i;

    if (!name_str) {
        RMS_DEBUG("openRecordStore: name is NULL");
        rms_throw(jvm, thread, "java/lang/NullPointerException", "store name is null");
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    const char* name = string_utf8(jvm, name_str);

    /* MIDP: RecordStoreException if name > 32 chars or empty */
    if (!name || name[0] == '\0' || strlen(name) > 32) {
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreException",
                  "invalid record store name");
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    RMS_DEBUG("openRecordStore: '%s', create=%d", name, create);

    int handle = rms_open(name, create != 0, NULL);

    if (handle < 0) {
        if (handle == -1) {
            rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreNotFoundException", name);
        } else {
            rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreFullException", NULL);
        }
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    JavaObject* rs_obj = rms_make_rs_object(jvm, handle);
    rms_unlock();
    if (!rs_obj) return NATIVE_RETURN_NULL();
    return NATIVE_RETURN_OBJECT(rs_obj);
}

/* RecordStore.closeRecordStore() */
static JavaValue native_rms_closeRecordStore(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)arg_count;
    rms_lock();
    JavaObject* rs_obj = (JavaObject*)args[0].ref;

    if (!rs_obj) {
        rms_throw(jvm, thread, "java/lang/NullPointerException", NULL);
        rms_unlock();
        return NATIVE_RETURN_VOID();
    }

    int state = RMS_STORE_INVALID;
    NativeRecordStore* store = get_native_store_ex(rs_obj, &state);
    if (!store) {
        /* Spec: RecordStoreNotOpenException when not open */
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreNotOpenException", NULL);
        rms_unlock();
        return NATIVE_RETURN_VOID();
    }

    store->ref_count--;
    RMS_DEBUG("closeRecordStore: refs now %d", store->ref_count);

    if (store->ref_count <= 0) {
        store->open = false;
        store->ref_count = 0;
        RMS_DEBUG("Store closed");

        /* Save to disk when last reference is closed */
        int h = rms_get_handle(store);
        if (h >= 0) rms_save_store_to_disk(h);
    }

    rms_unlock();
    return NATIVE_RETURN_VOID();
}

/* RecordStore.addRecord(byte[], int, int) - returns record ID */
static JavaValue native_rms_addRecord(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)arg_count;
    rms_lock();
    JavaObject* rs_obj = (JavaObject*)args[0].ref;
    JavaArray* data = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint length = args[3].i;

    RMS_DEBUG("addRecord: offset=%d, length=%d", offset, length);

    /* v34.74 (R-2, JSR 118): NullPointerException only when data is null
     * AND numBytes > 0 — "addRecord(null, 0, 0)" adds an EMPTY record
     * (canonical first-boot placeholder pattern; WTK/MicroEmu parity). */
    if (!rs_obj || (!data && length > 0)) {
        rms_throw(jvm, thread, "java/lang/NullPointerException",
                  !rs_obj ? "this is null" : "data is null");
        rms_unlock();
        return NATIVE_RETURN_INT(-1);
    }

    /* Spec: ArrayIndexOutOfBoundsException for bad offset/length
     * (data may be null when length==0 — see the R-2 rule above) */
    if (offset < 0 || length < 0 || (data && (offset + length > (jint)data->length))) {
        rms_throw(jvm, thread, "java/lang/ArrayIndexOutOfBoundsException", NULL);
        rms_unlock();
        return NATIVE_RETURN_INT(-1);
    }

    /* v17: zero-length records are legal */
    if (length > MAX_RECORD_SIZE) {
        RMS_DEBUG("addRecord: record too large (%d > %d)", length, MAX_RECORD_SIZE);
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreFullException",
                  "record exceeds maximum size");
        rms_unlock();
        return NATIVE_RETURN_INT(-1);
    }

    int state = RMS_STORE_INVALID;
    NativeRecordStore* store = get_native_store_ex(rs_obj, &state);
    if (!store) {
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreNotOpenException", NULL);
        rms_unlock();
        return NATIVE_RETURN_INT(-1);
    }

    /* MIDP: writing to a read-only store throws RecordStoreException */
    if (!store->writable) {
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreException",
                  "record store is read-only");
        rms_unlock();
        return NATIVE_RETURN_INT(-1);
    }

    /* Size check against real available space */
    int available = MAX_RECORD_SIZE - rms_used_bytes(store);
    if (length > available) {
        RMS_DEBUG("addRecord: store full (need %d, available %d)", length, available);
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreFullException", NULL);
        rms_unlock();
        return NATIVE_RETURN_INT(-1);
    }

    /* Monotonic record ID: never reuse, always hand out next_id */
    int id = store->next_id++;

    /* Allocate and copy data */
    uint8_t* record_data = NULL;
    if (length > 0) {
        record_data = (uint8_t*)malloc(length);
        if (!record_data) {
            store->next_id--; /* id not consumed on failure */
            rms_throw(jvm, thread, "java/lang/OutOfMemoryError", NULL);
            rms_unlock();
            return NATIVE_RETURN_INT(-1);
        }
        uint8_t* src_data = (uint8_t*)array_data(data);
        memcpy(record_data, src_data + offset, length);
    }

    Record* rec = rms_alloc_record_slot(store, id, record_data, length);
    if (!rec) {
        free(record_data);
        store->next_id--;
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreFullException", NULL);
        rms_unlock();
        return NATIVE_RETURN_INT(-1);
    }

    /* Update version and lastModified */
    store->version++;
    store->last_modified = rms_current_time_ms();

    RMS_DEBUG("addRecord: SUCCESS - id=%d, size=%d, next_id=%d",
            id, length, store->next_id);
    /* v36.05: the WRITE side of the save lifecycle (what session 1 left
     * behind is exactly what session 2 must be able to parse). */
    rms_trace_op("addRecord '%s' id=%d size=%d -> records=%d next_id=%d",
                 store->name ? store->name : "?", id, length,
                 store->record_count, store->next_id);

    /* Save to disk */
    int handle = rms_get_handle(store);
    if (handle >= 0) rms_mark_store_dirty(handle);

    /* Deliver recordAdded AFTER the mutation */
    rms_notify_listeners(jvm, store, rs_obj, "recordAdded", id);
    rms_enum_registry_update(jvm, handle);

    rms_unlock();
    return NATIVE_RETURN_INT(id);
}

/* RecordStore.getRecord(int) - returns byte[] */
static JavaValue native_rms_getRecord(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)arg_count;
    rms_lock();
    JavaObject* rs_obj = (JavaObject*)args[0].ref;
    jint record_id = args[1].i;

    RMS_DEBUG("getRecord: record_id=%d", record_id);

    if (!rs_obj) {
        rms_throw(jvm, thread, "java/lang/NullPointerException", NULL);
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    int state = RMS_STORE_INVALID;
    NativeRecordStore* store = get_native_store_ex(rs_obj, &state);
    if (!store) {
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreNotOpenException", NULL);
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    if (record_id < 1) {
        rms_throw(jvm, thread, "javax/microedition/rms/InvalidRecordIDException", NULL);
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    int slot = rms_find_record_slot(store, record_id);
    if (slot < 0) {
        RMS_DEBUG("getRecord: record %d not found", record_id);
        rms_throw(jvm, thread, "javax/microedition/rms/InvalidRecordIDException", NULL);
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    int length = store->records[slot].size;
    /* v36.05: what the game actually reads from its save (id + size —
     * content bugs show up as wrong sizes / holes in the id sequence). */
    rms_trace_op("getRecord '%s' id=%d size=%d (records=%d)",
                 store->name ? store->name : "?", record_id, length,
                 store->record_count);
    uint8_t* data = store->records[slot].data;

    /* Create byte array (zero-length records return byte[0]) */
    JavaArray* byte_array = jvm_new_array(jvm, T_BYTE, (jsize)length, NULL);
    if (!byte_array) {
        rms_throw(jvm, thread, "java/lang/OutOfMemoryError", NULL);
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    if (length > 0 && data) {
        memcpy(array_data(byte_array), data, length);
    }

    rms_unlock();
    return NATIVE_RETURN_OBJECT(byte_array);
}

/* RecordStore.getRecord(int, byte[], int) - returns size */
static JavaValue native_rms_getRecord_array(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)arg_count;
    rms_lock();
    JavaObject* rs_obj = (JavaObject*)args[0].ref;
    jint record_id = args[1].i;
    JavaArray* dest = (JavaArray*)args[2].ref;
    jint offset = args[3].i;

    if (!rs_obj) {
        rms_throw(jvm, thread, "java/lang/NullPointerException", NULL);
        rms_unlock();
        return NATIVE_RETURN_INT(-1);
    }

    /* Spec: NullPointerException if buffer is null */
    if (!dest) {
        rms_throw(jvm, thread, "java/lang/NullPointerException", "buffer is null");
        rms_unlock();
        return NATIVE_RETURN_INT(-1);
    }

    int state = RMS_STORE_INVALID;
    NativeRecordStore* store = get_native_store_ex(rs_obj, &state);
    if (!store) {
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreNotOpenException", NULL);
        rms_unlock();
        return NATIVE_RETURN_INT(-1);
    }

    int slot = rms_find_record_slot(store, record_id);
    if (slot < 0) {
        rms_throw(jvm, thread, "javax/microedition/rms/InvalidRecordIDException", NULL);
        rms_unlock();
        return NATIVE_RETURN_INT(-1);
    }

    int length = store->records[slot].size;

    /* Spec: ArrayIndexOutOfBoundsException when buffer too small */
    if (offset < 0 || offset + length > (jint)dest->length) {
        rms_throw(jvm, thread, "java/lang/ArrayIndexOutOfBoundsException", NULL);
        rms_unlock();
        return NATIVE_RETURN_INT(-1);
    }

    if (length > 0 && store->records[slot].data) {
        memcpy((uint8_t*)array_data(dest) + offset, store->records[slot].data, length);
    }

    rms_unlock();
    return NATIVE_RETURN_INT(length);
}

/* RecordStore.getRecordSize(int) - returns size */
static JavaValue native_rms_getRecordSize(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)arg_count;
    rms_lock();
    JavaObject* rs_obj = (JavaObject*)args[0].ref;
    jint record_id = args[1].i;

    if (!rs_obj) {
        rms_throw(jvm, thread, "java/lang/NullPointerException", NULL);
        rms_unlock();
        return NATIVE_RETURN_INT(-1);
    }

    int state = RMS_STORE_INVALID;
    NativeRecordStore* store = get_native_store_ex(rs_obj, &state);
    if (!store) {
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreNotOpenException", NULL);
        rms_unlock();
        return NATIVE_RETURN_INT(-1);
    }

    int slot = rms_find_record_slot(store, record_id);
    if (slot < 0) {
        rms_throw(jvm, thread, "javax/microedition/rms/InvalidRecordIDException", NULL);
        rms_unlock();
        return NATIVE_RETURN_INT(-1);
    }

    int size = store->records[slot].size;
    rms_unlock();
    return NATIVE_RETURN_INT(size);
}

/* RecordStore.setRecord(int, byte[], int, int) */
static JavaValue native_rms_setRecord(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)arg_count;
    rms_lock();
    JavaObject* rs_obj = (JavaObject*)args[0].ref;
    jint record_id = args[1].i;
    JavaArray* data = (JavaArray*)args[2].ref;
    jint offset = args[3].i;
    jint length = args[4].i;

    if (!rs_obj) {
        rms_throw(jvm, thread, "java/lang/NullPointerException", NULL);
        rms_unlock();
        return NATIVE_RETURN_VOID();
    }

    /* v34.74 (R-3, JSR 118): null data is legal when numBytes == 0 — the
     * record is truncated to empty (same rule as addRecord). */
    if (!data && length > 0) {
        rms_throw(jvm, thread, "java/lang/NullPointerException", "data is null");
        rms_unlock();
        return NATIVE_RETURN_VOID();
    }

    int state = RMS_STORE_INVALID;
    NativeRecordStore* store = get_native_store_ex(rs_obj, &state);
    if (!store) {
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreNotOpenException", NULL);
        rms_unlock();
        return NATIVE_RETURN_VOID();
    }

    if (!store->writable) {
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreException",
                  "record store is read-only");
        rms_unlock();
        return NATIVE_RETURN_VOID();
    }

    int slot = rms_find_record_slot(store, record_id);
    if (slot < 0) {
        rms_throw(jvm, thread, "javax/microedition/rms/InvalidRecordIDException", NULL);
        rms_unlock();
        return NATIVE_RETURN_VOID();
    }

    /* Bounds check (zero-length is legal) */
    if (offset < 0 || length < 0 || (data && (offset + length > (jint)data->length))) {
        rms_throw(jvm, thread, "java/lang/ArrayIndexOutOfBoundsException", NULL);
        rms_unlock();
        return NATIVE_RETURN_VOID();
    }

    /* v34.74 (R-4): the OLD record's bytes are released by this replace, so
     * the quota check must credit them back — grow-in-place updates on a
     * nearly-full store falsely threw RecordStoreFullException before. */
    {
        int old_total = (int)store->records[slot].size + RMS_RECORD_OVERHEAD;
        int used_after = rms_used_bytes(store) - old_total
                       + length + RMS_RECORD_OVERHEAD;
        if (length > MAX_RECORD_SIZE || used_after > MAX_RECORD_SIZE) {
            rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreFullException", NULL);
            rms_unlock();
            return NATIVE_RETURN_VOID();
        }
    }

    /* Allocate new data (length==0 with null data stays an empty record) */
    uint8_t* new_data = NULL;
    if (length > 0) {
        new_data = (uint8_t*)malloc(length);
        if (!new_data) {
            rms_throw(jvm, thread, "java/lang/OutOfMemoryError", NULL);
            rms_unlock();
            return NATIVE_RETURN_VOID();
        }
        memcpy(new_data, (uint8_t*)array_data(data) + offset, length);
    }

    /* Replace old data */
    free(store->records[slot].data);
    store->records[slot].data = new_data;
    store->records[slot].size = length;

    /* Update version and lastModified */
    store->version++;
    store->last_modified = rms_current_time_ms();

    /* Save to disk */
    int h = rms_get_handle(store);
    if (h >= 0) rms_mark_store_dirty(h);

    /* Deliver recordChanged AFTER the mutation */
    rms_notify_listeners(jvm, store, rs_obj, "recordChanged", record_id);
    rms_enum_registry_update(jvm, h);

    rms_unlock();
    return NATIVE_RETURN_VOID();
}

/* RecordStore.deleteRecord(int) */
static JavaValue native_rms_deleteRecord(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)arg_count;
    rms_lock();
    JavaObject* rs_obj = (JavaObject*)args[0].ref;
    jint record_id = args[1].i;

    if (!rs_obj) {
        rms_throw(jvm, thread, "java/lang/NullPointerException", NULL);
        rms_unlock();
        return NATIVE_RETURN_VOID();
    }

    int state = RMS_STORE_INVALID;
    NativeRecordStore* store = get_native_store_ex(rs_obj, &state);
    if (!store) {
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreNotOpenException", NULL);
        rms_unlock();
        return NATIVE_RETURN_VOID();
    }

    if (!store->writable) {
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreException",
                  "record store is read-only");
        rms_unlock();
        return NATIVE_RETURN_VOID();
    }

    int slot = rms_find_record_slot(store, record_id);
    if (slot < 0) {
        rms_throw(jvm, thread, "javax/microedition/rms/InvalidRecordIDException", NULL);
        rms_unlock();
        return NATIVE_RETURN_VOID();
    }

    rms_remove_record_slot(store, slot);

    /* NOTE: next_id is NOT rolled back - IDs are monotonic and never reused */
    store->version++;
    store->last_modified = rms_current_time_ms();

    /* Save to disk */
    int h = rms_get_handle(store);
    if (h >= 0) rms_mark_store_dirty(h);

    /* Deliver recordDeleted AFTER the mutation */
    rms_notify_listeners(jvm, store, rs_obj, "recordDeleted", record_id);
    rms_enum_registry_update(jvm, h);

    rms_unlock();
    return NATIVE_RETURN_VOID();
}

/* RecordStore.getNumRecords() */
static JavaValue native_rms_getNumRecords(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)arg_count;
    rms_lock();
    JavaObject* rs_obj = (JavaObject*)args[0].ref;

    if (!rs_obj) {
        rms_throw(jvm, thread, "java/lang/NullPointerException", NULL);
        rms_unlock();
        return NATIVE_RETURN_INT(0);
    }

    int state = RMS_STORE_INVALID;
    NativeRecordStore* store = get_native_store_ex(rs_obj, &state);
    if (!store) {
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreNotOpenException", NULL);
        rms_unlock();
        return NATIVE_RETURN_INT(0);
    }

    int count = rms_num_records(store);
    /* v36.05: polled by loading screens — the throttle collapses bursts. */
    rms_trace_op("getNumRecords '%s' -> %d", store->name ? store->name : "?", count);
    rms_unlock();
    return NATIVE_RETURN_INT(count);
}

/* ============================================
 * RecordEnumeration construction
 * ============================================ */

/* Collect record ids (filter + sort). Returns the id Java array or NULL.
 * v34.74 (R-10): the id buffer is heap-allocated to the store size — the
 * old fixed 4096-entry stack array silently truncated larger stores
 * ("records per store unbounded" per the v17 overhaul). */
static JavaArray* rms_build_id_array(JVM* jvm, NativeRecordStore* store,
                                     JavaObject* filter, JavaObject* comparator) {
    /* Collect valid record IDs, applying filter if present */
    int cap = store->record_count > 0 ? store->record_count : 1;
    int* record_ids = (int*)malloc((size_t)cap * sizeof(int));
    int count = 0;
    if (!record_ids) return NULL;

    for (int i = 0; i < store->record_count && count < cap; i++) {
        if (!store->records[i].valid) continue;

        /* If filter is provided, call filter.matches(byte[]) */
        if (filter && heap_java_object_valid(filter)) {
            JavaArray* byte_arr = jvm_new_array(jvm, T_BYTE, store->records[i].size, NULL);
            if (byte_arr && store->records[i].data && store->records[i].size > 0) {
                memcpy(array_data(byte_arr), store->records[i].data, store->records[i].size);
            }

            JavaValue filter_args[1];
            filter_args[0].ref = byte_arr;
            JavaValue filter_result;
            memset(&filter_result, 0, sizeof(filter_result));
            int invoke_res = jvm_invoke_virtual(jvm, filter, "matches",
                "([B)Z", filter_args, &filter_result);

            if (invoke_res != 0 || filter_result.i == 0) {
                continue;
            }
        }

        record_ids[count++] = store->records[i].id;
    }

    RMS_DEBUG("enumerateRecords: %d records passed filter", count);

    /* Sort using comparator if provided (insertion sort with JVM callback) */
    if (comparator && heap_java_object_valid(comparator) && count > 1) {
        for (int i = 1; i < count; i++) {
            int key_id = record_ids[i];
            int j = i - 1;

            while (j >= 0) {
                int id_a = record_ids[j];

                int slot_a = rms_find_record_slot(store, id_a);
                int slot_k = rms_find_record_slot(store, key_id);
                int sa = slot_a >= 0 ? store->records[slot_a].size : 0;
                int sk = slot_k >= 0 ? store->records[slot_k].size : 0;

                JavaArray* arr_a = jvm_new_array(jvm, T_BYTE, sa, NULL);
                JavaArray* arr_b = jvm_new_array(jvm, T_BYTE, sk, NULL);
                if (arr_a && slot_a >= 0 && store->records[slot_a].data && sa > 0) {
                    memcpy(array_data(arr_a), store->records[slot_a].data, sa);
                }
                if (arr_b && slot_k >= 0 && store->records[slot_k].data && sk > 0) {
                    memcpy(array_data(arr_b), store->records[slot_k].data, sk);
                }

                /* compare(byte[], byte[]) -> EQUIVALENT=0, FOLLOWS=1, PRECEDES=-1 */
                JavaValue cmp_args[2];
                cmp_args[0].ref = arr_a;
                cmp_args[1].ref = arr_b;
                JavaValue cmp_result;
                memset(&cmp_result, 0, sizeof(cmp_result));
                jvm_invoke_virtual(jvm, comparator, "compare",
                    "([B[B)I", cmp_args, &cmp_result);

                if (cmp_result.i > 0) {
                    record_ids[j + 1] = record_ids[j];
                    j--;
                } else {
                    break;
                }
            }
            record_ids[j + 1] = key_id;
        }

        RMS_DEBUG("enumerateRecords: sorted %d records using comparator", count);
    }

    JavaArray* ids = jvm_new_array(jvm, T_INT, count, NULL);
    if (ids) {
        jint* ids_data = (jint*)array_data(ids);
        for (int idx = 0; idx < count; idx++) {
            ids_data[idx] = record_ids[idx];
        }
    }
    free(record_ids);
    return ids;
}

/* RecordStore.enumerateRecords - returns RecordEnumeration object */
static JavaValue native_rms_enumerateRecords(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)arg_count;
    rms_lock();
    JavaObject* rs_obj = (JavaObject*)args[0].ref;
    JavaObject* filter_obj = (JavaObject*)args[1].ref;
    JavaObject* comparator_obj = (JavaObject*)args[2].ref;
    jboolean keep_updated = args[3].i;

    RMS_DEBUG("enumerateRecords called (filter=%p, comparator=%p, keepUpdated=%d)",
              (void*)filter_obj, (void*)comparator_obj, keep_updated);

    if (!rs_obj) {
        rms_throw(jvm, thread, "java/lang/NullPointerException", NULL);
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    int state = RMS_STORE_INVALID;
    NativeRecordStore* store = get_native_store_ex(rs_obj, &state);
    if (!store) {
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreNotOpenException", NULL);
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    /* Build (filtered, sorted) id list */
    JavaArray* ids = rms_build_id_array(jvm, store, filter_obj, comparator_obj);
    if (!ids) {
        rms_throw(jvm, thread, "java/lang/OutOfMemoryError", NULL);
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    /* Load RecordEnumerationImpl class (concrete implementation) */
    JavaClass* enum_class = jvm_load_class(jvm, "javax/microedition/rms/RecordEnumerationImpl");
    if (!enum_class) {
        RMS_DEBUG("Failed to load RecordEnumerationImpl class");
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    /* The enumeration object carries 7 native-managed fields:
     * [0]=ids int[], [1]=currentIndex, [2]=store handle,
     * [3]=filter ref, [4]=comparator ref, [5]=keepUpdated, [6]=RecordStore ref.
     * instance_size must be bumped BEFORE heap_alloc_object (v15 lesson:
     * undersized stub objects stomp the next heap object's GC header). */
    if (enum_class->instance_size < sizeof(ObjectHeader) + 7 * sizeof(JavaValue)) {
        enum_class->instance_size = sizeof(ObjectHeader) + 7 * sizeof(JavaValue);
    }

    JavaObject* enum_obj = heap_alloc_object(jvm, enum_class);
    if (!enum_obj) {
        RMS_DEBUG("Failed to allocate RecordEnumeration");
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    jint store_handle = rms_get_handle(store);

    enum_obj->fields[0].ref = ids;        /* recordIds array */
    enum_obj->fields[1].i = 0;            /* currentIndex */
    enum_obj->fields[2].i = store_handle; /* store handle for nextRecord */
    enum_obj->fields[3].ref = filter_obj; /* filter (kept for rebuild) */
    enum_obj->fields[4].ref = comparator_obj; /* comparator (kept for rebuild) */
    enum_obj->fields[5].i = keep_updated ? 1 : 0;
    enum_obj->fields[6].ref = rs_obj;     /* owning RecordStore */

    /* Track kept-updated enumerations so store mutations refresh them */
    if (keep_updated) {
        rms_enum_registry_add(jvm, enum_obj, store_handle, true);
    }

    RMS_DEBUG("Created RecordEnumeration with store_handle=%d keepUpdated=%d",
            store_handle, keep_updated);

    rms_unlock();
    return NATIVE_RETURN_OBJECT(enum_obj);
}

/* RecordStore.deleteRecordStore(String) - static method */
static JavaValue native_rms_deleteRecordStore(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)arg_count;
    rms_lock();
    /* STATIC METHOD - args[0] - name (String) */
    JavaString* name_str = (JavaString*)args[0].ref;

    if (!name_str) {
        rms_throw(jvm, thread, "java/lang/NullPointerException", NULL);
        rms_unlock();
        return NATIVE_RETURN_VOID();
    }

    const char* name = string_utf8(jvm, name_str);
    RMS_DEBUG("deleteRecordStore: '%s'", name);

    rms_init();

    /* Find store */
    for (int i = 0; i < MAX_RECORD_STORES; i++) {
        if (record_stores[i].name && strcmp(record_stores[i].name, name) == 0) {
            if (record_stores[i].open) {
                /* Spec: cannot delete a store with open handles */
                rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreException",
                          "record store is still open");
                rms_unlock();
                return NATIVE_RETURN_VOID();
            }

            /* Free all records */
            for (int j = 0; j < record_stores[i].record_count; j++) {
                free(record_stores[i].records[j].data);
            }
            free(record_stores[i].records);
            record_stores[i].records = NULL;
            record_stores[i].record_count = 0;
            record_stores[i].record_capacity = 0;

            /* Release listener GC roots */
            rms_listener_free_all(jvm, &record_stores[i]);

            free(record_stores[i].name);
            record_stores[i].name = NULL;
            record_stores[i].open = false;
            record_stores[i].ref_count = 0;
            record_stores[i].next_id = 1;
            record_stores[i].version = 0;
            record_stores[i].last_modified = 0;

            /* Delete the file from disk */
            rms_delete_store_file(name);

            RMS_DEBUG("Store deleted (slot %d marked free)", i);
            rms_trace_op("deleteRecordStore '%s' -> deleted (slot %d)", name, i);
            rms_unlock();
            return NATIVE_RETURN_VOID();
        }
    }

    /* Spec: RecordStoreNotFoundException when store does not exist.
     * v34.74 (R-7): a store may exist ONLY on disk (saved by a previous
     * session, never opened this one — listRecordStores lists it since
     * v17). Deleting it must remove the FILE, not throw NotFound; leaving
     * the file behind resurrected old records on the next
     * openRecordStore(name, true) — the delete-then-recreate write pattern
     * then appended to stale data. */
    {
        char filepath[1024];
        rms_build_filepath(name, filepath, sizeof(filepath));
        if (filepath[0] && nojme_pg_remove("rms-delstore", filepath) == 0) { /* [PATHGUARD] v36.51 */
            RMS_DEBUG("deleteRecordStore: removed disk-only store file %s", filepath);
            rms_unlock();
            return NATIVE_RETURN_VOID();
        }
    }
    rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreNotFoundException", name);
    rms_unlock();
    return NATIVE_RETURN_VOID();
}

/* listRecordStores callback: collect names into a JavaArray builder */
typedef struct {
    JVM* jvm;
    const char** names;
    int count;
    int capacity;
} ListCtx;

static void rms_list_cb(const char* store_name, void* ud) {
    ListCtx* ctx = (ListCtx*)ud;

    /* Dedupe against already collected names and in-memory stores */
    for (int i = 0; i < ctx->count; i++) {
        if (strcmp(ctx->names[i], store_name) == 0) return;
    }
    for (int i = 0; i < MAX_RECORD_STORES; i++) {
        if (record_stores[i].name && strcmp(record_stores[i].name, store_name) == 0) return;
    }

    if (ctx->count >= ctx->capacity) return;
    ctx->names[ctx->count++] = strdup(store_name);
}

/* RecordStore.listRecordStores() - returns String[] */
static JavaValue native_rms_listRecordStores(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;
    rms_lock();

    rms_init();

    /* Names from memory */
    const char* names[256];
    int count = 0;
    int capacity = (int)(sizeof(names) / sizeof(names[0]));

    for (int i = 0; i < MAX_RECORD_STORES && count < capacity; i++) {
        if (record_stores[i].name) {
            names[count++] = record_stores[i].name;
        }
    }

    /* v17: scan the save directory so stores from previous sessions appear
     * without being opened first. */
    ListCtx ctx = { jvm, names, count, capacity };
    rms_scan_save_dir(rms_list_cb, &ctx);
    count = ctx.count;

    /* v34.74 (R-6, JSR 118 listRecordStores): "Returns: array of the names
     * of record stores owned by the MIDlet suite, or NULL if the MIDlet
     * suite owns no record stores." The old always-array broke the
     * canonical first-boot idiom `listRecordStores() == null` (WTK and
     * MicroEmu return null when empty). */
    if (count == 0) {
        rms_unlock();
        RMS_DEBUG("listRecordStores: no stores -> null");
        return NATIVE_RETURN_NULL();
    }

    /* Create String array (always return an array, never null) */
    JavaArray* array = jvm_new_array(jvm, DESC_OBJECT, count, NULL);
    if (!array) {
        /* Free strdup'd extras */
        for (int i = 0; i < count; i++) {
            bool owned = true;
            for (int s = 0; s < MAX_RECORD_STORES; s++) {
                if (record_stores[s].name == names[i]) { owned = false; break; }
            }
            if (owned) free((void*)names[i]);
        }
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    int idx = 0;
    for (int i = 0; i < count && idx < count; i++) {
        JavaValue str_val = { .ref = jvm_new_string(jvm, names[i]) };
        array_set_ref(array, idx++, (JavaObject*)str_val.ref);
    }

    /* Free strdup'd extras (names owned by store table are not ours) */
    for (int i = 0; i < count; i++) {
        bool owned = true;
        for (int s = 0; s < MAX_RECORD_STORES; s++) {
            if (record_stores[s].name == names[i]) { owned = false; break; }
        }
        if (owned) free((void*)names[i]);
    }

    rms_unlock();
    return NATIVE_RETURN_OBJECT(array);
}

/* RecordStore.getNextRecordID() */
static JavaValue native_rms_getNextRecordID(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)arg_count;
    rms_lock();
    JavaObject* rs_obj = (JavaObject*)args[0].ref;

    if (!rs_obj) {
        rms_throw(jvm, thread, "java/lang/NullPointerException", NULL);
        rms_unlock();
        return NATIVE_RETURN_INT(-1);
    }

    int state = RMS_STORE_INVALID;
    NativeRecordStore* store = get_native_store_ex(rs_obj, &state);
    if (!store) {
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreNotOpenException", NULL);
        rms_unlock();
        return NATIVE_RETURN_INT(-1);
    }

    int nid = store->next_id;
    rms_unlock();
    return NATIVE_RETURN_INT(nid);
}

/* RecordStore.getVersion() */
static JavaValue native_rms_getVersion(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)arg_count;
    rms_lock();
    JavaObject* rs_obj = (JavaObject*)args[0].ref;

    if (!rs_obj) {
        rms_throw(jvm, thread, "java/lang/NullPointerException", NULL);
        rms_unlock();
        return NATIVE_RETURN_INT(0);
    }

    int state = RMS_STORE_INVALID;
    NativeRecordStore* store = get_native_store_ex(rs_obj, &state);
    if (!store) {
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreNotOpenException", NULL);
        rms_unlock();
        return NATIVE_RETURN_INT(0);
    }

    int v = store->version;
    rms_unlock();
    return NATIVE_RETURN_INT(v);
}

/* Helper: get current time in milliseconds since epoch */
static jlong rms_current_time_ms(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    jlong ll = ((jlong)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (ll - 116444736000000000LL) / 10000;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (jlong)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* RecordStore.getLastModified() */
static JavaValue native_rms_getLastModified(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)arg_count;
    rms_lock();
    JavaObject* rs_obj = (JavaObject*)args[0].ref;

    if (!rs_obj) {
        rms_throw(jvm, thread, "java/lang/NullPointerException", NULL);
        rms_unlock();
        return NATIVE_RETURN_LONG(0);
    }

    int state = RMS_STORE_INVALID;
    NativeRecordStore* store = get_native_store_ex(rs_obj, &state);
    if (!store) {
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreNotOpenException", NULL);
        rms_unlock();
        return NATIVE_RETURN_LONG(0);
    }

    jlong lm = store->last_modified;
    rms_unlock();
    return NATIVE_RETURN_LONG(lm);
}

/* RecordStore.getSize() - real data size incl. per-record overhead.
 * v34.74 (R-11, JSR 118): getSize() returns LONG — the registration was
 * ()I, so any midlet compiled against the real API (invokevirtual
 * getSize ()J) found NO native, got a silent 0 and its quota logic broke. */
static JavaValue native_rms_getSize(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)arg_count;
    rms_lock();
    JavaObject* rs_obj = (JavaObject*)args[0].ref;

    if (!rs_obj) {
        rms_throw(jvm, thread, "java/lang/NullPointerException", NULL);
        rms_unlock();
        return NATIVE_RETURN_LONG(0);
    }

    int state = RMS_STORE_INVALID;
    NativeRecordStore* store = get_native_store_ex(rs_obj, &state);
    if (!store) {
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreNotOpenException", NULL);
        rms_unlock();
        return NATIVE_RETURN_LONG(0);
    }

    jlong size = (jlong)rms_used_bytes(store);
    rms_unlock();
    return NATIVE_RETURN_LONG(size);
}

/* RecordStore.getSizeAvailable() - remaining space of the 512 KB budget */
static JavaValue native_rms_getSizeAvailable(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)arg_count;
    rms_lock();
    JavaObject* rs_obj = (JavaObject*)args[0].ref;

    if (!rs_obj) {
        rms_throw(jvm, thread, "java/lang/NullPointerException", NULL);
        rms_unlock();
        return NATIVE_RETURN_INT(0);
    }

    int state = RMS_STORE_INVALID;
    NativeRecordStore* store = get_native_store_ex(rs_obj, &state);
    if (!store) {
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreNotOpenException", NULL);
        rms_unlock();
        return NATIVE_RETURN_INT(0);
    }

    int avail = MAX_RECORD_SIZE - rms_used_bytes(store);
    if (avail < 0) avail = 0;
    rms_unlock();
    return NATIVE_RETURN_INT(avail);
}

/* RecordStore.getName() */
static JavaValue native_rms_getName(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)arg_count;
    rms_lock();
    JavaObject* rs_obj = (JavaObject*)args[0].ref;

    if (!rs_obj) {
        rms_throw(jvm, thread, "java/lang/NullPointerException", NULL);
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    int state = RMS_STORE_INVALID;
    NativeRecordStore* store = get_native_store_ex(rs_obj, &state);
    if (!store || !store->name) {
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreNotOpenException", NULL);
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    JavaObject* name_obj = (JavaObject*)jvm_new_string(jvm, store->name);
    rms_unlock();
    return NATIVE_RETURN_OBJECT(name_obj);
}

/* RecordStore.setMode(authmode, writable) - enforced */
static JavaValue native_rms_setMode(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)arg_count;
    rms_lock();
    JavaObject* rs_obj = (JavaObject*)args[0].ref;
    jint authmode = args[1].i;
    jboolean writable = args[2].i;

    if (!rs_obj) {
        rms_throw(jvm, thread, "java/lang/NullPointerException", NULL);
        rms_unlock();
        return NATIVE_RETURN_VOID();
    }

    int state = RMS_STORE_INVALID;
    NativeRecordStore* store = get_native_store_ex(rs_obj, &state);
    if (!store) {
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreNotOpenException", NULL);
        rms_unlock();
        return NATIVE_RETURN_VOID();
    }

    /* Spec: IllegalArgumentException for anything but AUTHMODE_PRIVATE(0)/ANY(1) */
    if (authmode != 0 && authmode != 1) {
        rms_throw(jvm, thread, "java/lang/IllegalArgumentException",
                  "invalid authmode");
        rms_unlock();
        return NATIVE_RETURN_VOID();
    }

    store->authmode = (int)authmode;
    store->writable = (writable != 0);

    /* Persist the new mode */
    int h = rms_get_handle(store);
    if (h >= 0) rms_mark_store_dirty(h);

    RMS_DEBUG("setMode: '%s' authmode=%d writable=%d",
            store->name ? store->name : "?", authmode, writable != 0);

    rms_unlock();
    return NATIVE_RETURN_VOID();
}

/* RecordStore.addRecordListener */
static JavaValue native_rms_addRecordListener(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)arg_count;
    rms_lock();
    JavaObject* rs_obj = (JavaObject*)args[0].ref;
    JavaObject* listener = (JavaObject*)args[1].ref;

    if (!rs_obj) {
        rms_throw(jvm, thread, "java/lang/NullPointerException", NULL);
        rms_unlock();
        return NATIVE_RETURN_VOID();
    }

    NativeRecordStore* store = get_native_store(rs_obj);
    if (!store) {
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreNotOpenException", NULL);
        rms_unlock();
        return NATIVE_RETURN_VOID();
    }

    if (listener) {
        rms_listener_add(jvm, store, listener);
    }
    rms_unlock();
    return NATIVE_RETURN_VOID();
}

/* RecordStore.removeRecordListener */
static JavaValue native_rms_removeRecordListener(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)arg_count;
    rms_lock();
    JavaObject* rs_obj = (JavaObject*)args[0].ref;
    JavaObject* listener = (JavaObject*)args[1].ref;

    if (!rs_obj) {
        rms_throw(jvm, thread, "java/lang/NullPointerException", NULL);
        rms_unlock();
        return NATIVE_RETURN_VOID();
    }

    NativeRecordStore* store = get_native_store(rs_obj);
    if (store && listener) {
        rms_listener_remove(jvm, store, listener);
    }
    rms_unlock();
    return NATIVE_RETURN_VOID();
}

/* openRecordStore(String, boolean, int, boolean) - authmode overload */
static JavaValue native_rms_openRecordStore_authmode(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)arg_count;
    rms_lock();
    /* Static: args[0]=name, args[1]=create, args[2]=authmode, args[3]=writable */
    JavaString* name_str = (JavaString*)args[0].ref;
    jboolean create = args[1].i;
    jint authmode = args[2].i;
    jboolean writable = args[3].i;

    if (!name_str) {
        RMS_DEBUG("openRecordStore(authmode): name is NULL");
        rms_throw(jvm, thread, "java/lang/NullPointerException", NULL);
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    const char* name = string_utf8(jvm, name_str);
    if (!name || name[0] == '\0' || strlen(name) > 32) {
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreException",
                  "invalid record store name");
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    /* v34.74 (R-5, JSR 118): IllegalArgumentException must be raised BEFORE
     * any store is touched (the old code opened the store first, leaking a
     * ref_count on the error path). */
    if (authmode != 0 && authmode != 1) {
        rms_throw(jvm, thread, "java/lang/IllegalArgumentException", "invalid authmode");
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    RMS_DEBUG("openRecordStore(authmode): '%s', create=%d", name, create);

    bool created = false;
    int handle = rms_open(name, create != 0, &created);

    if (handle < 0) {
        if (handle == -1) {
            rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreNotFoundException", name);
        } else {
            rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreFullException", NULL);
        }
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    /* v34.74 (R-5): authmode/writable apply ONLY when this call created a
     * brand-new store (JSR 118 openRecordStore: "the authorization mode is
     * set only when the record store is created"). Re-opening an existing
     * store (memory or disk) with writable=false used to flip the live
     * store read-only — and the mode was persisted, permanently bricking
     * the store for midlets that pass AUTHMODE_PRIVATE,false defensively. */
    if (created) {
        record_stores[handle].authmode = (int)authmode;
        record_stores[handle].writable = (writable != 0);
    }

    JavaObject* rs_obj = rms_make_rs_object(jvm, handle);
    rms_unlock();
    if (!rs_obj) return NATIVE_RETURN_NULL();
    return NATIVE_RETURN_OBJECT(rs_obj);
}

/* openRecordStore(String, String, String) - vendor/suite overload */
static JavaValue native_rms_openRecordStore_vendor(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)arg_count;
    rms_lock();
    /* Static: args[0]=name, args[1]=vendorName, args[2]=suiteName */
    JavaString* name_str = (JavaString*)args[0].ref;

    if (!name_str) {
        RMS_DEBUG("openRecordStore(vendor): name is NULL");
        rms_throw(jvm, thread, "java/lang/NullPointerException", NULL);
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    const char* name = string_utf8(jvm, name_str);
    RMS_DEBUG("openRecordStore(vendor): '%s'", name);

    /* Do not create if missing (matches FreeJ2ME) */
    int handle = rms_open(name, false, NULL);

    if (handle < 0) {
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreNotFoundException", name);
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    JavaObject* rs_obj = rms_make_rs_object(jvm, handle);
    rms_unlock();
    if (!rs_obj) return NATIVE_RETURN_NULL();
    return NATIVE_RETURN_OBJECT(rs_obj);
}

/* ============================================
 * RecordEnumeration methods
 * ============================================ */

/* Fetch enum state; returns false if object/handle invalid */
static bool rms_enum_ids(JavaObject* enum_obj, JavaArray** ids_out, jint* current_out,
                         jint* handle_out) {
    if (!enum_obj) return false;
    JavaArray* ids = (JavaArray*)enum_obj->fields[0].ref;
    if (!ids) return false;
    *ids_out = ids;
    *current_out = enum_obj->fields[1].i;
    *handle_out = enum_obj->fields[2].i;
    return true;
}

/* Validate store handle from an enumeration; returns store or NULL */
static NativeRecordStore* rms_enum_store(JavaObject* enum_obj, bool require_open) {
    jint handle = enum_obj->fields[2].i;
    if (handle < 0 || handle >= MAX_RECORD_STORES) return NULL;
    NativeRecordStore* store = &record_stores[handle];
    if (!store->name) return NULL;
    if (require_open && !store->open) return NULL;
    return store;
}

/* RecordEnumeration.hasNextElement() */
static JavaValue native_enumeration_hasNextElement(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* enum_obj = (JavaObject*)args[0].ref;

    if (!enum_obj) return NATIVE_RETURN_INT(0);

    JavaArray* ids = NULL; jint current = 0; jint handle = 0;
    if (!rms_enum_ids(enum_obj, &ids, &current, &handle)) {
        return NATIVE_RETURN_INT(0);
    }

    jboolean has_next = (current < (jint)ids->length) ? JNI_TRUE : JNI_FALSE;
    RMS_DEBUG("hasNextElement: current=%d, length=%d, result=%d",
            current, ids->length, has_next);
    return NATIVE_RETURN_INT(has_next);
}

/* RecordEnumeration.nextRecordId() */
static JavaValue native_enumeration_nextRecordId(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)arg_count;
    JavaObject* enum_obj = (JavaObject*)args[0].ref;

    if (!enum_obj) {
        rms_throw(jvm, thread, "javax/microedition/rms/InvalidRecordIDException", NULL);
        return NATIVE_RETURN_INT(-1);
    }

    JavaArray* ids = NULL; jint current = 0; jint handle = 0;
    if (!rms_enum_ids(enum_obj, &ids, &current, &handle) || current >= (jint)ids->length) {
        rms_throw(jvm, thread, "javax/microedition/rms/InvalidRecordIDException", NULL);
        return NATIVE_RETURN_INT(-1);
    }

    jint* ids_data = (jint*)array_data(ids);
    jint record_id = ids_data[current];

    /* v34.74 (R-1, JSR 118 RecordEnumeration.nextRecordId): "Returns the
     * recordId of the next record in this enumeration, that is, the
     * recordId of the record that will be returned by the next call to
     * nextRecord(). This method DOES NOT ADVANCE the enumeration."
     * The old code stepped the cursor here, so the canonical idiom
     *   while (e.hasNextElement()) { id = e.nextRecordId(); d = e.nextRecord(); }
     * consumed TWO records per iteration, mispaired ids with data and
     * skipped every second record — and midlets that wrote records back
     * using those ids corrupted their own save files. */
    RMS_DEBUG("nextRecordId: peeking %d (cursor stays %d)", record_id, current);
    return NATIVE_RETURN_INT(record_id);
}

/* RecordEnumeration.nextRecord() - returns byte[] of next record */
static JavaValue native_enumeration_nextRecord(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)arg_count;
    rms_lock();
    JavaObject* enum_obj = (JavaObject*)args[0].ref;

    if (!enum_obj) {
        rms_throw(jvm, thread, "javax/microedition/rms/InvalidRecordIDException", NULL);
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    JavaArray* ids = NULL; jint current = 0; jint handle = 0;
    if (!rms_enum_ids(enum_obj, &ids, &current, &handle) || current >= (jint)ids->length) {
        rms_throw(jvm, thread, "javax/microedition/rms/InvalidRecordIDException", NULL);
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    NativeRecordStore* store = rms_enum_store(enum_obj, true);
    if (!store) {
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreNotOpenException", NULL);
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    jint* ids_data = (jint*)array_data(ids);
    jint record_id = ids_data[current];

    /* Advance index */
    enum_obj->fields[1].i = current + 1;

    int slot = rms_find_record_slot(store, record_id);
    if (slot < 0) {
        /* Record was deleted after enumeration snapshot */
        rms_throw(jvm, thread, "javax/microedition/rms/InvalidRecordIDException", NULL);
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    Record* rec = &store->records[slot];
    JavaArray* data = jvm_new_array(jvm, T_BYTE, rec->size, NULL);
    if (!data) {
        rms_throw(jvm, thread, "java/lang/OutOfMemoryError", NULL);
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    if (rec->size > 0 && rec->data) {
        memcpy(array_data(data), rec->data, rec->size);
    }

    RMS_DEBUG("nextRecord: returning %d bytes for record %d", rec->size, record_id);
    rms_unlock();
    return NATIVE_RETURN_OBJECT(data);
}

/* RecordEnumeration.destroy() - cleanup enumeration */
static JavaValue native_enumeration_destroy(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* enum_obj = (JavaObject*)args[0].ref;

    if (enum_obj) {
        /* Unregister from kept-updated registry */
        for (int i = 0; i < RMS_MAX_ENUMS; i++) {
            if (g_rms_enums[i].in_use && g_rms_enums[i].enum_obj == enum_obj) {
                gc_remove_root(jvm, (void**)&g_rms_enums[i].enum_obj);
                g_rms_enums[i].in_use = false;
                g_rms_enums[i].enum_obj = NULL;
                break;
            }
        }

        /* Clear the ids array reference */
        enum_obj->fields[0].ref = NULL;
        enum_obj->fields[1].i = 0;
        enum_obj->fields[5].i = 0;
    }

    return NATIVE_RETURN_VOID();
}

/* RecordEnumeration.numRecords() */
static JavaValue native_enumeration_numRecords(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* enum_obj = (JavaObject*)args[0].ref;

    if (!enum_obj) return NATIVE_RETURN_INT(0);

    JavaArray* ids = (JavaArray*)enum_obj->fields[0].ref;
    return NATIVE_RETURN_INT(ids ? ids->length : 0);
}

/* RecordEnumeration.hasPreviousElement() */
static JavaValue native_enumeration_hasPreviousElement(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* enum_obj = (JavaObject*)args[0].ref;

    if (!enum_obj) return NATIVE_RETURN_INT(0);

    jint current = enum_obj->fields[1].i;
    return NATIVE_RETURN_INT(current > 0 ? 1 : 0);
}

/* RecordEnumeration.previousRecordId() - spec: throws at start, no wrap */
static JavaValue native_enumeration_previousRecordId(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)arg_count;
    JavaObject* enum_obj = (JavaObject*)args[0].ref;

    if (!enum_obj) {
        rms_throw(jvm, thread, "javax/microedition/rms/InvalidRecordIDException", NULL);
        return NATIVE_RETURN_INT(-1);
    }

    JavaArray* ids = (JavaArray*)enum_obj->fields[0].ref;
    jint current = enum_obj->fields[1].i;

    if (!ids || current <= 0) {
        rms_throw(jvm, thread, "javax/microedition/rms/InvalidRecordIDException", NULL);
        return NATIVE_RETURN_INT(-1);
    }

    /* v34.74 (R-1): previousRecordId is the mirror of nextRecordId — it
     * returns the id of the record that the NEXT previousRecord() call
     * will return, WITHOUT stepping the cursor. */
    jint* ids_data = (jint*)array_data(ids);
    RMS_DEBUG("previousRecordId: peeking %d (cursor stays %d)", ids_data[current - 1], current);
    return NATIVE_RETURN_INT(ids_data[current - 1]);
}

/* RecordEnumeration.previousRecord() - spec: throws at start, no wrap */
static JavaValue native_enumeration_previousRecord(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)arg_count;
    rms_lock();
    JavaObject* enum_obj = (JavaObject*)args[0].ref;

    if (!enum_obj) {
        rms_throw(jvm, thread, "javax/microedition/rms/InvalidRecordIDException", NULL);
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    JavaArray* ids = (JavaArray*)enum_obj->fields[0].ref;
    jint current = enum_obj->fields[1].i;

    if (!ids || current <= 0) {
        rms_throw(jvm, thread, "javax/microedition/rms/InvalidRecordIDException", NULL);
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    NativeRecordStore* store = rms_enum_store(enum_obj, true);
    if (!store) {
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreNotOpenException", NULL);
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    current--;
    enum_obj->fields[1].i = current;

    jint* ids_data = (jint*)array_data(ids);
    jint record_id = ids_data[current];

    int slot = rms_find_record_slot(store, record_id);
    if (slot < 0) {
        rms_throw(jvm, thread, "javax/microedition/rms/InvalidRecordIDException", NULL);
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }

    Record* rec = &store->records[slot];
    JavaArray* data = jvm_new_array(jvm, T_BYTE, rec->size, NULL);
    if (!data) {
        rms_throw(jvm, thread, "java/lang/OutOfMemoryError", NULL);
        rms_unlock();
        return NATIVE_RETURN_NULL();
    }
    if (rec->size > 0 && rec->data) {
        memcpy(array_data(data), rec->data, rec->size);
    }
    rms_unlock();
    return NATIVE_RETURN_OBJECT(data);
}

/* RecordEnumeration.reset() - reset index to beginning */
static JavaValue native_enumeration_reset(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* enum_obj = (JavaObject*)args[0].ref;

    if (enum_obj) {
        enum_obj->fields[1].i = 0;
    }

    return NATIVE_RETURN_VOID();
}

/* RecordEnumeration.rebuild() - re-run filter + comparator, reset index */
static JavaValue native_enumeration_rebuild(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)arg_count;
    rms_lock();
    JavaObject* enum_obj = (JavaObject*)args[0].ref;

    if (!enum_obj) {
        rms_throw(jvm, thread, "java/lang/NullPointerException", NULL);
        rms_unlock();
        return NATIVE_RETURN_VOID();
    }

    NativeRecordStore* store = rms_enum_store(enum_obj, true);
    if (!store) {
        rms_throw(jvm, thread, "javax/microedition/rms/RecordStoreNotOpenException", NULL);
        rms_unlock();
        return NATIVE_RETURN_VOID();
    }

    JavaObject* filter = (JavaObject*)enum_obj->fields[3].ref;
    JavaObject* comparator = (JavaObject*)enum_obj->fields[4].ref;

    JavaArray* ids = rms_build_id_array(jvm, store, filter, comparator);
    if (ids) {
        enum_obj->fields[0].ref = ids;
        enum_obj->fields[1].i = 0;
    }

    rms_unlock();
    return NATIVE_RETURN_VOID();
}

/* RecordEnumeration.isKeptUpdated() */
static JavaValue native_enumeration_isKeptUpdated(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count; (void)args;
    JavaObject* enum_obj = (JavaObject*)args[0].ref;
    if (!enum_obj) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(enum_obj->fields[5].i ? 1 : 0);
}

/* RecordEnumeration.keepUpdated(boolean) */
static JavaValue native_enumeration_keepUpdated(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)arg_count;
    rms_lock();
    JavaObject* enum_obj = (JavaObject*)args[0].ref;
    jboolean keep = args[1].i;

    if (!enum_obj) {
        rms_throw(jvm, thread, "java/lang/NullPointerException", NULL);
        rms_unlock();
        return NATIVE_RETURN_VOID();
    }

    bool was = enum_obj->fields[5].i != 0;
    enum_obj->fields[5].i = keep ? 1 : 0;

    if (keep && !was) {
        jint handle = enum_obj->fields[2].i;
        rms_enum_registry_add(jvm, enum_obj, handle, true);
        /* Spec: enabling keepUpdated refreshes the enumeration immediately */
        NativeRecordStore* store = rms_enum_store(enum_obj, true);
        if (store) {
            JavaObject* filter = (JavaObject*)enum_obj->fields[3].ref;
            JavaObject* comparator = (JavaObject*)enum_obj->fields[4].ref;
            JavaArray* ids = rms_build_id_array(jvm, store, filter, comparator);
            if (ids) {
                enum_obj->fields[0].ref = ids;
                enum_obj->fields[1].i = 0;
            }
        }
    } else if (!keep && was) {
        /* Stop auto-refreshing this enumeration */
        for (int i = 0; i < RMS_MAX_ENUMS; i++) {
            if (g_rms_enums[i].in_use && g_rms_enums[i].enum_obj == enum_obj) {
                gc_remove_root(jvm, (void**)&g_rms_enums[i].enum_obj);
                g_rms_enums[i].in_use = false;
                g_rms_enums[i].enum_obj = NULL;
                break;
            }
        }
    }

    rms_unlock();
    return NATIVE_RETURN_VOID();
}

void init_javax_microedition_rms(JVM* jvm) {
    rms_lock();

    /* Initialize the kept-updated enum registry */
    memset(g_rms_enums, 0, sizeof(g_rms_enums));

    NativeMethodEntry methods[] = {
        /* Static methods */
        {"javax/microedition/rms/RecordStore", "openRecordStore",
         "(Ljava/lang/String;Z)Ljavax/microedition/rms/RecordStore;", native_rms_openRecordStore},
        {"javax/microedition/rms/RecordStore", "openRecordStore",
         "(Ljava/lang/String;ZIZ)Ljavax/microedition/rms/RecordStore;", native_rms_openRecordStore_authmode},
        {"javax/microedition/rms/RecordStore", "openRecordStore",
         "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljavax/microedition/rms/RecordStore;", native_rms_openRecordStore_vendor},
        {"javax/microedition/rms/RecordStore", "deleteRecordStore",
         "(Ljava/lang/String;)V", native_rms_deleteRecordStore},
        {"javax/microedition/rms/RecordStore", "listRecordStores",
         "()[Ljava/lang/String;", native_rms_listRecordStores},

        /* Instance methods */
        {"javax/microedition/rms/RecordStore", "closeRecordStore",
         "()V", native_rms_closeRecordStore},
        {"javax/microedition/rms/RecordStore", "addRecord",
         "([BII)I", native_rms_addRecord},
        {"javax/microedition/rms/RecordStore", "getRecord",
         "(I)[B", native_rms_getRecord},
        {"javax/microedition/rms/RecordStore", "getRecord",
         "(I[BI)I", native_rms_getRecord_array},
        {"javax/microedition/rms/RecordStore", "getRecordSize",
         "(I)I", native_rms_getRecordSize},
        {"javax/microedition/rms/RecordStore", "setRecord",
         "(I[BII)V", native_rms_setRecord},
        {"javax/microedition/rms/RecordStore", "deleteRecord",
         "(I)V", native_rms_deleteRecord},
        {"javax/microedition/rms/RecordStore", "getNumRecords",
         "()I", native_rms_getNumRecords},
        {"javax/microedition/rms/RecordStore", "enumerateRecords",
         "(Ljavax/microedition/rms/RecordFilter;Ljavax/microedition/rms/RecordComparator;Z)Ljavax/microedition/rms/RecordEnumeration;",
         native_rms_enumerateRecords},
        {"javax/microedition/rms/RecordStore", "getNextRecordID",
         "()I", native_rms_getNextRecordID},
        {"javax/microedition/rms/RecordStore", "getVersion",
         "()I", native_rms_getVersion},
        {"javax/microedition/rms/RecordStore", "getLastModified",
         "()J", native_rms_getLastModified},
        {"javax/microedition/rms/RecordStore", "getSize",
         "()J", native_rms_getSize},
        {"javax/microedition/rms/RecordStore", "getSizeAvailable",
         "()I", native_rms_getSizeAvailable},
        {"javax/microedition/rms/RecordStore", "getName",
         "()Ljava/lang/String;", native_rms_getName},
        {"javax/microedition/rms/RecordStore", "setMode",
         "(IZ)V", native_rms_setMode},
        {"javax/microedition/rms/RecordStore", "addRecordListener",
         "(Ljavax/microedition/rms/RecordListener;)V", native_rms_addRecordListener},
        {"javax/microedition/rms/RecordStore", "removeRecordListener",
         "(Ljavax/microedition/rms/RecordListener;)V", native_rms_removeRecordListener},

        /* RecordEnumeration interface methods */
        {"javax/microedition/rms/RecordEnumeration", "hasNextElement",
         "()Z", native_enumeration_hasNextElement},
        {"javax/microedition/rms/RecordEnumeration", "hasPreviousElement",
         "()Z", native_enumeration_hasPreviousElement},
        {"javax/microedition/rms/RecordEnumeration", "nextRecordId",
         "()I", native_enumeration_nextRecordId},
        {"javax/microedition/rms/RecordEnumeration", "nextRecord",
         "()[B", native_enumeration_nextRecord},
        {"javax/microedition/rms/RecordEnumeration", "previousRecordId",
         "()I", native_enumeration_previousRecordId},
        {"javax/microedition/rms/RecordEnumeration", "previousRecord",
         "()[B", native_enumeration_previousRecord},
        {"javax/microedition/rms/RecordEnumeration", "destroy",
         "()V", native_enumeration_destroy},
        {"javax/microedition/rms/RecordEnumeration", "numRecords",
         "()I", native_enumeration_numRecords},
        {"javax/microedition/rms/RecordEnumeration", "reset",
         "()V", native_enumeration_reset},
        {"javax/microedition/rms/RecordEnumeration", "rebuild",
         "()V", native_enumeration_rebuild},
        {"javax/microedition/rms/RecordEnumeration", "isKeptUpdated",
         "()Z", native_enumeration_isKeptUpdated},
        {"javax/microedition/rms/RecordEnumeration", "keepUpdated",
         "(Z)V", native_enumeration_keepUpdated},

        /* RecordEnumerationImpl concrete class methods (same implementations) */
        {"javax/microedition/rms/RecordEnumerationImpl", "hasNextElement",
         "()Z", native_enumeration_hasNextElement},
        {"javax/microedition/rms/RecordEnumerationImpl", "hasPreviousElement",
         "()Z", native_enumeration_hasPreviousElement},
        {"javax/microedition/rms/RecordEnumerationImpl", "nextRecordId",
         "()I", native_enumeration_nextRecordId},
        {"javax/microedition/rms/RecordEnumerationImpl", "nextRecord",
         "()[B", native_enumeration_nextRecord},
        {"javax/microedition/rms/RecordEnumerationImpl", "previousRecordId",
         "()I", native_enumeration_previousRecordId},
        {"javax/microedition/rms/RecordEnumerationImpl", "previousRecord",
         "()[B", native_enumeration_previousRecord},
        {"javax/microedition/rms/RecordEnumerationImpl", "destroy",
         "()V", native_enumeration_destroy},
        {"javax/microedition/rms/RecordEnumerationImpl", "numRecords",
         "()I", native_enumeration_numRecords},
        {"javax/microedition/rms/RecordEnumerationImpl", "reset",
         "()V", native_enumeration_reset},
        {"javax/microedition/rms/RecordEnumerationImpl", "rebuild",
         "()V", native_enumeration_rebuild},
        {"javax/microedition/rms/RecordEnumerationImpl", "isKeptUpdated",
         "()Z", native_enumeration_isKeptUpdated},
        {"javax/microedition/rms/RecordEnumerationImpl", "keepUpdated",
         "(Z)V", native_enumeration_keepUpdated},
    };

    int count = sizeof(methods) / sizeof(methods[0]);
    native_register_methods(jvm, methods, count);

    rms_unlock();
    RMS_DEBUG("Registered %d native methods", count);
}

/* ============================================================================
 * v35.08 MULTI-SESSION: per-session static reset (midp_session_reset <-
 * jvm_destroy). The enumeration registry and the per-store listener slots
 * hold Java objects of the previous session. The central gc_roots_reset_all()
 * already NULLs every REGISTERED slot (both live here: static arrays), this
 * clears the surrounding bookkeeping (in_use flags, duplicate registrations
 * of slots whose listener was removed by value-compare, stale handles) so
 * the next game starts with an empty registry.
 * The store table itself (record_stores) keeps names/records - RMS data is
 * disk-backed and must survive across games; only Java object refs go.
 * ============================================================================ */
void midp_rms_session_reset(void) {
    /* v35.09: the Switch frontend has no unload hook (midp_rms_save_all is
     * libretro-only) and mutations now save lazily — this is the last chance
     * to flush dirty stores before the teardown. No-op when nothing changed. */
    rms_flush_dirty_stores();
    /* v36.13: drop the GC-root registrations BEFORE NULLing the object slots
     * — the old code only NULLed the fields, so every session leaked its
     * listener/enum root SLOTS in the heap root table (each dead slot kept
     * pointing at a stale heap address until gc_roots_reset_all at teardown;
     * root-table capacity erodes a little every session). */
    for (int i = 0; i < RMS_MAX_ENUMS; i++) {
        if (g_rms_enums[i].in_use) {
            gc_remove_root(NULL, (void**)&g_rms_enums[i].enum_obj);
        }
        g_rms_enums[i].enum_obj = NULL;
        g_rms_enums[i].store_handle = -1;
        g_rms_enums[i].in_use = false;
    }
    for (int s = 0; s < MAX_RECORD_STORES; s++) {
        for (int l = 0; l < RMS_MAX_LISTENERS; l++) {
            if (record_stores[s].listeners[l]) {
                gc_remove_root(NULL, (void**)&record_stores[s].listeners[l]);
                record_stores[s].listeners[l] = NULL;
            }
        }
    }
}
