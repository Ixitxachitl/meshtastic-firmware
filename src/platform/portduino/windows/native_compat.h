#pragma once

#ifdef _WIN32

#include <ctype.h>
#include <string.h>
#include <time.h>

static inline const char *portduino_strcasestr(const char *haystack, const char *needle)
{
    if (!haystack || !needle) {
        return NULL;
    }
    if (*needle == '\0') {
        return haystack;
    }

    const size_t needle_len = strlen(needle);
    for (const char *cursor = haystack; *cursor != '\0'; ++cursor) {
        size_t index = 0;
        while (index < needle_len && cursor[index] != '\0' && tolower((unsigned char)cursor[index]) == tolower((unsigned char)needle[index])) {
            ++index;
        }
        if (index == needle_len) {
            return cursor;
        }
    }
    return NULL;
}

static inline struct tm *portduino_localtime_r(const time_t *time_value, struct tm *result)
{
    if (!time_value || !result) {
        return NULL;
    }
    return localtime_s(result, time_value) == 0 ? result : NULL;
}

#define strcasestr portduino_strcasestr
#define localtime_r portduino_localtime_r

#endif