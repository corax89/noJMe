/*
 * jar_reader.c — single canonical JAR/ZIP resource reader (v19).
 *
 * WHY THIS EXISTS
 * ===============
 * Until v18 there were FOUR independent hand-rolled ZIP scanners in the
 * code base (main.c, jvm.c, libretro/libretro.c, libretro/sdl_backend_stubs.c).
 * All four parsed the End-Of-Central-Directory record and the central
 * directory by hand. Divergence between them caused at least one hard to
 * reproduce bug: on a user's Windows build class loading (jvm.c scanner)
 * worked while resource loading (sdl_backend_stubs.c scanner) returned NULL
 * for every entry — NEscube then rendered its cube without the texture.
 *
 * This file replaces all four with ONE implementation built on miniz
 * (src/utils/miniz.c, already part of every build target). miniz handles
 * the whole zoo of valid-but-exotic ZIP layouts our hand-rolled code does
 * not: data descriptors (streaming zippers), ZIP64, SFX/prepended data,
 * entries with extra fields/comments, etc.
 *
 * API
 * ===
 *   jar_read_file(data, size, "path/inside/jar", &out_size) -> malloc'd buffer
 *
 * The returned buffer always has ONE extra NUL byte (not counted in
 * *out_size) — callers historically relied on text resources being
 * NUL-terminated. Free with free().
 *
 * Name lookup: exact match first; if that fails a case-insensitive pass
 * runs (a jar repacked on Windows may change letter case). Directory
 * entries never match.
 *
 * Diagnostics: failures are logged to stderr with the exact reason and
 * rate-limited, so a broken build reports e.g.
 *   [JAR-READ] FAILED (no-zip): 'res/bin/texture.res'
 * instead of leaving callers to guess which NULL branch fired.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "debug.h"   /* v34.24: routes jar_log through the log gate (quiet by default) */
#include "miniz.h"
#include "jar_reader.h"

/* Rate limiting: stop spamming after this many messages per session. */
#define JAR_READ_MAX_LOGS 24
static int s_jar_read_logs = 0;

/* v36.08: mirror every failure into the device trace file. The stderr lines
 * never reach log.txt on Switch, and a jar-read failure is exactly the kind
 * of silent null that field logs must explain (Doom RPG session 3: a failed
 * entities.str read produced a null stream and the game's readFully helper
 * spun on EOF forever). Weak extern: host builds without switch_trace link
 * fine without it. */
__attribute__((format(gnu_printf, 1, 2)))
static void jar_trace(const char* fmt, ...) {
    extern void sw_trace_force(const char* fmt, ...) __attribute__((weak));
    if (&sw_trace_force == NULL || sw_trace_force == NULL) return;
    va_list ap;
    va_start(ap, fmt);
    char buf[192];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sw_trace_force("%s", buf);
}

/* v34.30: gnu_printf flavor — jar_log formats use %zu (size_t); MinGW's
 * ms_printf checking rejects 'z'. Same fix as J2ME_PRINTF_ATTR in debug.h
 * (which this file includes above). */
__attribute__((format(gnu_printf, 1, 2)))
static void jar_log(const char* fmt, ...) {
    if (s_jar_read_logs >= JAR_READ_MAX_LOGS) return;
    s_jar_read_logs++;
    va_list ap;
    va_start(ap, fmt);
    {
        char buf[192];
        vsnprintf(buf, sizeof(buf), fmt, ap);
        jar_trace("%s", buf);   /* v36.08: same text into the trace file */
        fputs(buf, stderr);
        fflush(stderr);
    }
    va_end(ap);
}

/* v34.71: existence probe WITHOUT extraction. Callers that blacklist
 * resources after a failed load (display.c's missing-image cache) must
 * distinguish "the entry genuinely does not exist in the JAR" from a
 * TRANSIENT failure (inflate OOM / heap pressure / unsupported method) —
 * blacklisting a good resource made every later createImage() return a
 * magenta placeholder forever (Stalker "textures disappear after a
 * while": texobj1.png IS in the JAR, one decode failure poisoned it). */
