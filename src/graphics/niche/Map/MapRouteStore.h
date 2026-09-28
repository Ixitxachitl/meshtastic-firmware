#pragma once

// A street route saved for Map > Navigate > Saved Routes: the target, how it is travelled, and the route's points and
// turns, packed small enough to keep several in internal flash. Dependency-free so the format is unit-tested.

#include "./MapRouteParse.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace NicheGraphics::MapTiles::RouteStore
{

constexpr uint32_t kMagic = 0x3154524D; // "MRT1"

struct Header {
    uint32_t sequence; // larger is newer
    int32_t targetLatE7, targetLonE7;
    uint32_t nodeNum, waypointId; // what the target follows, as in NavTarget
    uint8_t travelMode;
    char name[32];
    float lengthKm, timeSec;
    uint32_t pointCount;
    uint16_t maneuverCount;
};

namespace detail
{

struct Writer {
    uint8_t *out;
    size_t cap, len;
    bool ok;
    void bytes(const void *p, size_t n)
    {
        if (!ok || len + n > cap) {
            ok = false;
            return;
        }
        memcpy(out + len, p, n);
        len += n;
    }
    template <typename T> void value(T v) { bytes(&v, sizeof(v)); }
    void varint(uint32_t v)
    {
        do {
            const uint8_t b = (uint8_t)((v & 0x7F) | (v > 0x7F ? 0x80 : 0));
            bytes(&b, 1);
            v >>= 7;
        } while (v);
    }
    void svarint(int32_t v) { varint(((uint32_t)v << 1) ^ (v < 0 ? 0xFFFFFFFFu : 0u)); }
};

struct Reader {
    const uint8_t *in;
    size_t len, pos;
    bool ok;
    void bytes(void *p, size_t n)
    {
        if (!ok || pos + n > len) {
            ok = false;
            return;
        }
        memcpy(p, in + pos, n);
        pos += n;
    }
    template <typename T> T value()
    {
        T v{};
        bytes(&v, sizeof(v));
        return v;
    }
    uint32_t varint()
    {
        uint32_t v = 0;
        for (int shift = 0; shift < 35; shift += 7) {
            uint8_t b = 0;
            bytes(&b, 1);
            if (!ok)
                return 0;
            v |= (uint32_t)(b & 0x7F) << shift;
            if (!(b & 0x80))
                return v;
        }
        ok = false;
        return 0;
    }
    int32_t svarint()
    {
        const uint32_t v = varint();
        return (int32_t)((v >> 1) ^ (0u - (v & 1)));
    }
};

// FNV-1a: catches a file cut short or damaged, which is all a checksum here is for.
inline uint32_t checksum(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

inline void writeHeader(Writer &w, const Header &h)
{
    w.value(kMagic);
    w.value(h.sequence);
    w.value(h.targetLatE7);
    w.value(h.targetLonE7);
    w.value(h.nodeNum);
    w.value(h.waypointId);
    w.value(h.travelMode);
    w.bytes(h.name, sizeof(h.name));
    w.value(h.lengthKm);
    w.value(h.timeSec);
    w.value(h.pointCount);
    w.value(h.maneuverCount);
}

inline bool readHeader(Reader &r, Header &h)
{
    if (r.value<uint32_t>() != kMagic)
        return false;
    h.sequence = r.value<uint32_t>();
    h.targetLatE7 = r.value<int32_t>();
    h.targetLonE7 = r.value<int32_t>();
    h.nodeNum = r.value<uint32_t>();
    h.waypointId = r.value<uint32_t>();
    h.travelMode = r.value<uint8_t>();
    r.bytes(h.name, sizeof(h.name));
    h.name[sizeof(h.name) - 1] = '\0';
    h.lengthKm = r.value<float>();
    h.timeSec = r.value<float>();
    h.pointCount = r.value<uint32_t>();
    h.maneuverCount = r.value<uint16_t>();
    return r.ok;
}

} // namespace detail

// Bytes the header takes: enough to read for a listing.
constexpr size_t kHeaderBytes = 4 + 4 + 4 + 4 + 4 + 4 + 1 + 32 + 4 + 4 + 4 + 2;

// The most a route of this size can take, so the caller can size its buffer.
inline size_t maxEncodedSize(uint32_t points, uint16_t maneuvers)
{
    return kHeaderBytes + (size_t)points * 10 + (size_t)maneuvers * (2 + 5 + 4 + 1 + sizeof(Route::Maneuver::instruction)) + 4;
}

// Packs a route into out. Returns the length, or 0 if it doesn't fit. pointCount and maneuverCount come from h.
inline size_t encode(const Header &h, const int32_t *latE7, const int32_t *lonE7, const Route::Maneuver *maneuvers, uint8_t *out,
                     size_t cap)
{
    detail::Writer w{out, cap, 0, true};
    detail::writeHeader(w, h);
    int32_t lat = 0, lon = 0;
    for (uint32_t i = 0; i < h.pointCount; i++) { // deltas: neighbouring points are close, so most take 1-3 bytes
        // Wrapping arithmetic: a jump across the antimeridian overflows int32, and wraps back exactly on decode.
        w.svarint((int32_t)((uint32_t)latE7[i] - (uint32_t)lat));
        w.svarint((int32_t)((uint32_t)lonE7[i] - (uint32_t)lon));
        lat = latE7[i];
        lon = lonE7[i];
    }
    for (uint16_t i = 0; i < h.maneuverCount; i++) {
        const Route::Maneuver &m = maneuvers[i];
        w.value(m.type);
        w.varint(m.beginIndex);
        w.value(m.lengthKm);
        const uint8_t n = (uint8_t)strnlen(m.instruction, sizeof(m.instruction) - 1);
        w.value(n);
        w.bytes(m.instruction, n);
    }
    if (!w.ok)
        return 0;
    const uint32_t sum = detail::checksum(out, w.len);
    w.value(sum);
    return w.ok ? w.len : 0;
}

// Reads just the header, for a listing: needs only the first kHeaderBytes.
inline bool decodeHeader(const uint8_t *in, size_t len, Header &h)
{
    detail::Reader r{in, len, 0, true};
    return detail::readHeader(r, h);
}

// Unpacks a whole route. False if it is damaged, or larger than the caller's arrays.
inline bool decode(const uint8_t *in, size_t len, Header &h, int32_t *latE7, int32_t *lonE7, uint32_t pointCap,
                   Route::Maneuver *maneuvers, uint16_t maneuverCap)
{
    uint32_t stored;
    if (len < 4)
        return false;
    memcpy(&stored, in + len - 4, sizeof(stored)); // may not be aligned
    if (detail::checksum(in, len - 4) != stored)
        return false;
    detail::Reader r{in, len - 4, 0, true};
    if (!detail::readHeader(r, h) || h.pointCount > pointCap || h.maneuverCount > maneuverCap || h.pointCount < 2)
        return false;
    int32_t lat = 0, lon = 0;
    for (uint32_t i = 0; i < h.pointCount; i++) {
        lat = (int32_t)((uint32_t)lat + (uint32_t)r.svarint());
        lon = (int32_t)((uint32_t)lon + (uint32_t)r.svarint());
        latE7[i] = lat;
        lonE7[i] = lon;
    }
    for (uint16_t i = 0; i < h.maneuverCount; i++) {
        Route::Maneuver &m = maneuvers[i];
        m.type = r.value<uint16_t>();
        m.beginIndex = r.varint();
        m.lengthKm = r.value<float>();
        const uint8_t n = r.value<uint8_t>();
        if (n >= sizeof(m.instruction))
            return false;
        r.bytes(m.instruction, n);
        m.instruction[n] = '\0';
        if (m.beginIndex >= h.pointCount)
            return false;
    }
    return r.ok && r.pos == r.len;
}

} // namespace NicheGraphics::MapTiles::RouteStore
