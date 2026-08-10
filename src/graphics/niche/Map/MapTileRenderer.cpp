#include "./MapTileRenderer.h"
#include "./MapTile.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(ARCH_ESP32)
#include <esp_heap_caps.h> // heap_caps_malloc(), for the PSRAM tile cache slots
#endif

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

// Decoded-tile cache, keyed on (source, tileIndex), so blitTile can skip a redundant decode (SD
// read + LZ4 decompress) when a tile is needed again.
//
// This used to be a single slot, on the reasoning that most Map-capable screens are smaller than
// one tile so a whole redraw needs only one. A tile spans 256 world units, so that holds for a
// 320x240 panel and fails badly for anything larger: a 410x502 screen straddles up to 3x3 tiles,
// the single slot evicted itself on every one, and each pan frame re-read and re-decompressed all
// nine from SD. Measured at 200-270 ms per frame with a 0% hit rate, against a 28 ms panel push.
//
// Slot 0 is the static buffer above, so a build with no PSRAM - or one where the allocation below
// fails - still works exactly as it did before, just without the extra slots. nullptr is a valid
// source key (the compiled-in MapTile.h path), distinct from "nothing cached" (kNoCachedTile).
constexpr int kNoCachedTile = -1;

// 3x3 is the worst case for a viewport up to 512 world units on a side, which covers every panel
// this runs on. At kTileBufferBytes (32KB) a slot each, the eight non-static ones are 256KB of
// PSRAM - cheap next to what they save, but override this downwards for a tighter board.
#ifndef MAP_TILE_CACHE_SLOTS
#define MAP_TILE_CACHE_SLOTS 9
#endif

struct TileCacheSlot {
    uint8_t *bits;
    NicheGraphics::MapTiles::TileSource *source;
    int index;
    uint32_t lastUsed;
};
TileCacheSlot s_tileCache[MAP_TILE_CACHE_SLOTS];
bool s_tileCacheReady = false;
uint32_t s_tileCacheClock = 0;

void tileCacheInit()
{
    if (s_tileCacheReady)
        return;
    s_tileCacheReady = true;

    s_tileCache[0] = {s_tileCacheBuffer, nullptr, kNoCachedTile, 0};
    for (int i = 1; i < MAP_TILE_CACHE_SLOTS; i++)
        s_tileCache[i] = {nullptr, nullptr, kNoCachedTile, 0}; // A null buf never matches or wins eviction.

        // Extra slots only where there is memory to spare for them. InkHUD's compiled-in tile path runs
        // on nRF52 with a small screen, where one slot already covers the viewport and 32KB apiece would
        // be hopeless - it keeps the static slot alone and behaves exactly as it always has.
#if defined(ARCH_ESP32) || defined(ARCH_PORTDUINO)
    for (int i = 1; i < MAP_TILE_CACHE_SLOTS; i++) {
#if defined(ARCH_ESP32)
        // Deliberately PSRAM: 32KB a slot would swallow the internal heap, and the alternative to a
        // slot in slower RAM is an SD read plus an LZ4 decompress, which is far worse.
        s_tileCache[i].bits = (uint8_t *)heap_caps_malloc(NicheGraphics::MapTiles::kTileBufferBytes, MALLOC_CAP_SPIRAM);
#else
        s_tileCache[i].bits = (uint8_t *)malloc(NicheGraphics::MapTiles::kTileBufferBytes);
#endif
        if (!s_tileCache[i].bits)
            break; // Out of room - however many we got is what the cache gets to use.
    }
#endif
}

// The decoded tile for (source, index), or nullptr if it isn't cached.
uint8_t *tileCacheFind(NicheGraphics::MapTiles::TileSource *source, int index)
{
    tileCacheInit();
    for (int i = 0; i < MAP_TILE_CACHE_SLOTS; i++) {
        if (s_tileCache[i].bits && s_tileCache[i].index == index && s_tileCache[i].source == source) {
            s_tileCache[i].lastUsed = ++s_tileCacheClock;
            return s_tileCache[i].bits;
        }
    }
    return nullptr;
}

// A buffer to decode into, evicting whichever slot has gone longest unused. Left marked empty until
// tileCacheCommit(), so a decode that fails partway can't be served back out as a later hit.
uint8_t *tileCacheAcquire()
{
    tileCacheInit();
    int victim = 0;
    for (int i = 1; i < MAP_TILE_CACHE_SLOTS; i++) {
        if (!s_tileCache[i].bits)
            continue;
        if (!s_tileCache[victim].bits || s_tileCache[i].lastUsed < s_tileCache[victim].lastUsed)
            victim = i;
    }
    s_tileCache[victim].source = nullptr;
    s_tileCache[victim].index = kNoCachedTile;
    return s_tileCache[victim].bits;
}

