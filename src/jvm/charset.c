/*
 * charset.c — J2ME charset engine implementation (v34.44)
 * See include/charset.h for the design contract.
 *
 * NOTE: pure C99, no POSIX dependencies (must build under MSYS2/MinGW and
 * the m17 cross toolchain).
 *
 * v36.53 [LENIENT-UTF8] (восстановлено в v36.62 после потери исходников
 * v36.53-v36.56): ветка JCS_UTF8 декодирует MALFORMED-вход по семантике
 * KVM телефонов 2005 г. — осиротевший lead-байт (нет continuation),
 * одиночный continuation-байт (0x80-0xBF) и lead 0xF8-0xFF проходят
 * НАСКВОЗЬ как (char)b (byte-to-char widening), а НЕ U+FFFD. Класс игр
 * с побайтовыми шифрами-заменителями (Doom RPG [Rus] Corax89: латинские
 * двойники + переиспользованные слоты 0xC0-0xFC extended-charset)
 * строит строки через new String(byte[],int,int) и ждёт 1:1 widening —
 * строгий UTF-8 превращал каждый байт >= 0xC0 (lead без continuation в
 * этом шифре) в U+FFFD, и буквы «пропадали» (глиф-атлас не имеет слота
 * для U+FFFD — буква становилась невидимой). Валидные последовательности,
 * C0 80 -> U+0000 (modified UTF-8), overlong-формы -> U+FFFD, все
 * остальные charset'ы и энкодеры — НЕ ИЗМЕНЕНЫ.
 */

#include "charset.h"
#include <string.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Single-byte charset tables (byte + 0x80 -> Unicode),               */
/* 0xFFFD marks bytes undefined in the charset.                        */
/* Generated from the reference Python codec tables.                   */
/* ------------------------------------------------------------------ */

static const unsigned short jcs_cp1251_to_uni[128] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021,
    0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0xFFFD, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7,
    0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,
    0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,
    0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,
    0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437,
    0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447,
    0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,
};

static const unsigned short jcs_koi8r_to_uni[128] = {
    0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524,
    0x252C, 0x2534, 0x253C, 0x2580, 0x2584, 0x2588, 0x258C, 0x2590,
    0x2591, 0x2592, 0x2593, 0x2320, 0x25A0, 0x2219, 0x221A, 0x2248,
    0x2264, 0x2265, 0x00A0, 0x2321, 0x00B0, 0x00B2, 0x00B7, 0x00F7,
    0x2550, 0x2551, 0x2552, 0x0451, 0x2553, 0x2554, 0x2555, 0x2556,
    0x2557, 0x2558, 0x2559, 0x255A, 0x255B, 0x255C, 0x255D, 0x255E,
    0x255F, 0x2560, 0x2561, 0x0401, 0x2562, 0x2563, 0x2564, 0x2565,
    0x2566, 0x2567, 0x2568, 0x2569, 0x256A, 0x256B, 0x256C, 0x00A9,
    0x044E, 0x0430, 0x0431, 0x0446, 0x0434, 0x0435, 0x0444, 0x0433,
    0x0445, 0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E,
    0x043F, 0x044F, 0x0440, 0x0441, 0x0442, 0x0443, 0x0436, 0x0432,
    0x044C, 0x044B, 0x0437, 0x0448, 0x044D, 0x0449, 0x0447, 0x044A,
    0x042E, 0x0410, 0x0411, 0x0426, 0x0414, 0x0415, 0x0424, 0x0413,
    0x0425, 0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E,
    0x041F, 0x042F, 0x0420, 0x0421, 0x0422, 0x0423, 0x0416, 0x0412,
    0x042C, 0x042B, 0x0417, 0x0428, 0x042D, 0x0429, 0x0427, 0x042A,
};

