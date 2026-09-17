#pragma once

// Expands a slippy-map URL template, e.g. https://tile.openstreetmap.org/{z}/{x}/{y}.png, read from the
// .url file beside a style's tiles. Dependency-free (no Arduino, no SD) so the substitution is unit-tested.

#include <stddef.h>
#include <stdint.h>

namespace NicheGraphics::MapTiles::Fetch
{

// Writes the expanded URL into out. False - and out left empty - if the template is missing any of {z}/{x}/{y},
// isn't http(s), or the result would not fit, so a malformed .url can never produce a half-built request.
inline bool expandTileUrl(const char *tmpl, int z, int32_t x, int32_t y, char *out, size_t outSize)
{
    if (!tmpl || !out || outSize == 0)
        return false;
    out[0] = '\0';

    // Anything else is a typo or a scheme this build cannot speak; refuse rather than hand it to HTTPClient.
    const bool http = tmpl[0] == 'h' && tmpl[1] == 't' && tmpl[2] == 't' && tmpl[3] == 'p';
    const bool scheme = http && ((tmpl[4] == ':') || (tmpl[4] == 's' && tmpl[5] == ':'));
    if (!scheme)
        return false;

    bool seenZ = false, seenX = false, seenY = false;
    size_t w = 0;
    for (const char *p = tmpl; *p; p++) {
        long value = 0;
        if (p[0] == '{' && p[2] == '}' && (p[1] == 'z' || p[1] == 'x' || p[1] == 'y')) {
            if (p[1] == 'z') {
                value = z;
                seenZ = true;
            } else if (p[1] == 'x') {
                value = x;
                seenX = true;
            } else {
                value = y;
                seenY = true;
            }
            char digits[12];
            int n = 0;
            const bool negative = value < 0;
            unsigned long magnitude = negative ? (unsigned long)(-value) : (unsigned long)value;
            do {
                digits[n++] = (char)('0' + (magnitude % 10));
                magnitude /= 10;
            } while (magnitude && n < (int)sizeof(digits));
            if (negative)
                digits[n++] = '-';
            if (w + (size_t)n >= outSize) {
                out[0] = '\0';
                return false;
            }
            while (n-- > 0)
                out[w++] = digits[n];
            p += 2; // the } is consumed by the loop's own increment
            continue;
        }
        if (w + 1 >= outSize) {
            out[0] = '\0';
            return false;
        }
        out[w++] = *p;
    }
    out[w] = '\0';

    if (!seenZ || !seenX || !seenY) {
        out[0] = '\0';
        return false;
    }
    return true;
}

} // namespace NicheGraphics::MapTiles::Fetch
