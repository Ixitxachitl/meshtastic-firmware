#pragma once

// A minimal JSON reader for the small, trusted-shape replies the map fetches (address search, routes): walks a
// buffer in place, with no allocation and no library. Every read fails closed on anything malformed.

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

namespace NicheGraphics::MapTiles::Json
{

struct Cursor {
    const char *p, *end;
    bool ok() const { return p < end; }
    void skipSpace()
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            p++;
    }
    bool take(char c)
    {
        skipSpace();
        if (p < end && *p == c) {
            p++;
            return true;
        }
        return false;
    }
};

inline int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// Reads a JSON string at the cursor into out (UTF-8, truncated on a character boundary; out may be null to skip).
inline bool readString(Cursor &c, char *out, size_t outSize)
{
    c.skipSpace();
    if (!c.ok() || *c.p != '"')
        return false;
    c.p++;
    size_t w = 0;
    bool full = false;
    auto put = [&](const char *bytes, size_t n) {
        if (!out || full)
            return;
        if (w + n >= outSize) {
            full = true; // whole characters only, so a cut never leaves half a UTF-8 sequence
            return;
        }
        memcpy(out + w, bytes, n);
        w += n;
    };
    while (c.ok() && *c.p != '"') {
        char ch = *c.p++;
        if (ch != '\\') {
            // Copy a raw UTF-8 sequence whole.
            const unsigned char lead = (unsigned char)ch;
            size_t n = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 1;
            if ((size_t)(c.end - (c.p - 1)) < n)
                return false;
            put(c.p - 1, n);
            c.p += n - 1;
            continue;
        }
        if (!c.ok())
            return false;
        const char esc = *c.p++;
        char simple = 0;
        switch (esc) {
        case '"':
        case '\\':
        case '/':
            simple = esc;
            break;
        case 'b':
        case 'f':
        case 'n':
        case 'r':
        case 't':
            simple = ' ';
            break;
        case 'u': {
            if (c.end - c.p < 4)
                return false;
            unsigned cp = 0;
            for (int i = 0; i < 4; i++) {
                const int v = hexValue(c.p[i]);
                if (v < 0)
                    return false;
                cp = cp << 4 | (unsigned)v;
            }
            c.p += 4;
            char enc[3];
            if (cp >= 0xD800 && cp <= 0xDFFF) { // a surrogate pair's half: not worth a font glyph
                put("?", 1);
            } else if (cp < 0x80) {
                enc[0] = (char)cp;
                put(enc, 1);
            } else if (cp < 0x800) {
                enc[0] = (char)(0xC0 | cp >> 6);
                enc[1] = (char)(0x80 | (cp & 0x3F));
                put(enc, 2);
            } else {
                enc[0] = (char)(0xE0 | cp >> 12);
                enc[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                enc[2] = (char)(0x80 | (cp & 0x3F));
                put(enc, 3);
            }
            continue;
        }
        default:
            return false;
        }
        put(&simple, 1);
    }
    if (!c.ok())
        return false;
    c.p++; // closing quote
    if (out)
        out[w] = '\0';
    return true;
}

// Skips any JSON value, nested or not.
inline bool skipValue(Cursor &c)
{
    c.skipSpace();
    if (!c.ok())
        return false;
    if (*c.p == '"')
        return readString(c, nullptr, 0);
    if (*c.p == '{' || *c.p == '[') {
        int depth = 0;
        while (c.ok()) {
            const char ch = *c.p;
            if (ch == '"') {
                if (!readString(c, nullptr, 0))
                    return false;
                continue;
            }
            c.p++;
            if (ch == '{' || ch == '[')
                depth++;
            else if ((ch == '}' || ch == ']') && --depth == 0)
                return true;
        }
        return false;
    }
    while (c.ok() && *c.p != ',' && *c.p != '}' && *c.p != ']') // number, true, false, null
        c.p++;
    return true;
}

// A number given bare or as a JSON string ("39.2", as Nominatim sends coordinates).
inline bool readNumber(Cursor &c, double &out)
{
    c.skipSpace();
    char buf[32];
    if (c.ok() && *c.p == '"') {
        if (!readString(c, buf, sizeof(buf)))
            return false;
    } else {
        size_t n = 0;
        while (c.ok() && n + 1 < sizeof(buf) && *c.p != ',' && *c.p != '}' && *c.p != ']' && *c.p != ' ')
            buf[n++] = *c.p++;
        buf[n] = '\0';
    }
    char *endp = nullptr;
    out = strtod(buf, &endp);
    return endp != buf && *endp == '\0';
}

// The raw span of the string at the cursor, quotes excluded, escapes left in.
inline bool stringSpan(Cursor &c, const char *&start, size_t &len)
{
    c.skipSpace();
    if (!c.ok() || *c.p != '"')
        return false;
    start = ++c.p;
    while (c.ok() && *c.p != '"') {
        if (*c.p == '\\')
            c.p++;
        c.p++;
    }
    if (!c.ok())
        return false;
    len = (size_t)(c.p - start);
    c.p++;
    return true;
}

// Calls field(key) for each key of the object at the cursor; field must consume the value.
template <typename Field> bool eachField(Cursor &c, Field field)
{
    if (!c.take('{'))
        return false;
    if (c.take('}'))
        return true;
    do {
        char key[32];
        if (!readString(c, key, sizeof(key)) || !c.take(':') || !field(key))
            return false;
    } while (c.take(','));
    return c.take('}');
}

// Calls item(index) for each element of the array at the cursor; item must consume the element.
template <typename Item> bool eachItem(Cursor &c, Item item)
{
    if (!c.take('['))
        return false;
    if (c.take(']'))
        return true;
    int index = 0;
    do {
        if (!item(index++))
            return false;
    } while (c.take(','));
    return c.take(']');
}

} // namespace NicheGraphics::MapTiles::Json
