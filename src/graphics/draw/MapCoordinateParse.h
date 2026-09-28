#pragma once

// Parses a typed position for the map's Navigate > Coordinates. Dependency-free so it is unit-tested.

#include <stddef.h>

namespace graphics::MapCoordinateParse
{

namespace detail
{

struct Token {
    enum Kind : unsigned char { Number, Hemisphere, Separator } kind;
    double value;
    bool negative, fractional;
    char hemisphere;
};

// Degrees, then optional minutes and seconds, and the hemisphere letter if one was given.
struct Group {
    double parts[3];
    int count;
    bool negative, lastFractional;
    char hemisphere;
    bool closed; // a trailing letter ended it
};

inline char upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

// Anything that isn't a number, N/S/E/W or a comma is punctuation (spaces, degree signs, quotes); any other letter
// means this isn't a position at all.
inline int tokenize(const char *text, Token *out, int max)
{
    int n = 0;
    for (const char *p = text; *p;) {
        const char c = *p;
        const bool sign = (c == '-' || c == '+') && ((p[1] >= '0' && p[1] <= '9') || p[1] == '.');
        if (sign || (c >= '0' && c <= '9') || c == '.') {
            if (n == max)
                return -1;
            Token t{Token::Number, 0.0, c == '-', false, 0};
            if (sign)
                p++;
            bool digits = false;
            while (*p >= '0' && *p <= '9') {
                t.value = t.value * 10.0 + (*p++ - '0');
                digits = true;
            }
            if (*p == '.') {
                p++;
                double scale = 0.1;
                while (*p >= '0' && *p <= '9') {
                    t.value += (*p++ - '0') * scale;
                    scale *= 0.1;
                    digits = true;
                    t.fractional = true;
                }
            }
            if (!digits)
                return -1;
            out[n++] = t;
            continue;
        }
        const char u = upper(c);
        if (u == 'N' || u == 'S' || u == 'E' || u == 'W') {
            if (n == max)
                return -1;
            out[n++] = Token{Token::Hemisphere, 0.0, false, false, u};
        } else if (c == ',' || c == ';') {
            if (n == max)
                return -1;
            out[n++] = Token{Token::Separator, 0.0, false, false, 0};
        } else if (u >= 'A' && u <= 'Z') {
            return -1;
        }
        p++;
    }
    return n;
}

inline bool addNumber(Group &g, const Token &t)
{
    if (g.count == 3)
        return false;
    if (g.count > 0 && t.negative) // only the degrees carry a sign
        return false;
    if (g.closed || (g.count > 0 && g.lastFractional))
        return false; // only the last part may have a fraction: 39 12.5 is fine, 39.5 12 is not
    if (g.count == 0)
        g.negative = t.negative;
    g.parts[g.count++] = t.value;
    g.lastFractional = t.fractional;
    return true;
}

inline bool toDegrees(const Group &g, double &out)
{
    if (g.count == 0)
        return false;
    for (int i = 1; i < g.count; i++) {
        if (g.parts[i] >= 60.0)
            return false;
    }
    if (g.negative && g.hemisphere) // "-39 S" says the hemisphere twice
        return false;
    double v = g.parts[0] + (g.count > 1 ? g.parts[1] / 60.0 : 0.0) + (g.count > 2 ? g.parts[2] / 3600.0 : 0.0);
    if (g.negative || g.hemisphere == 'S' || g.hemisphere == 'W')
        v = -v;
    out = v;
    return true;
}

} // namespace detail

// Accepts decimal degrees ("39.2, -121.3"), hemisphere letters before or after ("39.2N 121.3W", "N39.2 W121.3") and
// degrees-minutes(-seconds) with any punctuation between them ("39°12'30"N 121°18'W", "39 12.5, -121 18"). Latitude
// first unless the letters say otherwise. False, with lat/lon untouched, for anything ambiguous or out of range.
inline bool parse(const char *text, double &lat, double &lon)
{
    using namespace detail;
    if (!text)
        return false;
    Token tokens[16];
    const int n = tokenize(text, tokens, 16);
    if (n <= 0)
        return false;

    int separators = 0, hemispheres = 0, numbers = 0;
    for (int i = 0; i < n; i++) {
        separators += tokens[i].kind == Token::Separator;
        hemispheres += tokens[i].kind == Token::Hemisphere;
        numbers += tokens[i].kind == Token::Number;
    }
    if (separators > 1 || hemispheres > 2 || numbers < 2)
        return false;

    Group groups[2] = {};
    int g = 0;
    if (separators == 0 && hemispheres == 0) {
        // Bare numbers split evenly: "39.2 -121.3", "39 12 -121 18", "39 12 30 121 18 0".
        if (numbers % 2 != 0 || numbers > 6)
            return false;
        for (int i = 0; i < n; i++) {
            if (!addNumber(groups[i < numbers / 2 ? 0 : 1], tokens[i]))
                return false;
        }
    } else {
        // A comma splits the two; otherwise the letters do, leading ("N39 W121") or trailing ("39N 121W").
        const bool leading = separators == 0 && tokens[0].kind == Token::Hemisphere;
        for (int i = 0; i < n; i++) {
            const Token &t = tokens[i];
            if (t.kind == Token::Number) {
                if (!addNumber(groups[g], t))
                    return false;
                continue;
            }
            if (t.kind == Token::Separator) {
                if (g == 1 || groups[0].count == 0)
                    return false;
                g = 1;
                continue;
            }
            if (leading && (groups[g].count > 0 || groups[g].hemisphere)) { // this letter opens the second
                if (g == 1)
                    return false;
                g = 1;
            }
            if (groups[g].hemisphere)
                return false;
            groups[g].hemisphere = t.hemisphere;
            if (!leading && groups[g].count > 0) { // trailing: the letter ends its group
                groups[g].closed = true;
                if (separators == 0) {
                    if (g == 1 && i != n - 1)
                        return false;
                    g = 1;
                }
            }
        }
        if (groups[0].count == 0 || groups[1].count == 0)
            return false;
    }

    double a, b;
    if (!toDegrees(groups[0], a) || !toDegrees(groups[1], b))
        return false;

    // Letters name the axis; with none, latitude comes first.
    auto isLon = [](char h) { return h == 'E' || h == 'W'; };
    auto isLat = [](char h) { return h == 'N' || h == 'S'; };
    const char h0 = groups[0].hemisphere, h1 = groups[1].hemisphere;
    if ((h0 && h1 && isLat(h0) == isLat(h1)))
        return false;
    const bool swapped = isLon(h0) || isLat(h1);
    const double newLat = swapped ? b : a, newLon = swapped ? a : b;
    if (newLat < -90.0 || newLat > 90.0 || newLon < -180.0 || newLon > 180.0)
        return false;
    lat = newLat;
    lon = newLon;
    return true;
}

} // namespace graphics::MapCoordinateParse
