#include "./MapTileRenderer.h"
#include "./MapTile.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef DEG_TO_RAD
#define DEG_TO_RAD 0.017453292519943295769236907684886f
#endif

using namespace NicheGraphics::MapTiles;

namespace
{

constexpr uint8_t MAP_TILE_LAYOUT_SPARSE = 0;
constexpr uint8_t MAP_TILE_LAYOUT_GRID = 1;

// Tiles are 1 bit/pixel, column-major: [bx=0..kTileSizePx/8-1][y=0..kTileSizePx-1], 8 px/byte.
uint8_t s_tileCacheBuffer[NicheGraphics::MapTiles::kTileBufferBytes];

// Which (source, tileIndex) is currently sitting in s_tileCacheBuffer, so blitTile can skip a
// redundant decode (SD read + LZ4 decompress) when the exact same tile is needed again. A single
// slot is enough for the common case that matters most: most Map-capable screens are far smaller
// than a single tile, so a whole redraw - and, critically, most individual pan/zoom steps, which
// usually don't even leave the current tile - needs only one tile. Without this, every such
// redraw/step was re-reading and re-decompressing the exact same bytes from SD for no reason,
// which is what actually made panning/zooming feel sluggish even after fixing the per-call file
// reopen and bumping the SD SPI clock. nullptr is a valid source key (the compiled-in MapTile.h
// path), distinct from "nothing cached" (kNoCachedTile).
constexpr int kNoCachedTile = -1;
NicheGraphics::MapTiles::TileSource *s_cachedTileSource = nullptr;
int s_cachedTileIndex = kNoCachedTile;

// Shared compressed-payload scratch buffer - see tileCompressedScratchBuffer() in the header.
// Only where a TileSource can exist; InkHUD's compiled-in path decodes straight from flash.
#if BASEUI_HAS_MAP
uint8_t s_tileCompressedScratchBuffer[NicheGraphics::MapTiles::kTileBufferBytes];
#endif

// A baked tile always covers the same geographic span regardless of storage resolution: the
// standard Web Mercator convention is 256 world-units per tile at any zoom (that's what defines
// the zoom levels themselves) - a wholly different, fixed concept from kTileSizePx (512, MapTiler's
// native/retina resolution - see MapTileRenderer.h), which packs 2x the stored pixels into that
// same 256-unit span. So world coordinates within a tile stay in the standard 0-255 range
// throughout this file; only the very last step - turning a world-relative coordinate into an
// actual stored-pixel index for the buffer - needs this scale factor.
constexpr float kWorldUnitsPerTile = 256.0f;
constexpr float kStoredPxPerWorldUnit = NicheGraphics::MapTiles::kTileSizePx / kWorldUnitsPerTile;

bool usesGridTileLayout()
{
    return map_tile_layout == MAP_TILE_LAYOUT_GRID && map_tile_grid_cols > 0 && map_tile_grid_rows > 0 &&
           map_tile_block_count > 0;
}

int gridTilesPerBlock()
{
    return (int)map_tile_grid_cols * (int)map_tile_grid_rows;
}

int tileZoomAt(int tileIndex)
{
    if (!usesGridTileLayout())
        return map_tile_zooms[tileIndex];
    int tilesPerBlock = gridTilesPerBlock();
    int blockIndex = tilesPerBlock > 0 ? (tileIndex / tilesPerBlock) : 0;
    return map_tile_block_zooms[blockIndex];
}

int tileTxAt(int tileIndex)
{
    if (!usesGridTileLayout())
        return map_tile_tx[tileIndex];
    int rows = map_tile_grid_rows;
    int tilesPerBlock = gridTilesPerBlock();
    int blockIndex = tilesPerBlock > 0 ? (tileIndex / tilesPerBlock) : 0;
    int localIndex = tilesPerBlock > 0 ? (tileIndex % tilesPerBlock) : 0;
    return map_tile_block_tx[blockIndex] + (rows > 0 ? (localIndex / rows) : 0);
}

int tileTyAt(int tileIndex)
{
    if (!usesGridTileLayout())
        return map_tile_ty[tileIndex];
    int rows = map_tile_grid_rows;
    int tilesPerBlock = gridTilesPerBlock();
    int blockIndex = tilesPerBlock > 0 ? (tileIndex / tilesPerBlock) : 0;
    int localIndex = tilesPerBlock > 0 ? (tileIndex % tilesPerBlock) : 0;
    return map_tile_block_ty[blockIndex] + (rows > 0 ? (localIndex % rows) : 0);
}

// Raw LZ4 block decompressor. Returns bytes written, or -1 on error.
int lz4_decompress(const uint8_t *src, int src_len, uint8_t *dst, int dst_cap)
{
    const uint8_t *s = src;
    const uint8_t *s_end = src + src_len;
    uint8_t *d = dst;
    const uint8_t *d_end = dst + dst_cap;
    while (s < s_end) {
        uint8_t token = *s++;
        int lit_len = (token >> 4) & 0xF;
        if (lit_len == 15) {
            uint8_t x;
            do {
                x = *s++;
                lit_len += x;
            } while (x == 255 && s < s_end);
        }
        if (d + lit_len > d_end || s + lit_len > s_end)
            return -1;
        memcpy(d, s, lit_len);
        d += lit_len;
        s += lit_len;
        if (s >= s_end)
            break;
        if (s + 2 > s_end)
            return -1;
        int offset = (int)s[0] | ((int)s[1] << 8);
        s += 2;
        if (offset == 0 || d - offset < dst)
            return -1;
        int mat_len = (token & 0xF) + 4;
        if (mat_len == 4 + 15) {
            uint8_t x;
            do {
                x = *s++;
                mat_len += x;
            } while (x == 255 && s < s_end);
        }
        if (d + mat_len > d_end)
            return -1;
        const uint8_t *m = d - offset;
        for (int i = 0; i < mat_len; i++)
            *d++ = m[i];
    }
    return (int)(d - dst);
}

// The compiled-in tiles are baked at 256px, but kTileSizePx (and so the buffer decodeTilePayload
// fills) is 512 in a BASEUI_HAS_MAP build - where MAP.BIN is the tile provider and MapTile.h is
// expected to be the empty stub that ships in the repo. Populating both at once would leave every
// compiled-in tile failing its decode-length check silently, so catch it at compile time instead.
static_assert(map_tile_count == 0 || NicheGraphics::MapTiles::kTileSizePx == 256,
              "compiled-in MapTile.h tiles are baked at 256px, but this build decodes tiles at 512px "
              "(BASEUI_HAS_MAP) - the two tile providers can't both be populated in one build");

const uint8_t *decodeSparseTile(int tileIndex)
{
    const uint8_t *compressed = map_tile_data + map_tile_offsets[tileIndex];
    bool ok = NicheGraphics::MapTiles::decodeTilePayload(map_tile_kinds[tileIndex], compressed, map_tile_sizes[tileIndex],
                                                         s_tileCacheBuffer);
    return ok ? s_tileCacheBuffer : nullptr;
}

NicheGraphics::MapTiles::TileSource *s_activeSource = nullptr;

int tileCountCompiledIn()
{
    return map_tile_count;
}

} // namespace

