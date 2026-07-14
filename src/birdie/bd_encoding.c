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
    return BD_ENC_UTF8;
}

const char *
bd_encoding_name(bd_encoding e)
{
    switch (e) {
    case BD_ENC_LATIN1: return "ISO-8859-1";
    case BD_ENC_CP1252: return "Windows-1252";
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
