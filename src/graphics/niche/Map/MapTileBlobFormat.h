#pragma once

/*

Shared parsing helpers for the tile-index blob format written by the browser-based baker at
https://github.com/Ixitxachitl/binary-map-downloader, used by both
MapTileSourceFile (FSCom filesystem) and MapTileSourceSD (real SD card via SdFat) so the two
readers - which differ only in which file API they read through - can't drift out of sync with
each other on the actual on-disk format or the index math built on top of it.

Format:
    u32 magic 'MTL2', u32 tile_count
    u8  zoom_range_count (N)
    N * { u8 zoom, u16 xMin, u16 yMin, u16 width, u16 height }
    tile_count * { u8 zoom, u16 tx, u16 ty, u8 kind, u32 offset, u16 size }
    followed by concatenated payload bytes (offset is relative to the start of this region).

Holds NO per-tile index in RAM: each *range* covers exactly the rectangle
[xMin, xMin+width) x [yMin, yMin+height), densely, in ascending (ty, tx) order, and the ranges
themselves sit back-to-back in the tile-entry table in the order the range table lists them - so a
tile's position is computable from just the small zoom-range table instead of a per-tile RAM index.
This used to require each zoom's rectangle to be the *entire* 2^zoom grid (a "dense zoom range
starting at z0"), which made a region baked for just part of the world at a given zoom either
impossible or (if padded with free tiles to keep the old dense-count check happy) require a full
2^zoom-square index regardless - astronomically large past ~z10. The explicit per-range rectangle
removes that restriction while keeping lookups just as cheap (linear-scan the range table, then
O(1) arithmetic within the matched rectangle).

A zoom may be covered by more than one range, as long as ranges sharing a zoom don't overlap each
other, which is what lets one blob hold several disjoint regions at the same zoom (two separate
cities, say, without also carrying the ocean between them). Ranges are ordered by non-decreasing
zoom - not strictly ascending, so same-zoom ranges are adjacent. Firmware predating that rule
required strictly-ascending zooms, so it cleanly refuses a blob that actually uses two ranges for
one zoom rather than misreading it; the on-disk layout itself is unchanged, so the magic stays
'MTL2'.

*/

#include <stddef.h>
#include <stdint.h>

