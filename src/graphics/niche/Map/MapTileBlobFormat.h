#pragma once

/*

Shared parsing helpers for the tile-index blob format written by bin/generate_map_tiles.py, used
by both MapTileSourceFile (FSCom filesystem) and MapTileSourceSD (real SD card via SdFat) so the
two readers - which differ only in which file API they read through - can't drift out of sync with
each other on the actual on-disk format or the index math built on top of it.

Format:
    u32 magic 'MTLB', u32 tile_count
    tile_count * { u8 zoom, u16 tx, u16 ty, u8 kind, u32 offset, u16 size }
    followed by concatenated payload bytes (offset is relative to the start of this region).

Holds NO per-tile index in RAM: bin/generate_map_tiles.py always emits every (zoom, tx, ty) for a
requested zoom range, densely, in ascending (zoom, ty, tx) order, so the whole index's shape is
fully determined by just the first and last entries, and any tile's position in it is computable
algebraically (see tileBlobIndexOf() etc. below) instead of needing a per-tile RAM index.

*/

#include <stddef.h>
#include <stdint.h>

namespace NicheGraphics::MapTiles
{

constexpr uint32_t kTileBlobMagic = 0x424C544D; // 'MTLB' little-endian
constexpr size_t kTileBlobHeaderSize = 8;       // u32 magic, u32 tile_count
constexpr size_t kTileBlobEntrySize = 12;       // u8 zoom, u16 tx, u16 ty, u8 kind, u32 offset, u16 size

struct TileBlobEntry {
    uint8_t zoom;
    uint16_t tx;
    uint16_t ty;
    uint8_t kind;
    uint32_t offset;
    uint16_t size;
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

// A real basemap bake never goes anywhere near this deep; this exists purely so the shift below
// can't be handed a corrupt/hostile tile_count and shift by >= 64 bits (undefined behaviour).
constexpr int kTileBlobMaxPlausibleZoom = 24;

// Solves for the highest zoom present (zHiOut) given `zLo` (the first index entry's zoom) and the
// total `count` of entries, from the dense-range tile-count identity:
//   count == sum_{z=zLo}^{zHi} 4^z == (4^(zHi+1) - 4^zLo) / 3
// Returns false if `count` doesn't correspond to any dense range starting at zLo (a corrupt or
// sparse blob), or if the implied zoom would be implausibly deep (corrupt/hostile blob).
inline bool solveTileBlobZoomRange(uint32_t count, int zLo, int &zHiOut)
{
    if (zLo < 0 || zLo > kTileBlobMaxPlausibleZoom)
        return false;
    int zHi = zLo;
    uint64_t cumulative = 1ULL << (2 * zLo);
    while (cumulative < count) {
        if (zHi >= kTileBlobMaxPlausibleZoom)
            return false;
        zHi++;
        cumulative += 1ULL << (2 * zHi);
    }
    if (cumulative != count)
        return false;
    zHiOut = zHi;
    return true;
}

// Cumulative tile count for all zooms below `zoom` (i.e. this zoom's base index) within a dense,
// contiguous [zLo, zHi] zoom range: sum_{z=zLo}^{zoom-1} 4^z.
inline uint32_t tileBlobBaseIndexForZoom(int zoom, int zLo)
{
    return (uint32_t)(((1ULL << (2 * zoom)) - (1ULL << (2 * zLo))) / 3);
}

// Returns the tile index for (zoom, tx, ty) within a dense [zLo, zHi] zoom range of `count` total
// tiles, or -1 if not present.
inline int tileBlobIndexOf(int zoom, int tx, int ty, int zLo, int zHi, uint32_t count)
{
    if (zoom < zLo || zoom > zHi)
        return -1;
    const int side = 1 << zoom;
    if (tx < 0 || tx >= side || ty < 0 || ty >= side)
        return -1;
    const uint32_t i = tileBlobBaseIndexForZoom(zoom, zLo) + (uint32_t)ty * side + tx;
    return i < count ? (int)i : -1;
}

inline int tileBlobZoomAt(int tileIndex, int zLo, int zHi)
{
    for (int z = zLo; z <= zHi; z++) {
        if ((uint32_t)tileIndex < tileBlobBaseIndexForZoom(z + 1, zLo))
            return z;
    }
    return zHi;
}

inline int tileBlobTxAt(int tileIndex, int zLo, int zHi)
{
    const int zoom = tileBlobZoomAt(tileIndex, zLo, zHi);
    const uint32_t local = (uint32_t)tileIndex - tileBlobBaseIndexForZoom(zoom, zLo);
    return (int)(local % (1u << zoom));
}

inline int tileBlobTyAt(int tileIndex, int zLo, int zHi)
{
    const int zoom = tileBlobZoomAt(tileIndex, zLo, zHi);
    const uint32_t local = (uint32_t)tileIndex - tileBlobBaseIndexForZoom(zoom, zLo);
    return (int)(local / (1u << zoom));
}

} // namespace NicheGraphics::MapTiles
