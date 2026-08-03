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

Holds NO per-tile index in RAM: within one zoom, tiles are always emitted for exactly the
rectangle [xMin, xMin+width) x [yMin, yMin+height), densely, in ascending (ty, tx) order (zooms
themselves ascending too) - so a tile's position is computable from just the small zoom-range
table (at most kTileBlobMaxZoomRanges entries) instead of a per-tile RAM index. This used to
require each zoom's rectangle to be the *entire* 2^zoom grid (a "dense zoom range starting at
z0"), which made a region baked for just part of the world at a given zoom either impossible or
(if padded with free tiles to keep the old dense-count check happy) require a full 2^zoom-square
index regardless - astronomically large past ~z10. The explicit per-zoom rectangle removes that
restriction while keeping lookups just as cheap (linear-scan a table of at most ~25 entries, then
O(1) arithmetic within the matched rectangle).

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
// zoom byte can't be used as a shift count >= 32 (undefined behaviour) or blow past a plausible
// table size.
constexpr int kTileBlobMaxPlausibleZoom = 24;
constexpr int kTileBlobMaxZoomRanges = kTileBlobMaxPlausibleZoom + 1; // at most one entry per zoom, z0..z24

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

// Validates a decoded zoom-range table against the file's total tile_count: zooms must be
// strictly ascending (no duplicates), each rectangle must fit inside its zoom's actual 2^zoom
// grid, and the rectangles' areas must sum to exactly totalTileCount. Returns false for a
// corrupt/hostile/truncated table rather than risking a silently-wrong tile lookup.
inline bool validateTileBlobZoomRanges(const TileBlobZoomRange *ranges, int count, uint32_t totalTileCount)
{
    if (count <= 0 || count > kTileBlobMaxZoomRanges)
        return false;
    uint64_t sum = 0;
    int prevZoom = -1;
    for (int i = 0; i < count; i++) {
        const TileBlobZoomRange &r = ranges[i];
        if (r.zoom <= prevZoom || r.zoom > kTileBlobMaxPlausibleZoom)
            return false;
        if (r.width == 0 || r.height == 0)
            return false;
        const uint32_t side = 1u << r.zoom;
        if (r.xMin >= side || r.yMin >= side || (uint32_t)r.xMin + r.width > side || (uint32_t)r.yMin + r.height > side)
            return false;
        sum += (uint64_t)r.width * r.height;
        prevZoom = r.zoom;
    }
    return sum == totalTileCount;
}

// Base tile-index (offset into the tile-entry table) for the range at `rangeIndex` - the sum of
// every earlier range's tile count.
inline uint32_t tileBlobBaseIndexForRangeIndex(const TileBlobZoomRange *ranges, int rangeIndex)
{
    uint32_t base = 0;
    for (int i = 0; i < rangeIndex; i++)
        base += (uint32_t)ranges[i].width * ranges[i].height;
    return base;
}

inline int tileBlobFindRangeIndex(const TileBlobZoomRange *ranges, int count, int zoom)
{
    for (int i = 0; i < count; i++) {
        if (ranges[i].zoom == zoom)
            return i;
    }
    return -1;
}

// Returns the tile index for (zoom, tx, ty), or -1 if not present in this blob's ranges.
inline int tileBlobIndexOf(const TileBlobZoomRange *ranges, int count, uint32_t totalCount, int zoom, int tx, int ty)
{
    const int ri = tileBlobFindRangeIndex(ranges, count, zoom);
    if (ri < 0)
        return -1;
    const TileBlobZoomRange &r = ranges[ri];
    if (tx < r.xMin || tx >= (int)r.xMin + (int)r.width || ty < r.yMin || ty >= (int)r.yMin + (int)r.height)
        return -1;
    const uint32_t base = tileBlobBaseIndexForRangeIndex(ranges, ri);
    const uint32_t local = (uint32_t)(ty - r.yMin) * r.width + (uint32_t)(tx - r.xMin);
    const uint32_t idx = base + local;
    return idx < totalCount ? (int)idx : -1;
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