namespace NicheGraphics::MapTiles
{

constexpr uint32_t kTileBlobMagic = 0x324C544D;   // 'MTL2' little-endian
constexpr size_t kTileBlobHeaderSize = 8;         // u32 magic, u32 tile_count
constexpr size_t kTileBlobZoomRangeEntrySize = 9; // u8 zoom, u16 xMin, u16 yMin, u16 width, u16 height
constexpr size_t kTileBlobEntrySize = 12;         // u8 zoom, u16 tx, u16 ty, u8 kind, u32 offset, u16 size

// A real basemap bake never goes anywhere near this deep; this exists purely so a corrupt/hostile
// zoom byte can't be used as a shift count >= 32 (undefined behaviour).
constexpr int kTileBlobMaxPlausibleZoom = 24;
// Whatever the u8 count field can express. Since a zoom may now be split across several disjoint
// ranges, this can't be bounded by the zoom depth any more (it used to be "at most one entry per
// zoom, z0..z24") - so the readers size their range table to the file's actual count rather than
// carrying a fixed 255-entry array, and this is only the "can't be worse than the wire format
// allows" ceiling. The baker enforces the same limit for the same reason.
constexpr int kTileBlobMaxZoomRanges = 255;
// Distinct zoom levels can still never exceed one per plausible zoom - the bound that matters for
// the small z0..z24 lookup table the readers expose via zoomCount()/zoomAt().
constexpr int kTileBlobMaxDistinctZooms = kTileBlobMaxPlausibleZoom + 1;

struct TileBlobEntry {
    uint8_t zoom;
    uint16_t tx;
    uint16_t ty;
    uint8_t kind;
    uint32_t offset;
    uint16_t size;
};

struct TileBlobZoomRange {
    uint8_t zoom;
    uint16_t xMin, yMin, width, height;
};

inline uint16_t readTileBlobU16LE(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

inline uint32_t readTileBlobU32LE(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Decodes one kTileBlobEntrySize-byte record read from the file into `out`.
inline void decodeTileBlobEntry(const uint8_t *buf, TileBlobEntry &out)
{
    out.zoom = buf[0];
    out.tx = readTileBlobU16LE(buf + 1);
    out.ty = readTileBlobU16LE(buf + 3);
    out.kind = buf[5];
    out.offset = readTileBlobU32LE(buf + 6);
    out.size = readTileBlobU16LE(buf + 10);
}

// Decodes one kTileBlobZoomRangeEntrySize-byte record read from the file into `out`.
inline void decodeTileBlobZoomRange(const uint8_t *buf, TileBlobZoomRange &out)
{
    out.zoom = buf[0];
    out.xMin = readTileBlobU16LE(buf + 1);
    out.yMin = readTileBlobU16LE(buf + 3);
    out.width = readTileBlobU16LE(buf + 5);
    out.height = readTileBlobU16LE(buf + 7);
}

// True if two ranges' rectangles intersect. Only meaningful for ranges at the same zoom - tile
// coordinates at different zooms aren't comparable.
inline bool tileBlobRangesOverlap(const TileBlobZoomRange &a, const TileBlobZoomRange &b)
{
    return a.xMin < b.xMin + b.width && a.xMin + a.width > b.xMin && a.yMin < b.yMin + b.height && a.yMin + a.height > b.yMin;
}

// Validates a decoded zoom-range table against the file's total tile_count: zooms must be
// non-decreasing (a zoom may repeat, but only as ranges that don't overlap each other - two
// rectangles covering the same tile would make its index ambiguous), each rectangle must fit
// inside its zoom's actual 2^zoom grid, and the rectangles' areas must sum to exactly
// totalTileCount. Returns false for a corrupt/hostile/truncated table rather than risking a
// silently-wrong tile lookup.
inline bool validateTileBlobZoomRanges(const TileBlobZoomRange *ranges, int count, uint32_t totalTileCount)
{
    if (count <= 0 || count > kTileBlobMaxZoomRanges)
        return false;
    uint64_t sum = 0;
    int prevZoom = -1;
    for (int i = 0; i < count; i++) {
        const TileBlobZoomRange &r = ranges[i];
        if (r.zoom < prevZoom || r.zoom > kTileBlobMaxPlausibleZoom)
            return false;
        if (r.width == 0 || r.height == 0)
            return false;
        const uint32_t side = 1u << r.zoom;
        if (r.xMin >= side || r.yMin >= side || (uint32_t)r.xMin + r.width > side || (uint32_t)r.yMin + r.height > side)
            return false;
        // Only the ranges after this one need checking (overlap is symmetric), and only until the
        // zoom changes - the non-decreasing order checked above keeps a zoom's ranges adjacent.
        for (int j = i + 1; j < count && ranges[j].zoom == r.zoom; j++) {
            if (tileBlobRangesOverlap(r, ranges[j]))
                return false;
        }
        sum += (uint64_t)r.width * r.height;
        prevZoom = r.zoom;
    }
    return sum == totalTileCount;
}

// Collapses a validated range table into the distinct zoom levels it covers, ascending, writing
// them to `out` (which must hold kTileBlobMaxDistinctZooms entries) and returning how many there
// are. Several ranges can share a zoom, so this is what zoomCount()/zoomAt() report rather than
// the range table itself - their contract is distinct zooms, and a duplicate would make callers
// that enumerate available zoom levels (see MapApplet's zoom picker) see the same one twice.
// Relies on the non-decreasing zoom order validateTileBlobZoomRanges enforces.
inline int tileBlobDistinctZooms(const TileBlobZoomRange *ranges, int count, uint8_t *out)
{
    int n = 0;
    for (int i = 0; i < count; i++) {
        if (n == 0 || out[n - 1] != ranges[i].zoom)
            out[n++] = ranges[i].zoom;
    }
    return n;
}

// Returns the tile index for (zoom, tx, ty), or -1 if not present in this blob's ranges. Scans for
// the range that actually *contains* the tile rather than the first one at its zoom: a zoom can be
// covered by several disjoint rectangles, so matching on zoom alone would miss tiles that live in
// a later range (and, worse, return a wrong index for one outside the first range's rectangle if
// the bounds check below were relaxed). Disjointness makes the containing range unique, so the
// first match is the only match. `base` accumulates as we go, which is the same running sum
// tileBlobZoomAt/TxAt/TyAt walk in the other direction.
inline int tileBlobIndexOf(const TileBlobZoomRange *ranges, int count, uint32_t totalCount, int zoom, int tx, int ty)
{
    uint32_t base = 0;
    for (int i = 0; i < count; i++) {
        const TileBlobZoomRange &r = ranges[i];
        if (r.zoom == zoom && tx >= r.xMin && tx < (int)r.xMin + (int)r.width && ty >= r.yMin &&
            ty < (int)r.yMin + (int)r.height) {
            const uint32_t local = (uint32_t)(ty - r.yMin) * r.width + (uint32_t)(tx - r.xMin);
            const uint32_t idx = base + local;
            return idx < totalCount ? (int)idx : -1;
        }
        base += (uint32_t)r.width * r.height;
    }
    return -1;
}

inline int tileBlobZoomAt(const TileBlobZoomRange *ranges, int count, int tileIndex)
{
    uint32_t base = 0;
    for (int i = 0; i < count; i++) {
        const uint32_t size = (uint32_t)ranges[i].width * ranges[i].height;
        if ((uint32_t)tileIndex < base + size)
            return ranges[i].zoom;
        base += size;
    }
    return count > 0 ? ranges[count - 1].zoom : 0;
}

inline int tileBlobTxAt(const TileBlobZoomRange *ranges, int count, int tileIndex)
{
    uint32_t base = 0;
    for (int i = 0; i < count; i++) {
        const uint32_t size = (uint32_t)ranges[i].width * ranges[i].height;
        if ((uint32_t)tileIndex < base + size) {
            const uint32_t local = (uint32_t)tileIndex - base;
            return ranges[i].xMin + (int)(local % ranges[i].width);
        }
        base += size;
    }
    return 0;
}

inline int tileBlobTyAt(const TileBlobZoomRange *ranges, int count, int tileIndex)
{
    uint32_t base = 0;
    for (int i = 0; i < count; i++) {
        const uint32_t size = (uint32_t)ranges[i].width * ranges[i].height;
        if ((uint32_t)tileIndex < base + size) {
            const uint32_t local = (uint32_t)tileIndex - base;
            return ranges[i].yMin + (int)(local / ranges[i].width);
        }
        base += size;
    }
    return 0;
}

} // namespace NicheGraphics::MapTiles
