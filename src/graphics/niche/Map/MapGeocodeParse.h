#pragma once

// Address search for the map's Navigate > Address: the request URL and the parse of a Nominatim-style reply.
// Dependency-free (no Arduino, no JSON library) so it is unit-tested.

#include "./MapJsonScan.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace NicheGraphics::MapTiles::Geocode
{

// OpenStreetMap's own search. A card file /maps/.geocode may name another Nominatim-compatible server.
constexpr const char *kDefaultSearchUrl = "https://nominatim.openstreetmap.org/search?format=jsonv2&limit=5&q={q}";
// Tried in turn when the one before finds nothing. OpenStreetMap often lacks house numbers, or files a street under
// no city at all; the US Census geocoder has every US address range, and Photon matches loosely and finds places.
constexpr const char *kCensusSearchUrl =
    "https://geocoding.geo.census.gov/geocoder/locations/onelineaddress?address={q}&benchmark=Public_AR_Current&format=json";
constexpr const char *kPhotonSearchUrl = "https://photon.komoot.io/api/?q={q}&limit=5";

enum class Provider : uint8_t { Nominatim, Census, Photon };
constexpr int kMaxResults = 5;

struct Result {
    double lat, lon;
    char name[64];
};

// Percent-encodes a query for a URL. False, and out left empty, if it would not fit.
inline bool encodeQuery(const char *in, char *out, size_t outSize)
{
    if (!in || !out || outSize == 0)
        return false;
    static const char kHex[] = "0123456789ABCDEF";
    size_t w = 0;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(in); *p; p++) {
        const unsigned char c = *p;
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                           c == '_' || c == '~';
        const size_t need = plain ? 1 : 3;
        if (w + need >= outSize) {
            out[0] = '\0';
            return false;
        }
        if (plain) {
            out[w++] = (char)c;
        } else {
            out[w++] = '%';
            out[w++] = kHex[c >> 4];
            out[w++] = kHex[c & 0xF];
        }
    }
    out[w] = '\0';
    return true;
}

// Substitutes every {q} in the template with the encoded query. False, and out left empty, if the template has no
// {q}, isn't http(s), or the result would not fit.
inline bool expandSearchUrl(const char *tmpl, const char *encodedQuery, char *out, size_t outSize)
{
    if (!tmpl || !encodedQuery || !out || outSize == 0)
        return false;
    out[0] = '\0';
    if (strncmp(tmpl, "http://", 7) != 0 && strncmp(tmpl, "https://", 8) != 0)
        return false;
    const size_t qLen = strlen(encodedQuery);
    bool seenQ = false;
    size_t w = 0;
    for (const char *p = tmpl; *p; p++) {
        if (p[0] == '{' && p[1] == 'q' && p[2] == '}') {
            if (w + qLen >= outSize) {
                out[0] = '\0';
                return false;
            }
            memcpy(out + w, encodedQuery, qLen);
            w += qLen;
            p += 2;
            seenQ = true;
            continue;
        }
        if (w + 1 >= outSize) {
            out[0] = '\0';
            return false;
        }
        out[w++] = *p;
    }
    out[w] = '\0';
    if (!seenQ) {
        out[0] = '\0';
        return false;
    }
    return true;
}

// Parses Nominatim's reply: an array of places, each with "lat", "lon" and "display_name". Places missing a usable
// position are skipped. Returns how many were written (at most max), or -1 if the reply isn't such an array.
inline int parseResults(const char *json, size_t len, Result *out, int max)
{
    using namespace Json;
    if (!json || !out || max <= 0)
        return -1;
    Cursor c{json, json + len};
    if (!c.take('['))
        return -1;
    int count = 0;
    if (c.take(']'))
        return 0;
    do {
        if (!c.take('{'))
            return -1;
        Result r{};
        bool haveLat = false, haveLon = false;
        if (!c.take('}')) {
            do {
                char key[16];
                if (!readString(c, key, sizeof(key)) || !c.take(':'))
                    return -1;
                if (strcmp(key, "lat") == 0) {
                    haveLat = readNumber(c, r.lat);
                } else if (strcmp(key, "lon") == 0) {
                    haveLon = readNumber(c, r.lon);
                } else if (strcmp(key, "display_name") == 0) {
                    if (!readString(c, r.name, sizeof(r.name)))
                        return -1;
                } else if (!skipValue(c)) {
                    return -1;
                }
            } while (c.take(','));
            if (!c.take('}'))
                return -1;
        }
        const bool inRange = r.lat >= -90.0 && r.lat <= 90.0 && r.lon >= -180.0 && r.lon <= 180.0;
        if (haveLat && haveLon && inRange && count < max)
            out[count++] = r;
    } while (c.take(','));
    return c.take(']') ? count : -1;
}

