#pragma once

#include <cstdint>
#include <string>

namespace WaypointUtils
{

inline std::string utf8FromCodepoint(uint32_t codepoint)
{
    if (codepoint == 0 || (codepoint >= 0xD800 && codepoint <= 0xDFFF) || codepoint > 0x10FFFF)
        return "";

    char buf[4];
    if (codepoint <= 0x7F) {
        buf[0] = static_cast<char>(codepoint);
        return std::string(buf, 1);
    }
    if (codepoint <= 0x7FF) {
        buf[0] = static_cast<char>(0xC0 | (codepoint >> 6));
        buf[1] = static_cast<char>(0x80 | (codepoint & 0x3F));
        return std::string(buf, 2);
    }
    if (codepoint <= 0xFFFF) {
        buf[0] = static_cast<char>(0xE0 | (codepoint >> 12));
        buf[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        buf[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
        return std::string(buf, 3);
    }

    buf[0] = static_cast<char>(0xF0 | (codepoint >> 18));
    buf[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
    buf[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    buf[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
    return std::string(buf, 4);
}

// The first codepoint of a UTF-8 string - a waypoint's icon is one codepoint, and an emote's label may carry more
// (a variation selector) after it. 0 for an empty or malformed string.
inline uint32_t codepointFromUtf8(const char *s)
{
    if (!s || !*s)
        return 0;
    const unsigned char c = (unsigned char)s[0];
    const int n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (!n)
        return 0;
    uint32_t cp = n == 1 ? c : c & (0x7F >> n);
    for (int i = 1; i < n; i++) {
        const unsigned char cc = (unsigned char)s[i];
        if ((cc & 0xC0) != 0x80)
            return 0;
        cp = (cp << 6) | (cc & 0x3F);
    }
    return cp;
}

} // namespace WaypointUtils