void NicheGraphics::MapTiles::setTileSource(TileSource *source)
{
    s_activeSource = source;
    s_cachedTileIndex = kNoCachedTile; // The old cached tile's data no longer applies.
}

bool NicheGraphics::MapTiles::decodeTilePayload(uint8_t kind, const uint8_t *compressed, int compressedSize, uint8_t *outBuf)
{
    if (kind == kTileKindWhite) {
        memset(outBuf, 0x00, kTileBufferBytes);
        return true;
    }
    if (kind == kTileKindBlack) {
        memset(outBuf, 0xFF, kTileBufferBytes);
        return true;
    }
    return lz4_decompress(compressed, compressedSize, outBuf, kTileBufferBytes) == kTileBufferBytes;
}

#if BASEUI_HAS_MAP
uint8_t *NicheGraphics::MapTiles::tileCompressedScratchBuffer()
{
    return s_tileCompressedScratchBuffer;
}
#endif

int NicheGraphics::MapTiles::zoomCount()
{
    if (s_activeSource)
        return s_activeSource->zoomCount();
    return usesGridTileLayout() ? map_tile_block_count : map_tile_count;
}

int NicheGraphics::MapTiles::zoomAt(int index)
{
    if (s_activeSource)
        return s_activeSource->zoomAt(index);
    return usesGridTileLayout() ? map_tile_block_zooms[index] : map_tile_zooms[index];
}

bool NicheGraphics::MapTiles::hasTiles()
{
    if (s_activeSource)
        return s_activeSource->tileCount() > 0;
    return map_tile_count > 0;
}