void tileCacheCommit(uint8_t *bits, NicheGraphics::MapTiles::TileSource *source, int index)
{
    for (int i = 0; i < MAP_TILE_CACHE_SLOTS; i++) {
        if (s_tileCache[i].bits == bits) {
            s_tileCache[i].source = source;
            s_tileCache[i].index = index;
            s_tileCache[i].lastUsed = ++s_tileCacheClock;
            return;
        }
    }
}

void tileCacheInvalidateAll()
{
    tileCacheInit();
    for (int i = 0; i < MAP_TILE_CACHE_SLOTS; i++) {
        s_tileCache[i].source = nullptr;
        s_tileCache[i].index = kNoCachedTile;
    }
}

#ifdef UI_PERF_DEBUG
// Reset per drawTileBackground() call, so the caller can report how the cache above actually fares.
// While panning, a healthy ratio is mostly hits: only tiles newly entering the viewport should need
// decoding. Sustained decodes with no hits means the cache is too small for the viewport.
uint32_t s_tileDecodes = 0;
uint32_t s_tileCacheHits = 0;
#endif

// Shared compressed-payload scratch buffer - see tileCompressedScratchBuffer() in the header.
// Only where a TileSource can exist; InkHUD's compiled-in path decodes straight from flash.
//
// Allocated on first use rather than declared as an array, because as .bss at kTileBufferBytes
// (32KB with BASEUI_HAS_MAP's 512px tiles) this was the single largest internal-DRAM object in
// the firmware - and it was paid at link time on every build that can show a map, whether or not
// the map was ever opened. On a t-watch-ultra that mattered: internal DRAM sat around 45KB free,
// so this one buffer was most of the missing headroom, and an I2S DMA ring failing to allocate
// mid-session is the sort of thing it caused.
//
// PSRAM first: this holds an SD read destination and an LZ4 input, both streamed through once
// per decode rather than random-accessed, so the slower memory costs little. Internal is the
// fallback, which keeps boards without PSRAM working - and even there this is now paid only if
// a map is actually opened.
#if BASEUI_HAS_MAP
uint8_t *s_tileCompressedScratchBuffer = nullptr;
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

bool decodeSparseTileInto(int tileIndex, uint8_t *outBuf)
{
    const uint8_t *compressed = map_tile_data + map_tile_offsets[tileIndex];
    return NicheGraphics::MapTiles::decodeTilePayload(map_tile_kinds[tileIndex], compressed, map_tile_sizes[tileIndex], outBuf);
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
    tileCacheInvalidateAll(); // The old cached tiles' data no longer applies.
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
    if (!s_tileCompressedScratchBuffer) {
#if defined(ARCH_ESP32)
        s_tileCompressedScratchBuffer = (uint8_t *)heap_caps_malloc(kTileBufferBytes, MALLOC_CAP_SPIRAM);
#endif
        if (!s_tileCompressedScratchBuffer)
            s_tileCompressedScratchBuffer = (uint8_t *)malloc(kTileBufferBytes);
    }
    // May still be null: callers treat that as a failed decode, which is what the renderer
    // already does with any tile it cannot read, so a map simply draws blank rather than
    // taking the device down. Deliberately not logged - this file has no logging dependency.
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
#ifdef UI_PERF_DEBUG
// Tile decodes vs single-slot cache hits during the most recent drawTileBackground() call.
void NicheGraphics::MapTiles::lastTileStats(uint32_t *decodes, uint32_t *cacheHits)
{
    *decodes = s_tileDecodes;
    *cacheHits = s_tileCacheHits;
}
#endif

void NicheGraphics::MapTiles::drawTileBackground(float latCenter, float lngCenter, int zoom, float metersToPx, int16_t viewWidth,
                                                 int16_t viewHeight, PlotFn plot, void *ctx)
{
#ifdef UI_PERF_DEBUG
    s_tileDecodes = 0;
    s_tileCacheHits = 0;
#endif

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
                tile = tileCacheFind(s_activeSource, i);
                if (tile) {
#ifdef UI_PERF_DEBUG
                    s_tileCacheHits++;
#endif
                } else {
#ifdef UI_PERF_DEBUG
                    s_tileDecodes++;
#endif
                    uint8_t *dst = tileCacheAcquire();
                    const bool ok = dst && (s_activeSource ? s_activeSource->decodeTile(i, dst) : decodeSparseTileInto(i, dst));
                    if (!ok) {
                        // A failed decode may have partially overwritten the slot (e.g. LZ4 erroring
                        // out mid-decompress). tileCacheAcquire() already left it marked empty and
                        // we don't commit it, so that garbage can never come back as a later hit.
                        break;
                    }
                    tileCacheCommit(dst, s_activeSource, i);
                    tile = dst;
                }
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
