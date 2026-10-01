/*
 * jar_reader.h — single canonical in-memory JAR/ZIP reader (v19).
 *
 * Replaces the four divergent hand-rolled ZIP scanners that used to live in
 * main.c, jvm.c, libretro/libretro.c and libretro/sdl_backend_stubs.c.
 * Built on miniz; see src/utils/jar_reader.c for the rationale.
 */

#ifndef JAR_READER_H
#define JAR_READER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Read one file from an in-memory JAR/ZIP image.
 *
 *   jar_data/jar_size  — the JAR image (as loaded into memory)
 *   name               — entry name, e.g. "res/bin/texture.res"
 *   out_size           — receives the exact byte count (may be NULL)
 *
 * Returns a malloc'd buffer of *out_size + 1 bytes (the extra byte is a NUL
 * terminator, not counted in *out_size) or NULL if the archive is not a
 * readable ZIP, the entry is absent, or extraction fails. Free with free().
 *
 * Lookup: exact case-sensitive match first, then a case-insensitive pass
 * (jars repacked on Windows often change letter case).
 */
uint8_t* jar_read_file(const void* jar_data, size_t jar_size,
                       const char* name, size_t* out_size);

/* v34.71: does the archive contain this entry? No extraction, no allocation.
 * Lets callers separate a GENUINE resource miss (safe to blacklist/throw)
 * from a transient extraction failure (must stay retryable — see the
 * display.c missing-image cache rationale). */
int jar_has_entry(const void* jar_data, size_t jar_size, const char* name);

/* v34.29 FIX (manifest line folding): JAR manifests (MIDP/CLDC spec, same
 * format as java.util.jar) allow a logical line to span multiple physical
 * lines: a physical line starting with a single space is a CONTINUATION —
 * the space is dropped and the remainder is appended to the previous
 * logical line. JBenchmark 3D's manifest folds its main class:
 *
 *   MIDlet-1: JBenchmark3D,/JBenchmark3D/3dlogo32.png,JBenchmark3D.JBenchm\r\n
 *   ark3D\r\n
 *
 * The old parsers split on strtok("\r\n") and read the class as the
 * truncated "…JBenchm" -> ClassNotFoundException at launch.
 *
 * Returns a malloc'd NUL-terminated string with folds joined (continuation
 * content concatenated directly after the previous line, no space) and all
 * newline runs (CRLF/LF/CRCR...) collapsed to a single '\n'. Free with
 * free(). Returns NULL on allocation failure.
 */
char* jar_manifest_unfold(const uint8_t* data, size_t size);

/* v34.46 (Treasure Towers): screen-size hints from the JAR manifest.
 *
 * WHY: fixed-resolution builds (2003-2005 era, e.g. Treasure Towers
 * [128x160]) hard-gate on Canvas.getWidth()/getHeight() and refuse to
 * start ("Error! Cannot start the game.") on any other size. The intended
 * size is recorded in vendor manifest attributes:
 *
 *   MIDxlet-Application-Range: 128, 160          (SK Telecom / LG builds)
 *   Nokia-MIDlet-Original-Display-Size: 176,208   (Nokia builds; landscape
 *                                                 uses "W,&,H")
 *
 * This helper reads the manifest out of an in-memory JAR, unfolds it and
 * extracts the first/last integer pair from the highest-priority hint
 * present. Bounds-checked against the libretro AV geometry limits
 * (max 640x800) so a corrupt/foreign attribute can never produce an
 * absurd canvas.
 *
 * Returns 1 and fills *out_w and *out_h on success, 0 when no valid hint
 * exists (the caller keeps its default resolution).
 */
int jar_detect_screen_size(const void* jar_data, size_t jar_size,
                           int* out_w, int* out_h);

/* v35.12: screen-size hint from the jar FILE NAME ("... 240x320.jar").
 * Fallback for jar_detect_screen_size(): scans the basename for the last
 * <digits>[xX]<digits> pair that forms a plausible phone canvas and fills
 * the output width/height. Returns 1 on success, 0 when the name carries
 * no size. */
int jar_detect_screen_size_from_name(const char* path, int* out_w, int* out_h);

#ifdef __cplusplus
}
#endif

#endif /* JAR_READER_H */