int jar_has_entry(const void* jar_data, size_t jar_size, const char* name) {
    if (!jar_data || jar_size < 22 || !name || !name[0]) return 0;

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, jar_data, jar_size, 0)) return 0;

    int idx = mz_zip_reader_locate_file(&zip, name, NULL, MZ_ZIP_FLAG_CASE_SENSITIVE);
    if (idx < 0) idx = mz_zip_reader_locate_file(&zip, name, NULL, 0);

    mz_zip_reader_end(&zip);
    return idx >= 0;
}

uint8_t* jar_read_file(const void* jar_data, size_t jar_size,
                       const char* name, size_t* out_size) {
    if (out_size) *out_size = 0;
    if (!jar_data || jar_size < 22 || !name || !name[0]) {
        jar_log("[JAR-READ] FAILED (bad-args): '%s' (data=%p size=%zu)\n",
                name ? name : "NULL", jar_data, jar_size);
        return NULL;
    }

    /* v36.33 [JAR-ABS-PATH]: archive entry names never carry a leading
     * slash, but the J2ME APIs take jar-absolute resource names WITH one —
     * Image.createImage("/font_button.png") and
     * Class.getResourceAsStream("/font_button.png") are canonical usage
     * (Speedx 3D's BitmapFont.startElement loads its glyph pages exactly
     * like that; the un-normalized probe failed and the game threw
     * IOException — fonts dead). Strip any leading slashes before the
     * miniz lookup instead of patching every caller. */
    while (name[0] == '/') name++;

    /* Stack-local archive handle: getResourceAsStream may be called from
     * several Java threads concurrently, so no shared static state here. */
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));

    if (!mz_zip_reader_init_mem(&zip, jar_data, jar_size, 0)) {
        jar_log("[JAR-READ] FAILED (no-zip): '%s' (not a readable ZIP/JAR image)\n", name);
        return NULL;
    }

    /* 1) Exact, case-sensitive match (fast path — miniz sorts lowercased
     *    names at init, so even the insensitive pass is a binary search). */
    int idx = mz_zip_reader_locate_file(&zip, name, NULL, MZ_ZIP_FLAG_CASE_SENSITIVE);

    /* 2) Case-insensitive fallback (jar repacked on Windows). */
    if (idx < 0) {
        idx = mz_zip_reader_locate_file(&zip, name, NULL, 0);
    }

    if (idx < 0) {
        /* Class probes (foo/bar/Exception.class, [I.class ...) are EXPECTED
         * misses: the VM probes the JAR before falling back to built-in
         * stubs. Logging them would spam every session with noise, so they
         * stay silent; genuine resource misses still log. */
        if (!strstr(name, ".class")) {
            jar_log("[JAR-READ] FAILED (no-entry): '%s'\n", name);
        }
        mz_zip_reader_end(&zip);
        return NULL;
    }

    size_t len = 0;
    void* data = mz_zip_reader_extract_to_heap(&zip, (mz_uint)idx, &len, 0);
    mz_zip_reader_end(&zip);

    if (!data) {
        jar_log("[JAR-READ] FAILED (extract-fail): '%s' (corrupt or unsupported method)\n", name);
        return NULL;
    }

    /* Preserve the historical contract: one extra NUL terminator. */
    uint8_t* out = (uint8_t*)realloc(data, len + 1);
    if (!out) {
        free(data);
        jar_log("[JAR-READ] FAILED (oom): '%s' (%zu bytes)\n", name, len);
        return NULL;
    }
    out[len] = '\0';

    if (out_size) *out_size = len;
    return out;
}

/* v34.29: manifest line-folding — see include/jar_reader.h for the spec
 * rationale and the JBenchmark 3D failure case. Implementation notes:
 *  - a physical line break is \r, \n or a run of them (CRLF, LFCR, CRCR…)
 *  - a SINGLE space right after the break marks a continuation: the space
 *    is dropped and nothing else happens (the logical line continues)
 *  - a line starting with 2+ spaces is a continuation whose value begins
 *    with the remaining spaces (spec: exactly one space is the marker)
 *  - a real logical-line break emits exactly one '\n', so downstream
 *    strtok/strstr parsing sees clean folded lines */
