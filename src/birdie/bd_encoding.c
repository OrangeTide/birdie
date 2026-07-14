/*
 * bd_encoding.c -- legacy 8-bit to UTF-8 transcoding.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_encoding.h"
#include "bd_utf8.h"

#include <stdint.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */

/* Windows-1252 code points for 0x80-0x9F, the only bytes where it differs from
 * ISO-8859-1. Undefined slots map to the matching C1 control (the byte value),
 * following the WHATWG encoding standard. */
static const unsigned short cp1252_hi[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
};

/* IBM PC code page 437 glyphs for the C0 range 0x00-0x1F. On a real PC the
 * video hardware drew a glyph for every byte, so DOS BBS terminals (Telix,
 * Telemate, Terminate, and the ANSI-BBS lineage they codified) displayed these
 * as art: the smileys, card suits, arrows, and note symbols LORD-style games
 * lean on. The eight bytes with a genuine terminal-control function are the
 * exception (see cp437_ctrl); their slots here are 0 and never read. */
static const unsigned short cp437_lo[32] = {
    0x0000, 0x263A, 0x263B, 0x2665, 0x2666, 0x2663, 0x2660, 0x0000, /* 00-07 */
    0x0000, 0x0000, 0x0000, 0x2642, 0x0000, 0x0000, 0x266B, 0x263C, /* 08-0F */
    0x25BA, 0x25C4, 0x2195, 0x203C, 0x00B6, 0x00A7, 0x25AC, 0x21A8, /* 10-17 */
    0x2191, 0x2193, 0x2192, 0x0000, 0x221F, 0x2194, 0x25B2, 0x25BC  /* 18-1F */
};

/* The C0 bytes CP437 terminals kept as control functions rather than drawing
 * (ANSI-BBS): NUL, BEL, BS, HT, LF, FF, CR, ESC. Everything else in 0x00-0x1F
 * is rendered as its cp437_lo glyph, matching period DOS terminal behavior. */
static int
cp437_ctrl(unsigned char b)
{
    return b == 0x00 || b == 0x07 || b == 0x08 || b == 0x09 ||
           b == 0x0A || b == 0x0C || b == 0x0D || b == 0x1B;
}

/* IBM PC code page 437, bytes 0x80-0xFF: accented Latin, box-drawing, block
 * elements, and math/Greek glyphs, the vocabulary of DOS-era MUD/BBS art. */
static const unsigned short cp437_hi[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0
};

bd_encoding
bd_encoding_parse(const char *name)
{
    if (!name || !*name)
        return BD_ENC_UTF8;
    if (!strcasecmp(name, "utf-8") || !strcasecmp(name, "utf8"))
        return BD_ENC_UTF8;
    if (!strcasecmp(name, "iso-8859-1") || !strcasecmp(name, "iso8859-1") ||
        !strcasecmp(name, "latin1") || !strcasecmp(name, "latin-1") ||
        !strcasecmp(name, "8859-1"))
        return BD_ENC_LATIN1;
    if (!strcasecmp(name, "windows-1252") || !strcasecmp(name, "cp1252") ||
        !strcasecmp(name, "cp-1252") || !strcasecmp(name, "1252"))
        return BD_ENC_CP1252;
    if (!strcasecmp(name, "cp437") || !strcasecmp(name, "cp-437") ||
        !strcasecmp(name, "437") || !strcasecmp(name, "ibm437") ||
        !strcasecmp(name, "oem-us") || !strcasecmp(name, "dos"))
        return BD_ENC_CP437;
    return BD_ENC_UTF8;
}

const char *
bd_encoding_name(bd_encoding e)
{
    switch (e) {
    case BD_ENC_LATIN1: return "ISO-8859-1";
    case BD_ENC_CP1252: return "Windows-1252";
    case BD_ENC_CP437:  return "CP437";
    case BD_ENC_UTF8:
    default:            return "UTF-8";
    }
}

size_t
bd_encoding_decode(bd_encoding e, const unsigned char *src, size_t len,
                   unsigned char *dst, size_t dstcap)
{
    size_t i, out = 0;

    if (e == BD_ENC_UTF8) {
        size_t n = len < dstcap ? len : dstcap;
        memcpy(dst, src, n);
        return n;
    }
    for (i = 0; i < len; i++) {
        unsigned char b = src[i];
        uint32_t cp;

        /* CP437 also draws most C0 bytes (smileys, suits, arrows) as glyphs,
         * so it needs the whole 0x00-0xFF range, not just the high half. */
        if (e == BD_ENC_CP437) {
            if ((b >= 0x20 && b <= 0x7E) || (b < 0x20 && cp437_ctrl(b))) {
                if (out < dstcap)   /* plain ASCII, or a kept control code */
                    dst[out++] = b;
                continue;
            }
            cp = (b == 0x7F) ? 0x2302u          /* house */
               : (b < 0x20)  ? cp437_lo[b]
                             : cp437_hi[b - 0x80];
            if (out + BD_UTF8_MAX <= dstcap)
                out += (size_t)bd_utf8_encode(dst + out, cp);
            continue;
        }

        if (b < 0x80) {                 /* ASCII (and all control bytes) */
            if (out < dstcap)
                dst[out++] = b;
            continue;
        }
        if (e == BD_ENC_CP1252 && b < 0xA0)
            cp = cp1252_hi[b - 0x80];
        else
            cp = b;                     /* Latin-1, or CP1252 0xA0-0xFF */
        if (out + BD_UTF8_MAX <= dstcap)
            out += (size_t)bd_utf8_encode(dst + out, cp);
    }
    return out;
}