// Draw tiles centered on latCenter/lngCenter. Falls back to the nearest available zoom if
// no tiles exist at exactly zoom (upsamples), enabling smooth zoom steps.
void NicheGraphics::MapTiles::drawTileBackground(float latCenter, float lngCenter, int zoom, float metersToPx, int16_t viewWidth,
                                                 int16_t viewHeight, PlotFn plot, void *ctx)
{
    const int tileCount = s_activeSource ? s_activeSource->tileCount() : tileCountCompiledIn();
    if (tileCount == 0 || metersToPx <= 0.0f)
        return;

    const float R = 6378137.0f;
    const float latRad = latCenter * DEG_TO_RAD;
    const float mpp = (2.0f * (float)M_PI * R / (kWorldUnitsPerTile * (float)(1 << zoom))) * cosf(latRad);
    const float worldPxPerScreenPx = 1.0f / (metersToPx * mpp);

    // Find best tile zoom: highest available <= zoom, or lowest available if none below.
    int tileZoom = -1;
    for (int i = 0; i < zoomCount(); i++) {
        int z = zoomAt(i);
        if (z <= zoom && (tileZoom < 0 || z > tileZoom))
            tileZoom = z;
    }
    if (tileZoom < 0) {
        for (int i = 0; i < zoomCount(); i++) {
            int z = zoomAt(i);
            if (tileZoom < 0 || z < tileZoom)
                tileZoom = z;
        }
    }
    if (tileZoom < 0)
        return;

    // Convert screen-pixel movement into tileZoom coordinate space.
    // When tileZoom < zoom, tile pixels are upsampled (each tile pixel covers >1 screen px).
    const float tileWorldPx = worldPxPerScreenPx * ((float)(1 << tileZoom) / (float)(1 << zoom));

    const float sinLat = sinf(latRad);
    const float gpxX = ((lngCenter + 180.0f) / 360.0f) * (float)(1 << tileZoom) * kWorldUnitsPerTile;
    const float gpxY =
        (0.5f - logf((1.0f + sinLat) / (1.0f - sinLat)) / (4.0f * (float)M_PI)) * (float)(1 << tileZoom) * kWorldUnitsPerTile;

    const float minWx = gpxX - viewWidth * 0.5f * tileWorldPx;
    const float maxWx = gpxX + viewWidth * 0.5f * tileWorldPx;
    const float minWy = gpxY - viewHeight * 0.5f * tileWorldPx;
    const float maxWy = gpxY + viewHeight * 0.5f * tileWorldPx;

    // Longitude wraps at the antimeridian; latitude doesn't (standard Web Mercator). When zoomed
    // out enough (or panned near an edge) that the viewport extends past world x=0 or the world's
    // right edge, the missing side should show tiles wrapping in from the other side of the globe
    // rather than a blank void - so each tile is also tried shifted by one whole world-width in
    // either direction, and rendered at whichever shifted copy (if any) actually falls in view.
    const float worldWidthAtTileZoom = (float)(1 << tileZoom) * kWorldUnitsPerTile;

    // Decodes tile `i` (at unwrapped position tx,ty) at most once, then blits every screen-space
    // copy of it that falls in the (possibly antimeridian-wrapped) viewport.
    auto blitTile = [&](int i, int tx, int ty) {
        const float baseMinWx = tx * kWorldUnitsPerTile;
        const float tileMinWy = ty * kWorldUnitsPerTile;
        const float tileMaxWy = tileMinWy + kWorldUnitsPerTile;
        if (tileMaxWy < minWy || tileMinWy > maxWy)
            return; // No vertical wrap - skip entirely if this row is out of view.

        const uint8_t *tile = nullptr;

        for (int wrap = -1; wrap <= 1; wrap++) {
            const float tileMinWx = baseMinWx + wrap * worldWidthAtTileZoom;
            const float tileMaxWx = tileMinWx + kWorldUnitsPerTile;
            if (tileMaxWx < minWx || tileMinWx > maxWx)
                continue;

            if (!tile) { // Decode at most once per tile, regardless of how many copies are in view.
                if (s_cachedTileSource == s_activeSource && s_cachedTileIndex == i) {
                    tile = s_tileCacheBuffer; // Already sitting there from a previous call - reuse it.
                } else if (s_activeSource) {
                    tile = s_activeSource->decodeTile(i, s_tileCacheBuffer) ? s_tileCacheBuffer : nullptr;
                } else {
                    tile = decodeSparseTile(i);
                }
                if (!tile) {
                    // A failed decode may have partially overwritten s_tileCacheBuffer (e.g. LZ4
                    // erroring out mid-decompress) without this tile becoming the cached one -
                    // invalidate rather than risk a later cache "hit" serving that garbage back
                    // out under the previous (different) tile's index.
                    s_cachedTileIndex = kNoCachedTile;
                    break;
                }
                s_cachedTileSource = s_activeSource;
                s_cachedTileIndex = i;
            }

            const int sxStart = (int)((tileMinWx - gpxX) / tileWorldPx + viewWidth * 0.5f);
            const int sxEnd = (int)ceilf((tileMaxWx - gpxX) / tileWorldPx + viewWidth * 0.5f) - 1;
            const int syStart = (int)((tileMinWy - gpxY) / tileWorldPx + viewHeight * 0.5f);
            const int syEnd = (int)ceilf((tileMaxWy - gpxY) / tileWorldPx + viewHeight * 0.5f) - 1;

            const int sxLo = sxStart < 0 ? 0 : sxStart;
            const int sxHi = sxEnd > viewWidth - 1 ? viewWidth - 1 : sxEnd;
            const int syLo = syStart < 0 ? 0 : syStart;
            const int syHi = syEnd > viewHeight - 1 ? viewHeight - 1 : syEnd;

            for (int sy = syLo; sy <= syHi; sy++) {
                const float wy = gpxY + (sy - viewHeight * 0.5f) * tileWorldPx;
                const int py = (int)(wy - tileMinWy); // 0-255, standard world-relative range
                if (py < 0 || py >= (int)kWorldUnitsPerTile)
                    continue;

                for (int sx = sxLo; sx <= sxHi; sx++) {
                    const float wx = gpxX + (sx - viewWidth * 0.5f) * tileWorldPx;
                    const int px = (int)(wx - tileMinWx); // 0-255, standard world-relative range
                    if (px < 0 || px >= (int)kWorldUnitsPerTile)
                        continue;

                    // Scale from the standard 0-255 world-relative range to this tile's actual
                    // stored-pixel index (1:1 at 256px tiles, 2x at 512px, etc).
                    const int storedPx = (int)(px * kStoredPxPerWorldUnit);
                    const int storedPy = (int)(py * kStoredPxPerWorldUnit);

                    if (!(tile[(storedPx / 8) * kTileSizePx + storedPy] & (1 << (storedPx % 8))))
                        continue;

                    plot(ctx, (int16_t)sx, (int16_t)sy);
                }
            }
        }
    };

    if (s_activeSource && s_activeSource->supportsDirectLookup()) {
        // A worldwide deep-zoom bake is millions of tiles - iterating tileCount() below to find
        // which ones overlap the viewport would be far too slow (this is what made a z0-10 bake
        // effectively hang the T-Deck even after fixing the RAM crash). Compute directly which
        // (small) handful of tx/ty tiles the viewport actually needs instead.
        const int side = 1 << tileZoom;
        // blitTile does its own -1/0/+1 wrap pass, so where these tx ranges overlap a tile can be
        // visited more than once. Plotting is idempotent and the decode-once guard inside blitTile
        // means a repeat visit costs only the (clipped, usually empty) blit loop, so this is left
        // alone rather than deduplicated - the ranges only overlap when zoomed far enough out for
        // the whole world to be on screen, where there are few tiles to begin with.
        for (int wrap = -1; wrap <= 1; wrap++) {
            const float shift = wrap * worldWidthAtTileZoom;
            int txLo = (int)floorf((minWx - shift) / kWorldUnitsPerTile);
            int txHi = (int)floorf((maxWx - shift) / kWorldUnitsPerTile);
            if (txLo < 0)
                txLo = 0;
            if (txHi > side - 1)
                txHi = side - 1;
            if (txLo > txHi)
                continue;

            int tyLo = (int)floorf(minWy / kWorldUnitsPerTile);
            int tyHi = (int)floorf(maxWy / kWorldUnitsPerTile);
            if (tyLo < 0)
                tyLo = 0;
            if (tyHi > side - 1)
                tyHi = side - 1;

            for (int ty = tyLo; ty <= tyHi; ty++) {
                for (int tx = txLo; tx <= txHi; tx++) {
                    const int i = s_activeSource->indexOf(tileZoom, tx, ty);
                    if (i < 0)
                        continue;
                    blitTile(i, tx, ty);
                }
            }
        }
        return;
    }

    for (int i = 0; i < tileCount; i++) {
        const int tzoom = s_activeSource ? s_activeSource->tileZoomAt(i) : tileZoomAt(i);
        if (tzoom != tileZoom)
            continue;

        const int tx = s_activeSource ? s_activeSource->tileTxAt(i) : tileTxAt(i);
        const int ty = s_activeSource ? s_activeSource->tileTyAt(i) : tileTyAt(i);
        blitTile(i, tx, ty);
    }
}