char* jar_manifest_unfold(const uint8_t* data, size_t size) {
    if (!data && size > 0) return NULL;

    char* out = (char*)malloc(size + 1);
    if (!out) return NULL;

    size_t o = 0;
    size_t i = 0;
    while (i < size) {
        char c = (char)data[i];
        if (c == '\r' || c == '\n') {
            /* Consume the entire run of newline characters. */
            size_t j = i;
            while (j < size && ((char)data[j] == '\r' || (char)data[j] == '\n')) j++;

            int folded = 0;
            if (j < size && (char)data[j] == ' ') {
                j++;          /* exactly one space is the fold marker */
                folded = 1;
            }
            if (!folded && j < size) {
                out[o++] = '\n';   /* real end of logical line */
            }
            i = j;
            continue;
        }
        out[o++] = c;
        i++;
    }
    out[o] = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/* v34.46 (Treasure Towers): screen-size hints from the JAR manifest. */

/* Portable case-insensitive prefix match (locale-free, ASCII only —
 * manifest keys are ASCII by spec). */
static int jrs_key_starts_with(const char* line, const char* key) {
    size_t n = 0;
    while (key[n]) {
        char a = line[n];
        char b = key[n];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (!a || a != b) return 0;
        n++;
    }
    return 1;
}

/* Extract the FIRST and the LAST integer from a hint value.
 * Covers every separator style seen in real manifests:
 *   "128, 160"  "128,160"  "176x208"  "176,&,208" (Nokia orientation mark)
 * Returns 1 when at least two integers were found. */
static int jrs_parse_int_pair(const char* s, int* first, int* last) {
    int found = 0;
    int val = 0;
    const char* p = s;
    *first = 0;
    *last = 0;
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            val = 0;
            while (*p >= '0' && *p <= '9') {
                val = val * 10 + (*p - '0');
                if (val > 99999) {   /* runaway digits: not a size */
                    while (*p >= '0' && *p <= '9') p++;
                    val = 0;
                    break;
                }
                p++;
            }
            if (val > 0) {
                if (found == 0) *first = val;
                *last = val;
                found++;
            }
        } else {
            p++;
        }
    }
    return found >= 2;
}

/* ASCII-only alnum/digit tests (bytes >= 0x80 — cyrillic etc. — are NOT
 * alnum here, so "рация240x320.jar" still finds the pair). */
