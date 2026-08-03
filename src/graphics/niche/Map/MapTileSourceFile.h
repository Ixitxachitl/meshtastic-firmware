#pragma once

/*

TileSource backed by a plain file on FSCom (see FSCommon.h), read via the standard Arduino
File API (open/seek/read). Suitable wherever the filesystem has room to spare for a real
worldwide basemap: ESP32 (LittleFS on internal flash, typically several MB) and native/portduino
(PortduinoFS, backed directly by the host OS filesystem - no size constraint at all).

Not used on nRF52: FSCom there is InternalFS, a ~28KB partition nowhere near big enough for a real
basemap, and the Wio Tracker L1 (the only nRF52 Map target so far) has neither an SD slot nor
enough external flash to make a worldwide bake viable by any other means either.

Reads the blob format documented in MapTileBlobFormat.h - each zoom present spans one rectangle
(xMin/yMin/width/height), tiles emitted densely in ascending (zoom, ty, tx) order within it.

Holds NO per-tile index in RAM - just the small per-zoom rectangle table (see ranges_ below, at
most kTileBlobMaxZoomRanges entries), so a tile's position in the index is a table scan plus
arithmetic (see indexOf()) instead of a per-tile lookup. A worldwide z0-10 bake is 1.4 million
tiles; a per-tile RAM index at ~16 bytes each (struct padding) is ~22MB, which is what crashed
this on the Wio Tracker L1/T-Deck - this design costs O(1) RAM regardless of how deep the bake
goes, at the cost of one small extra seek+read per tile decoded (looking up that tile's own index
entry on demand) instead of a RAM lookup - negligible next to the cost of the tile payload read
itself.

*/

#include "./MapTileBlobFormat.h" // TileBlobZoomRange, kTileBlobMaxZoomRanges
#include "./MapTileRenderer.h"
#include "FSCommon.h" // File, and configuration.h for BASEUI_HAS_MAP

// See MapTileSourceSD.h - this source is BaseUI-map-only, so it compiles away with the frame.
#if BASEUI_HAS_MAP

namespace NicheGraphics::MapTiles
{

class FileTileSource : public TileSource
{
  public:
    // Opens `path` via FSCom and reads its header (+ zoom-range table + a sanity-check entry -
    // see .cpp). Returns false if the file is missing, too short, fails the magic-number check,
    // or the zoom-range table doesn't validate against tile_count - callers should still register
    // this source via setTileSource() regardless, so hasTiles() cleanly reports false rather than
    // leaving the compiled-in MapTile.h data (also empty, in any build shipping this path) active.
    bool begin(const char *path);

    int zoomCount() override { return rangeCount_; }
    int zoomAt(int index) override { return ranges_[index].zoom; }
    int tileCount() override { return (int)count_; }
    int tileZoomAt(int tileIndex) override;
    int tileTxAt(int tileIndex) override;
    int tileTyAt(int tileIndex) override;
    bool decodeTile(int tileIndex, uint8_t *outBuf) override;

    bool supportsDirectLookup() override { return true; }
    int indexOf(int zoom, int tx, int ty) override;

  private:
    // Kept open for the lifetime of a successful begin() rather than reopened per decodeTile()
    // call - see MapTileSourceSD.h's file_ for why (same reasoning; less severe on flash-backed
    // filesystems than on real SD cards, but still avoidable per-tile-per-frame overhead).
    File file_;
    TileBlobZoomRange ranges_[kTileBlobMaxZoomRanges] = {};
    int rangeCount_ = 0;
    uint32_t count_ = 0;
    uint32_t indexTableStart_ = 0; // where the tile-entry table starts (after header + zoom-range table)
    uint32_t payloadStart_ = 0;
    uint32_t payloadBytes_ = 0; // size of the payload region, so an entry's offset/size can be bounds-checked
    char path_[64] = {};
};

} // namespace NicheGraphics::MapTiles

#endif // BASEUI_HAS_MAP
