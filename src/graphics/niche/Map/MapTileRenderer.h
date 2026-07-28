#pragma once

/*

Framework-agnostic core for rendering baked-in 1-bit map tiles (see MapTile.h).

Shared by InkHUD's MapApplet and BaseUI's MapRenderer, so the tile format,
LZ4 decoding, and Mercator projection math exist in exactly one place.

Callers supply a plot callback instead of calling display primitives
directly, since InkHUD (Applet::drawPixel) and BaseUI (OLEDDisplay /
TFTDisplay) have different drawing APIs.

*/

#include <stdint.h>

// Tile edge length in pixels - a compile-time choice per build, not a runtime option: a given
// firmware image only ever uses one tile size. Defaults to 256 (the flash/RAM-constrained
// choice - e.g. Wio Tracker L1's QSPI budget). Builds with storage to spare and no resolution
// constraint (e.g. an SD card, or native/portduino) can override to 512 (MapTiler's native
// fetch resolution - avoids the downsample-then-threshold quality loss of shrinking to 256)
// via -D MAP_TILE_SIZE_PX=512.
#ifndef MAP_TILE_SIZE_PX
#define MAP_TILE_SIZE_PX 256
#endif

namespace NicheGraphics::MapTiles
{

using PlotFn = void (*)(void *ctx, int16_t x, int16_t y);

constexpr int kTileSizePx = MAP_TILE_SIZE_PX;
constexpr int kTileBufferBytes = kTileSizePx * kTileSizePx / 8; // 1bpp

// Pluggable source of baked tile data. The default (no source registered) reads the
// compiled-in MapTile.h arrays, which is all InkHUD needs (small, hand-provided datasets
// that fit in internal flash). Platforms with room for a real worldwide basemap - too big to
// compile in - register their own source instead: e.g. a QSPI+FAT file reader on the Wio
// Tracker L1, or a plain FSCom file reader on ESP32/native (see MapTileSourceFile.h) where the
// filesystem has ample room.
class TileSource
{
  public:
    virtual ~TileSource() = default;
    virtual int zoomCount() = 0;
    virtual int zoomAt(int index) = 0;
    virtual int tileCount() = 0;
    virtual int tileZoomAt(int tileIndex) = 0;
    virtual int tileTxAt(int tileIndex) = 0;
    virtual int tileTyAt(int tileIndex) = 0;
    // Decodes tile `tileIndex` into `outBuf` (exactly kTileBufferBytes: kTileSizePx square,
    // 1bpp, column-major). Returns false if the tile couldn't be read/decoded.
    virtual bool decodeTile(int tileIndex, uint8_t *outBuf) = 0;
};

// Registers the active tile source. Pass nullptr to revert to the compiled-in MapTile.h data.
// Caller retains ownership - MapTiles never deletes it.
void setTileSource(TileSource *source);

// Tile payload "kind" byte, shared by MapTile.h and any external TileSource's own index -
// see generate_map_tiles.py / MapTile.h for the canonical definitions.
constexpr uint8_t kTileKindLZ4 = 0;   // payload is an LZ4 raw block, decompresses to kTileBufferBytes
constexpr uint8_t kTileKindWhite = 1; // no payload - tile is entirely unset (0x00) pixels
constexpr uint8_t kTileKindBlack = 2; // no payload - tile is entirely set (0xFF) pixels

// Decodes one tile's payload into outBuf (kTileSizePx square, 1bpp, column-major - exactly
// kTileBufferBytes). `compressed`/`compressedSize` are ignored for kTileKindWhite/kTileKindBlack.
// Returns false if LZ4 decompression fails or doesn't produce exactly kTileBufferBytes. Exposed
// so TileSource implementations backed by external storage (QSPI, filesystem, SD) can reuse the
// same LZ4 raw-block decoder as the compiled-in path, rather than duplicating it.
bool decodeTilePayload(uint8_t kind, const uint8_t *compressed, int compressedSize, uint8_t *outBuf);

// Number of distinct zoom levels present in the baked tile set (accounts for both
// sparse and grid tile layouts - see MapTile.h). Returns 0 if no tiles are baked in.
int zoomCount();

// The Nth distinct zoom level present in the baked tile set (0 <= index < zoomCount()).
int zoomAt(int index);

// True if any tile data has been baked into this build at all.
bool hasTiles();

// Draw the baked tile background for a view centered at (latCenter, lngCenter), at the
// given zoom, into a viewWidth x viewHeight viewport. metersToPx converts world meters to
// screen pixels (same convention as MapApplet's fit-to-nodes scale). Falls back to the
// nearest available baked zoom if no tile exists at exactly `zoom`, so callers can request
// smooth intermediate zoom steps. Calls `plot(ctx, x, y)` once per set pixel; callers own
// clearing the background and drawing everything else (markers, scale bars, etc).
void drawTileBackground(float latCenter, float lngCenter, int zoom, float metersToPx, int16_t viewWidth,
                         int16_t viewHeight, PlotFn plot, void *ctx);

} // namespace NicheGraphics::MapTiles