/* Case-insensitive ASCII comparison (locale-free). */
static int jcs_ieq(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

JCharsetId jcharset_match(const char* name) {
    if (!name || !*name) return JCS_UNSUPPORTED;

    /* --- UTF-8 family --- */
    if (jcs_ieq(name, "UTF-8") || jcs_ieq(name, "UTF8") ||
        jcs_ieq(name, "utf")  || jcs_ieq(name, "unicode-1-1-utf-8"))
        return JCS_UTF8;

    /* --- ISO-8859-1 family (cp1252/latin1 are close enough for J2ME) --- */
    if (jcs_ieq(name, "ISO-8859-1") || jcs_ieq(name, "ISO8859-1") ||
        jcs_ieq(name, "ISO8859_1")  || jcs_ieq(name, "8859_1")    ||
        jcs_ieq(name, "8859-1")     || jcs_ieq(name, "latin1")    ||
        jcs_ieq(name, "LATIN1")      || jcs_ieq(name, "ISO_8859-1")||
        jcs_ieq(name, "cp1252")      || jcs_ieq(name, "windows-1252") ||
        jcs_ieq(name, "windows-1252") || jcs_ieq(name, "Cp1252") ||
        jcs_ieq(name, "CP1252"))
        return JCS_ISO8859_1;

    /* --- ASCII --- */
    if (jcs_ieq(name, "US-ASCII") || jcs_ieq(name, "ASCII") ||
        jcs_ieq(name, "USASCII")  || jcs_ieq(name, "ANSI_X3.4-1968") ||
        jcs_ieq(name, "646"))
        return JCS_ASCII;

    /* --- Windows-1251 (Russian) --- */
    if (jcs_ieq(name, "windows-1251") || jcs_ieq(name, "cp1251") ||
        jcs_ieq(name, "CP1251")       || jcs_ieq(name, "cp-1251") ||
        jcs_ieq(name, "Cp1251")       || jcs_ieq(name, "windows1251"))
        return JCS_CP1251;

    /* --- KOI8-R --- */
    if (jcs_ieq(name, "KOI8-R") || jcs_ieq(name, "KOI8R") ||
        jcs_ieq(name, "koi8r")  || jcs_ieq(name, "KOI-8-R"))
        return JCS_KOI8R;

    /* --- UTF-16 variants --- */
    if (jcs_ieq(name, "UTF-16BE") || jcs_ieq(name, "UnicodeBigUnmarked") ||
        jcs_ieq(name, "UTF16BE")  || jcs_ieq(name, "X-UTF-16BE"))
        return JCS_UTF16BE;
    if (jcs_ieq(name, "UTF-16LE") || jcs_ieq(name, "UnicodeLittleUnmarked") ||
        jcs_ieq(name, "UTF16LE")  || jcs_ieq(name, "X-UTF-16LE"))
        return JCS_UTF16LE;
    if (jcs_ieq(name, "UTF-16") || jcs_ieq(name, "UTF16") ||
        jcs_ieq(name, "UnicodeBig"))
        return JCS_UTF16;

    return JCS_UNSUPPORTED;
}

jsize jcharset_decode_max_units(JCharsetId cs, jsize nbytes) {
    switch (cs) {
        case JCS_UTF16BE:
        case JCS_UTF16LE:
            return nbytes / 2 + 1;          /* +1: odd tail byte ignored */
        case JCS_UTF16:
            return nbytes / 2 + 1;          /* BOM may eat 2 bytes; +1 slack */
        case JCS_UTF8:
            return nbytes;                   /* worst case 1 unit per byte */
        default:
            return nbytes;                   /* single-byte charsets */
    }
}

jsize jcharset_encode_max_bytes(JCharsetId cs, jsize nunits) {
    switch (cs) {
        case JCS_UTF16BE:
        case JCS_UTF16LE:
        case JCS_UTF16:
            return nunits * 2;
        case JCS_UTF8:
            return nunits * 3;               /* BMP: max 3 bytes/unit */
        default:
            return nunits;                   /* single-byte charsets */
    }
}

/* Append one unit to dst, respecting capacity. Returns 1 if written. */
static jsize jcs_put(jchar* dst, jsize cap, jsize* at, jchar c) {
    if (*at < cap) {
        dst[*at] = c;
        (*at)++;
        return 1;
    }
    return 0;
}

jsize jcharset_decode(JCharsetId cs, const uint8_t* src, jsize nbytes,
                      jchar* dst, jsize dst_cap) {
    jsize out = 0;
    jsize i = 0;

    if (!src && nbytes > 0) return 0;

    /* BOM sniffing for plain "UTF-16" (and UTF-16BE/LE tolerate a BOM too,
     * consuming it, which matches Java's behaviour). */
    if ((cs == JCS_UTF16 || cs == JCS_UTF16BE || cs == JCS_UTF16LE) &&
        nbytes >= 2 && src[0] == 0xFE && src[1] == 0xFF) {
        if (cs == JCS_UTF16LE) { /* LE declared but BE BOM — trust the BOM */ }
        cs = JCS_UTF16BE;
        i = 2;
    } else if ((cs == JCS_UTF16 || cs == JCS_UTF16LE) &&
               nbytes >= 2 && src[0] == 0xFF && src[1] == 0xFE) {
        cs = JCS_UTF16LE;
        i = 2;
    } else if (cs == JCS_UTF16) {
        cs = JCS_UTF16BE;                    /* no BOM: default big-endian */
    }

    switch (cs) {
        case JCS_UTF8: {
            while (i < nbytes) {
                uint8_t b = src[i];
                if (b < 0x80) {
                    jcs_put(dst, dst_cap, &out, (jchar)b);
                    i++;
                } else if ((b & 0xE0) == 0xC0) {
                    if (i + 1 < nbytes && (src[i+1] & 0xC0) == 0x80) {
                        uint32_t cp = ((uint32_t)(b & 0x1F) << 6) |
                                     (uint32_t)(src[i+1] & 0x3F);
                        /* Modified-UTF-8 leniency: C0 80 == U+0000.
                         * Other overlong forms are malformed -> U+FFFD. */
                        if (cp >= 0x80 || (b == 0xC0 && src[i+1] == 0x80)) {
                            jcs_put(dst, dst_cap, &out, (jchar)cp);
                        } else {
                            jcs_put(dst, dst_cap, &out, 0xFFFD);
                        }
                        i += 2;
                    } else {
                        /* [LENIENT-UTF8]: orphaned 2-byte lead (no
                         * continuation available) — pass through as
                         * (char)b (KVM widening), resync 1 byte. */
                        jcs_put(dst, dst_cap, &out, (jchar)b);
                        i++;
                    }
                } else if ((b & 0xF0) == 0xE0) {
                    if (i + 2 < nbytes && (src[i+1] & 0xC0) == 0x80 &&
                        (src[i+2] & 0xC0) == 0x80) {
                        uint32_t cp = ((uint32_t)(b & 0x0F) << 12) |
                                     ((uint32_t)(src[i+1] & 0x3F) << 6) |
                                      (uint32_t)(src[i+2] & 0x3F);
                        /* Overlong / beyond BMP are malformed here. */
                        if (cp >= 0x800) {
                            jcs_put(dst, dst_cap, &out, (jchar)cp);
                        } else {
                            jcs_put(dst, dst_cap, &out, 0xFFFD);
                        }
                        i += 3;
                    } else {
                        /* [LENIENT-UTF8]: orphaned 3-byte lead. */
                        jcs_put(dst, dst_cap, &out, (jchar)b);
                        i++;
                    }
                } else if ((b & 0xF8) == 0xF0) {
                    if (i + 3 < nbytes && (src[i+1] & 0xC0) == 0x80 &&
                        (src[i+2] & 0xC0) == 0x80 && (src[i+3] & 0xC0) == 0x80) {
                        uint32_t cp = ((uint32_t)(b & 0x07) << 18) |
                                     ((uint32_t)(src[i+1] & 0x3F) << 12) |
                                     ((uint32_t)(src[i+2] & 0x3F) << 6) |
                                      (uint32_t)(src[i+3] & 0x3F);
                        if (cp >= 0x10000 && cp <= 0x10FFFF) {
                            cp -= 0x10000;
                            if (jcs_put(dst, dst_cap, &out,
                                        (jchar)(0xD800 | (cp >> 10))))
                                jcs_put(dst, dst_cap, &out,
                                        (jchar)(0xDC00 | (cp & 0x3FF)));
                        } else {
                            jcs_put(dst, dst_cap, &out, 0xFFFD);
                        }
                        i += 4;
                    } else {
                        /* [LENIENT-UTF8]: orphaned 4-byte lead. */
                        jcs_put(dst, dst_cap, &out, (jchar)b);
                        i++;
                    }
                } else {
                    /* [LENIENT-UTF8]: lone continuation byte (0x80-0xBF)
                     * or invalid lead 0xF8-0xFF — pass through as (char)b
                     * (KVM widening). До v36.53 оба случая давали U+FFFD. */
                    jcs_put(dst, dst_cap, &out, (jchar)b);
                    i++;
                }
            }
            break;
        }

        case JCS_ISO8859_1: {
            while (i < nbytes) {
                jcs_put(dst, dst_cap, &out, (jchar)src[i]);
                i++;
            }
            break;
        }

        case JCS_ASCII: {
            while (i < nbytes) {
                jcs_put(dst, dst_cap, &out,
                        (src[i] < 0x80) ? (jchar)src[i] : (jchar)0xFFFD);
                i++;
            }
            break;
        }

        case JCS_CP1251: {
            while (i < nbytes) {
                uint8_t b = src[i];
                if (b < 0x80) {
                    jcs_put(dst, dst_cap, &out, (jchar)b);
                } else {
                    unsigned short u = jcs_cp1251_to_uni[b - 0x80];
                    jcs_put(dst, dst_cap, &out, (u == 0xFFFD) ? (jchar)0xFFFD
                                                             : (jchar)u);
                }
                i++;
            }
            break;
        }

        case JCS_KOI8R: {
            while (i < nbytes) {
                uint8_t b = src[i];
                if (b < 0x80) {
                    jcs_put(dst, dst_cap, &out, (jchar)b);
                } else {
                    unsigned short u = jcs_koi8r_to_uni[b - 0x80];
                    jcs_put(dst, dst_cap, &out, (u == 0xFFFD) ? (jchar)0xFFFD
                                                             : (jchar)u);
                }
                i++;
            }
            break;
        }

        case JCS_UTF16BE:
        case JCS_UTF16LE: {
            int be = (cs == JCS_UTF16BE);
            while (i + 1 < nbytes) {
                jchar c = be ? (jchar)(((uint16_t)src[i] << 8) | src[i+1])
                             : (jchar)(((uint16_t)src[i+1] << 8) | src[i]);
                jcs_put(dst, dst_cap, &out, c);
                i += 2;
            }
            /* Odd trailing byte is ignored (Java truncates similarly). */
            break;
        }

        default:
            return 0;   /* JCS_UNSUPPORTED — caller decides policy */
    }

    return out;
}

/* Reverse lookup: which byte encodes codepoint cp in this single-byte
 * charset? Returns -1 if unmappable. */
static int jcs_sb_from_uni(JCharsetId cs, jchar cp) {
    const unsigned short* tbl;
    int i;

    if (cp < 0x80) return (int)cp;

    tbl = (cs == JCS_CP1251) ? jcs_cp1251_to_uni : jcs_koi8r_to_uni;
    for (i = 0; i < 128; i++) {
        if (tbl[i] == (unsigned short)cp && tbl[i] != 0xFFFD) return 0x80 + i;
    }
    return -1;
}

jsize jcharset_encode(JCharsetId cs, const jchar* src, jsize nunits,
                      uint8_t* dst, jsize dst_cap) {
    jsize out = 0;
    jsize i;

    if (!src && nunits > 0) return 0;

#define JCS_EMIT(byte) do { \
        if (out < dst_cap) { dst[out] = (uint8_t)(byte); out++; } \
    } while (0)

    switch (cs) {
        case JCS_UTF8: {
            for (i = 0; i < nunits; i++) {
                jchar c = src[i];
                if (c < 0x80) {
                    JCS_EMIT(c);
                } else if (c < 0x800) {
                    JCS_EMIT(0xC0 | (c >> 6));
                    JCS_EMIT(0x80 | (c & 0x3F));
                } else {
                    /* 3-byte (surrogates encoded as-is, CESU-8 style —
                     * matches string_utf8() and real KVM round-trips). */
                    JCS_EMIT(0xE0 | (c >> 12));
                    JCS_EMIT(0x80 | ((c >> 6) & 0x3F));
                    JCS_EMIT(0x80 | (c & 0x3F));
                }
            }
            break;
        }

        case JCS_ISO8859_1: {
            for (i = 0; i < nunits; i++) {
                jchar c = src[i];
                if (c <= 0xFF) JCS_EMIT(c);
                else           JCS_EMIT('?');
            }
            break;
        }

        case JCS_ASCII: {
            for (i = 0; i < nunits; i++) {
                jchar c = src[i];
                if (c < 0x80) JCS_EMIT(c);
                else           JCS_EMIT('?');
            }
            break;
        }

        case JCS_CP1251:
        case JCS_KOI8R: {
            for (i = 0; i < nunits; i++) {
                int b = jcs_sb_from_uni(cs, src[i]);
                if (b >= 0) JCS_EMIT(b);
                else        JCS_EMIT('?');
            }
            break;
        }

        case JCS_UTF16BE: {
            for (i = 0; i < nunits; i++) {
                JCS_EMIT(src[i] >> 8);
                JCS_EMIT(src[i] & 0xFF);
            }
            break;
        }

        case JCS_UTF16LE: {
            for (i = 0; i < nunits; i++) {
                JCS_EMIT(src[i] & 0xFF);
                JCS_EMIT(src[i] >> 8);
            }
            break;
        }

        case JCS_UTF16: {
            /* Treated as BE without BOM (see header contract). */
            for (i = 0; i < nunits; i++) {
                JCS_EMIT(src[i] >> 8);
                JCS_EMIT(src[i] & 0xFF);
            }
            break;
        }

        default:
            return 0;
    }

#undef JCS_EMIT
    return out;
}

jsize jcharset_modified_utf8_len(jchar c) {
    if (c == 0) return 2;          /* C0 80 */
    if (c < 0x80) return 1;
    if (c < 0x800) return 2;
    return 3;                      /* incl. surrogates (CESU-8) */
}

jsize jcharset_modified_utf8_one(uint32_t cp, uint8_t* dst, jsize dst_cap) {
    jsize n = 0;
    if (cp == 0) {
        if (n < dst_cap) { dst[n++] = 0xC0; }
        if (n < dst_cap) { dst[n++] = 0x80; }
        return n;
    }
    if (cp < 0x80) {
        if (n < dst_cap) { dst[n++] = (uint8_t)cp; }
        return n;
    }
    if (cp < 0x800) {
        if (n < dst_cap) { dst[n++] = (uint8_t)(0xC0 | (cp >> 6)); }
        if (n < dst_cap) { dst[n++] = (uint8_t)(0x80 | (cp & 0x3F)); }
        return n;
    }
    if (n < dst_cap) { dst[n++] = (uint8_t)(0xE0 | (cp >> 12)); }
    if (n < dst_cap) { dst[n++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); }
    if (n < dst_cap) { dst[n++] = (uint8_t)(0x80 | (cp & 0x3F)); }
    return n;
}
