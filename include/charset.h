/*
 * charset.h — J2ME charset engine (v34.44)
 *
 * Implements the String/byte[] conversion surface of CLDC/MIDP with real
 * character-set semantics. Before v34.44 every byte->String path in the
 * emulator widened bytes to chars (Latin-1), so UTF-8 resources loaded by
 * games (e.g. Gish Reloaded [Rus] reads /t_zee.ru through
 * new String(bytes, "UTF-8")) rendered as mojibake: each UTF-8 byte became
 * its own character and was re-encoded on output.
 *
 * Supported encodings (alias matching is ASCII-case-insensitive):
 *   UTF-8 / UTF8 / unicode-1-1-utf-8   — strict decode, U+FFFD replacement
 *                                       on malformed input (resync 1 byte);
 *                                       C0 80 (modified-UTF-8 NUL) accepted
 *                                       as U+0000 for DataInput interop;
 *                                       CESU-8 surrogate pairs pass through.
 *   ISO-8859-1 / ISO8859-1 / latin1 / 8859_1 / cp1252 (approx)
 *   US-ASCII / ASCII
 *   windows-1251 / CP1251 / CP-1251    — Russian Windows charset
 *   KOI8-R / KOI8R
 *   UTF-16BE / UnicodeBigUnmarked, UTF-16LE / UnicodeLittleUnmarked,
 *   UTF-16 / UnicodeBig (BOM sniff, default BE)
 *
 * Unsupported names return JCS_UNSUPPORTED — String(byte[], String) then
 * throws java/io/UnsupportedEncodingException per CLDC 1.1 spec (games
 * catch it; before v34.44 the name was silently ignored).
 *
 * The platform DEFAULT encoding is UTF-8 (what most real handsets reported
 * through microedition.encoding), overridable with NOJME_DEFAULT_ENCODING
 * = utf8|iso-8859-1|cp1251|koi8-r|ascii (resolved by the caller in
 * native.c, which owns the getenv policy).
 */

#ifndef NOJME_CHARSET_H
#define NOJME_CHARSET_H

#include <stdint.h>
#include "heap.h"   /* jchar, jsize */

typedef enum {
    JCS_UNSUPPORTED = 0,
    JCS_UTF8,
    JCS_ISO8859_1,
    JCS_ASCII,
    JCS_CP1251,
    JCS_KOI8R,
    JCS_UTF16BE,
    JCS_UTF16LE,
    JCS_UTF16       /* BOM sniffing; without BOM defaults to BE */
} JCharsetId;

/* Match a charset name (case-insensitive, tolerant of '-'/'_' and the
 * most common J2ME spellings). Returns JCS_UNSUPPORTED if unknown. */
JCharsetId jcharset_match(const char* name);

/* Worst-case number of UTF-16 code units produced by decoding nbytes. */
jsize jcharset_decode_max_units(JCharsetId cs, jsize nbytes);

/* Worst-case number of bytes produced by encoding nunits UTF-16 code units. */
jsize jcharset_encode_max_bytes(JCharsetId cs, jsize nunits);

/* Decode bytes -> UTF-16 code units into dst (capacity dst_cap, in units).
 * Returns the number of units actually written; never writes past dst_cap.
 * Malformed sequences become U+FFFD and input resynchronises on the next
 * byte. A NULL dst with dst_cap==0 is a pure length probe. */
jsize jcharset_decode(JCharsetId cs, const uint8_t* src, jsize nbytes,
                      jchar* dst, jsize dst_cap);

/* Encode UTF-16 code units -> bytes into dst (capacity dst_cap bytes).
 * Returns the number of bytes actually written; never writes past
 * dst_cap. Characters unrepresentable in the target charset become '?'
 * (0x3F), matching java.lang.String.getBytes(String) semantics.
 * For JCS_UTF16 a BE BOM is NOT emitted (matching Java's
 * UnicodeBigUnmarked); only jcharset_match("UTF-16") callers that want
 * a BOM should prepend it themselves. */
jsize jcharset_encode(JCharsetId cs, const jchar* src, jsize nunits,
                      uint8_t* dst, jsize dst_cap);

/* Encode a single codepoint as modified UTF-8 (DataOutput.writeUTF rules:
 * U+0000 becomes C0 80, supplementary characters are NOT expected — caller
 * passes UTF-16 code units; a surrogate is encoded as its own 3-byte
 * CESU-8 sequence). Returns bytes written (0 if dst_cap too small).
 * dst_cap of 4 is always enough for one unit. */
jsize jcharset_modified_utf8_one(uint32_t cp, uint8_t* dst, jsize dst_cap);

/* Number of bytes modified UTF-8 needs for one UTF-16 code unit. */
jsize jcharset_modified_utf8_len(jchar c);

#endif /* NOJME_CHARSET_H */