// Parses the US Census geocoder's reply: result.addressMatches, each with coordinates {x: lon, y: lat} and a
// matchedAddress. Returns how many were written (at most max), or -1 if the reply isn't that shape.
inline int parseCensus(const char *json, size_t len, Result *out, int max)
{
    using namespace Json;
    if (!json || !out || max <= 0)
        return -1;
    Cursor c{json, json + len};
    int count = 0;
    const bool ok = eachField(c, [&](const char *key) {
        if (strcmp(key, "result") != 0)
            return skipValue(c);
        return eachField(c, [&](const char *resultKey) {
            if (strcmp(resultKey, "addressMatches") != 0)
                return skipValue(c);
            return eachItem(c, [&](int) {
                Result r{};
                bool haveLat = false, haveLon = false;
                const bool parsed = eachField(c, [&](const char *k) {
                    if (strcmp(k, "matchedAddress") == 0)
                        return readString(c, r.name, sizeof(r.name));
                    if (strcmp(k, "coordinates") != 0)
                        return skipValue(c);
                    return eachField(c, [&](const char *axis) {
                        if (strcmp(axis, "x") == 0)
                            return haveLon = readNumber(c, r.lon);
                        if (strcmp(axis, "y") == 0)
                            return haveLat = readNumber(c, r.lat);
                        return skipValue(c);
                    });
                });
                const bool inRange = r.lat >= -90.0 && r.lat <= 90.0 && r.lon >= -180.0 && r.lon <= 180.0;
                if (parsed && haveLat && haveLon && inRange && count < max)
                    out[count++] = r;
                return parsed;
            });
        });
    });
    return ok ? count : -1;
}

// Parses Photon's GeoJSON reply: features, each with geometry.coordinates [lon, lat] and address properties, which
// are joined into the name ("Sutter Auburn Surgery Center, Professional Drive, Auburn, California").
inline int parsePhoton(const char *json, size_t len, Result *out, int max)
{
    using namespace Json;
    if (!json || !out || max <= 0)
        return -1;
    Cursor c{json, json + len};
    int count = 0;
    const bool ok = eachField(c, [&](const char *key) {
        if (strcmp(key, "features") != 0)
            return skipValue(c);
        return eachItem(c, [&](int) {
            Result r{};
            bool havePoint = false;
            char name[sizeof(Result::name)] = "", number[12] = "", street[48] = "", city[32] = "", state[32] = "";
            const bool parsed = eachField(c, [&](const char *k) {
                if (strcmp(k, "geometry") == 0) {
                    return eachField(c, [&](const char *g) {
                        if (strcmp(g, "coordinates") != 0)
                            return skipValue(c);
                        return eachItem(c, [&](int axis) {
                            double v;
                            if (!readNumber(c, v))
                                return false;
                            if (axis == 0)
                                r.lon = v;
                            else if (axis == 1)
                                r.lat = v, havePoint = true;
                            return true;
                        });
                    });
                }
                if (strcmp(k, "properties") != 0)
                    return skipValue(c);
                return eachField(c, [&](const char *prop) {
                    if (strcmp(prop, "name") == 0)
                        return readString(c, name, sizeof(name));
                    if (strcmp(prop, "housenumber") == 0)
                        return readString(c, number, sizeof(number));
                    if (strcmp(prop, "street") == 0)
                        return readString(c, street, sizeof(street));
                    if (strcmp(prop, "city") == 0 || (strcmp(prop, "county") == 0 && !city[0]))
                        return readString(c, city, sizeof(city));
                    if (strcmp(prop, "state") == 0)
                        return readString(c, state, sizeof(state));
                    return skipValue(c);
                });
            });
            if (!parsed)
                return false;
            const bool inRange = r.lat >= -90.0 && r.lat <= 90.0 && r.lon >= -180.0 && r.lon <= 180.0;
            if (!havePoint || !inRange || count >= max)
                return true;
            char streetLine[64] = "";
            if (street[0])
                snprintf(streetLine, sizeof(streetLine), "%s%s%s", number, number[0] ? " " : "", street);
            // Whole parts only, as many as fit; a first part too long by itself is cut on a character boundary.
            const char *parts[] = {name, streetLine, city, state};
            size_t w = 0;
            for (const char *part : parts) {
                if (!part[0])
                    continue;
                const size_t need = strlen(part) + (w ? 2 : 0);
                if (w + need < sizeof(r.name)) {
                    w += (size_t)snprintf(r.name + w, sizeof(r.name) - w, "%s%s", w ? ", " : "", part);
                    continue;
                }
                if (w == 0) {
                    size_t cut = sizeof(r.name) - 1;
                    while (cut > 0 && ((unsigned char)part[cut] & 0xC0) == 0x80)
                        cut--; // back to the start of the character the limit falls in
                    memcpy(r.name, part, cut);
                    r.name[cut] = '\0';
                }
                break;
            }
            out[count++] = r;
            return true;
        });
    });
    return ok ? count : -1;
}

// The reply parser for a provider.
inline int parseFor(Provider provider, const char *json, size_t len, Result *out, int max)
{
    return provider == Provider::Census   ? parseCensus(json, len, out, max)
           : provider == Provider::Photon ? parsePhoton(json, len, out, max)
                                          : parseResults(json, len, out, max);
}

} // namespace NicheGraphics::MapTiles::Geocode