static int jrs_is_alnum(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/* v35.12: screen-size hint from the FILE NAME.
 * Many repro sites ship one jar per handset with the resolution embedded in
 * the file name ("Hero of Sparta 240x320.jar", "Rayman 3_320X240.jar",
 * "Game240x320.jar"). Used when the manifest carries no explicit
 * display-size attribute.
 *
 * Matches <digits>(1-4)[xX]<digits>(1-4): the first digit run must START at
 * a non-digit ("3 3D" can't pair) and the second must END on a non-digit;
 * components must form a plausible phone canvas (64..640 x 64..800 — same
 * bounds as the manifest path), which also rejects junk like "2x2" or the
 * "1.1.5" version tail. Left-to-right scan, prefers the LAST valid pair
 * (multi-device packs list small variants first, e.g.
 * "Game 176x208 240x320.jar").
 */
static int jrs_scan_name_resolution(const char* base, int* out_w, int* out_h) {
    int found_w = 0, found_h = 0;
    const char* p = base;
    while (*p) {
        /* candidate digit run: must START at a non-digit (a preceding digit
         * means this run is the tail of a longer number) */
        if (*p >= '0' && *p <= '9' &&
            (p == base || p[-1] < '0' || p[-1] > '9')) {
            const char* ds = p;
            while (*p >= '0' && *p <= '9') p++;
            size_t dlen = (size_t)(p - ds);
            if (dlen <= 4 && (*p == 'x' || *p == 'X')) {
                const char* ds2 = p + 1;
                const char* de2 = ds2;
                while (*de2 >= '0' && *de2 <= '9') de2++;
                size_t dlen2 = (size_t)(de2 - ds2);
                /* right boundary: the second run must end on a non-digit */
                if (dlen2 >= 1 && dlen2 <= 4 && (*de2 < '0' || *de2 > '9')) {
                    int w = 0, h = 0;
                    for (size_t i = 0; i < dlen; i++)  w = w * 10 + (ds[i] - '0');
                    for (size_t i = 0; i < dlen2; i++) h = h * 10 + (ds2[i] - '0');
                    if (w >= 64 && w <= 640 && h >= 64 && h <= 800) {
                        found_w = w;            /* keep the LAST valid pair */
                        found_h = h;
                    }
                    p = de2;
                    continue;
                }
            }
            /* not a resolution: resume scanning after this digit run */
        } else {
            p++;
        }
        /* skip ahead past LETTERS only (v35.12 fix: skipping digits here
         * swallowed a run like "240x320" before the top-of-loop candidate
         * check ever saw it — every match failed). A digit must always be
         * re-examined as a candidate start at the loop top. */
        while (*p && jrs_is_alnum(*p) && (*p < '0' || *p > '9')) p++;
    }
    if (found_w && found_h) {
        *out_w = found_w;
        *out_h = found_h;
        return 1;
    }
    return 0;
}

int jar_detect_screen_size_from_name(const char* path, int* out_w, int* out_h) {
    if (!path || !path[0] || !out_w || !out_h) return 0;
    const char* base = path;
    for (const char* q = path; *q; q++) {
        if (*q == '/' || *q == '\\') base = q + 1;
    }
    return jrs_scan_name_resolution(base, out_w, out_h);
}

int jar_detect_screen_size(const void* jar_data, size_t jar_size,
                           int* out_w, int* out_h) {
    if (!jar_data || jar_size < 22 || !out_w || !out_h) return 0;
    size_t mf_size = 0;
    uint8_t* mf = jar_read_file(jar_data, jar_size, "META-INF/MANIFEST.MF", &mf_size);
    if (!mf) mf = jar_read_file(jar_data, jar_size, "META-INF/manifest.mf", &mf_size);
    if (!mf) return 0;

    char* unfolded = jar_manifest_unfold(mf, mf_size);
    free(mf);
    if (!unfolded) return 0;

    /* Priority order: the Nokia attribute is an explicit display-size
     * declaration; MIDxlet-Application-Range (SK Telecom) is the fallback.
     * Manifest attribute names are case-insensitive per the MIDP spec. */
    static const char* const s_keys[] = {
        "Nokia-MIDlet-Original-Display-Size",
        "MIDxlet-Application-Range",
    };

    int result = 0;
    for (size_t k = 0; k < sizeof(s_keys) / sizeof(s_keys[0]) && !result; k++) {
        /* Non-destructive line walk: strtok_r would NUL-out the '\n'
         * delimiters during the first key's pass and hide every line
         * after the first from the second key's pass. */
        const char* p = unfolded;
        while (*p && !result) {
            const char* nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            if (len >= 1024) len = 1023;   /* value inspection cap */

            char line[1024];
            memcpy(line, p, len);
            line[len] = '\0';

            const char* lp = line;
            while (*lp == ' ' || *lp == '\t') lp++;
            if (jrs_key_starts_with(lp, s_keys[k])) {
                const char* val = strchr(lp, ':');
                if (val) {
                    val++;   /* past ':' */
                    int a = 0, b = 0;
                    if (jrs_parse_int_pair(val, &a, &b) &&
                        a >= 64 && a <= 640 && b >= 64 && b <= 800) {
                        *out_w = a;
                        *out_h = b;
                        result = 1;
                    }
                }
            }

            p = nl ? nl + 1 : p + len;
        }
    }

    free(unfolded);
    return result;
}
