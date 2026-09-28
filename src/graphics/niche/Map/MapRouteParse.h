#pragma once

// Street routing for Map > Navigate: the request to a Valhalla server and the parse of its reply into points and
// turn instructions. Dependency-free so it is unit-tested.

#include "./MapGeocodeParse.h"
#include "./MapJsonScan.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace NicheGraphics::MapTiles::Route
{

// FOSSGIS's public Valhalla server: no key, fair use. A card file /maps/.route may name another Valhalla server; the
// line must contain {json}, where the request goes.
constexpr const char *kDefaultRouteUrl = "https://valhalla1.openstreetmap.de/route?json={json}";

enum class Mode : uint8_t { Car = 0, Bicycle = 1, Walk = 2 };

inline const char *costing(Mode mode)
{
    return mode == Mode::Bicycle ? "bicycle" : mode == Mode::Walk ? "pedestrian" : "auto";
}

struct Maneuver {
    uint16_t type;       // Valhalla's maneuver type: 10 right, 15 left, 4 destination, and so on
    uint32_t beginIndex; // the point it starts at
    float lengthKm;      // how far it runs
    char instruction[72];
};

// Where a parse writes. The caller owns the arrays; the route is thinned evenly to fit pointCap.
struct Result {
    int32_t *latE7, *lonE7;
    uint32_t pointCap, pointCount;
    Maneuver *maneuvers;
    uint16_t maneuverCap, maneuverCount;
    float lengthKm, timeSec;
};

// The request URL: from and to, and how the route is travelled. False, and out left empty, if the template has no
// {json}, isn't http(s), or the result would not fit.
inline bool buildRequestUrl(const char *tmpl, double fromLat, double fromLon, double toLat, double toLon, Mode mode, char *out,
                            size_t outSize)
{
    if (!tmpl || !out || outSize == 0)
        return false;
    out[0] = '\0';
    if (strncmp(tmpl, "http://", 7) != 0 && strncmp(tmpl, "https://", 8) != 0)
        return false;
    const char *slot = strstr(tmpl, "{json}");
    if (!slot)
        return false;

    char json[256];
    const int n = snprintf(json, sizeof(json),
                           "{\"locations\":[{\"lat\":%.6f,\"lon\":%.6f},{\"lat\":%.6f,\"lon\":%.6f}],\"costing\":\"%s\","
                           "\"directions_options\":{\"units\":\"kilometers\"}}",
                           fromLat, fromLon, toLat, toLon, costing(mode));
    if (n <= 0 || (size_t)n >= sizeof(json))
        return false;
    char encoded[768];
    if (!Geocode::encodeQuery(json, encoded, sizeof(encoded)))
        return false;

    const size_t head = (size_t)(slot - tmpl), encodedLen = strlen(encoded), tail = strlen(slot + 6);
    if (head + encodedLen + tail >= outSize)
        return false;
    memcpy(out, tmpl, head);
    memcpy(out + head, encoded, encodedLen);
    memcpy(out + head + encodedLen, slot + 6, tail + 1);
    return true;
}

namespace detail
{

// Walks an encoded polyline held raw in the reply, so JSON's escaped backslash counts as one character. Calls
// emit(index, latE7, lonE7) per point and returns how many there were, or -1 if it is malformed.
template <typename Emit> long walkPolyline(const char *raw, size_t rawLen, int precision, Emit emit)
{
    const int64_t toE7 = precision == 6 ? 10 : precision == 5 ? 100 : 0;
    if (!toE7)
        return -1;
    int64_t lat = 0, lon = 0;
    long index = 0;
    size_t i = 0;
    auto next = [&](int &value) -> int {
        if (i >= rawLen)
            return -1;
        char ch = raw[i++];
        if (ch == '\\') {
            if (i >= rawLen || raw[i] != '\\')
                return -1; // the only escape a polyline can need
            ch = raw[i++];
        }
        if (ch < 63 || ch > 126)
            return -1;
        value = ch - 63;
        return 0;
    };
    auto readDelta = [&](int64_t &delta) -> bool {
        int64_t result = 0;
        int shift = 0, chunk = 0;
        do {
            if (next(chunk) < 0 || shift > 60)
                return false;
            result |= (int64_t)(chunk & 0x1F) << shift;
            shift += 5;
        } while (chunk >= 0x20);
        delta = (result & 1) ? ~(result >> 1) : (result >> 1);
        return true;
    };
    while (i < rawLen) {
        int64_t dLat, dLon;
        if (!readDelta(dLat) || !readDelta(dLon))
            return -1;
        lat += dLat;
        lon += dLon;
        const int64_t latE7 = lat * toE7, lonE7 = lon * toE7;
        if (latE7 < -900000000LL || latE7 > 900000000LL || lonE7 < -1800000000LL || lonE7 > 1800000000LL)
            return -1;
        emit(index++, (int32_t)latE7, (int32_t)lonE7);
    }
    return index;
}

} // namespace detail

// Parses a Valhalla /route reply into out. Only the first leg is read (a two-location request has one). False if the
// reply is malformed or has no usable shape.
inline bool parseValhalla(const char *json, size_t len, Result &out)
{
    using namespace detail;
    using Json::eachField;
    using Json::eachItem;
    using Json::stringSpan;
    if (!json || !out.latE7 || !out.lonE7 || out.pointCap < 3)
        return false;
    out.pointCount = 0;
    out.maneuverCount = 0;
    out.lengthKm = out.timeSec = 0;

    Json::Cursor c{json, json + len};
    const char *shape = nullptr;
    size_t shapeLen = 0;
    bool ok = eachField(c, [&](const char *key) {
        if (strcmp(key, "trip") != 0)
            return Json::skipValue(c);
        return eachField(c, [&](const char *tripKey) {
            if (strcmp(tripKey, "summary") == 0) {
                return eachField(c, [&](const char *k) {
                    double v;
                    if (strcmp(k, "length") == 0 && Json::readNumber(c, v)) {
                        out.lengthKm = (float)v;
                        return true;
                    }
                    if (strcmp(k, "time") == 0 && Json::readNumber(c, v)) {
                        out.timeSec = (float)v;
                        return true;
                    }
                    return Json::skipValue(c);
                });
            }
            if (strcmp(tripKey, "legs") != 0)
                return Json::skipValue(c);
            return eachItem(c, [&](int legIndex) {
                if (legIndex > 0)
                    return Json::skipValue(c);
                return eachField(c, [&](const char *legKey) {
                    if (strcmp(legKey, "shape") == 0)
                        return stringSpan(c, shape, shapeLen);
                    if (strcmp(legKey, "maneuvers") != 0)
                        return Json::skipValue(c);
                    return eachItem(c, [&](int) {
                        Maneuver m{};
                        bool haveBegin = false;
                        const bool parsed = eachField(c, [&](const char *k) {
                            double v;
                            if (strcmp(k, "type") == 0 && Json::readNumber(c, v)) {
                                m.type = (uint16_t)v;
                                return true;
                            }
                            if (strcmp(k, "begin_shape_index") == 0 && Json::readNumber(c, v)) {
                                m.beginIndex = (uint32_t)v;
                                haveBegin = true;
                                return true;
                            }
                            if (strcmp(k, "length") == 0 && Json::readNumber(c, v)) {
                                m.lengthKm = (float)v;
                                return true;
                            }
                            if (strcmp(k, "instruction") == 0)
                                return Json::readString(c, m.instruction, sizeof(m.instruction));
                            return Json::skipValue(c);
                        });
                        if (parsed && haveBegin && out.maneuvers && out.maneuverCount < out.maneuverCap)
                            out.maneuvers[out.maneuverCount++] = m;
                        return parsed;
                    });
                });
            });
        });
    });
    if (!ok || !shape)
        return false;

    // Count first, then keep every stride-th point (and always the last) so a long route still fits: the multiples
    // of stride below total-1 number at most pointCap-1, leaving room for the last.
    const long total = walkPolyline(shape, shapeLen, 6, [](long, int32_t, int32_t) {});
    if (total < 2)
        return false;
    const long cap = (long)out.pointCap;
    const long stride = total <= cap ? 1 : (total - 1 + cap - 3) / (cap - 2);
    walkPolyline(shape, shapeLen, 6, [&](long index, int32_t latE7, int32_t lonE7) {
        if ((index % stride == 0 || index == total - 1) && out.pointCount < out.pointCap) {
            out.latE7[out.pointCount] = latE7;
            out.lonE7[out.pointCount] = lonE7;
            out.pointCount++;
        }
    });
    for (uint16_t i = 0; i < out.maneuverCount; i++) {
        uint32_t kept = out.maneuvers[i].beginIndex / (uint32_t)stride;
        if (out.maneuvers[i].beginIndex >= (uint32_t)(total - 1))
            kept = out.pointCount - 1;
        out.maneuvers[i].beginIndex = kept < out.pointCount ? kept : out.pointCount - 1;
    }
    return true;
}

} // namespace NicheGraphics::MapTiles::Route
