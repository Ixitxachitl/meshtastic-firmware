#pragma once

/*

TileSource backed by a plain file on FSCom (see FSCommon.h), read via the standard Arduino
File API (open/seek/read). Suitable wherever the filesystem has room to spare for a real
worldwide basemap: ESP32 (LittleFS on internal flash, typically several MB) and native/portduino
(PortduinoFS, backed directly by the host OS filesystem - no size constraint at all).

Not used on nRF52: FSCom there is InternalFS, a ~28KB partition nowhere near big enough - see
MapTileSourceQSPI.h for that platform's approach instead (reading the same blob format from a
file on the external QSPI chip's FAT filesystem).

Reads the blob format written by bin/generate_map_tiles.py:
    u32 magic 'MTLB', u32 tile_count
    tile_count * { u8 zoom, u16 tx, u16 ty, u8 kind, u32 offset, u16 size }
    followed by concatenated payload bytes (offset is relative to the start of this region).

Holds NO per-tile index in RAM. bin/generate_map_tiles.py always emits every (zoom, tx, ty) in a
requested zoom range, in a dense, predictable raster order (zoom ascending, then ty, then tx) -
so a tile's position in the index is just arithmetic (see indexOf()), computable from only the
min/max zoom present. A worldwide z0-10 bake is 1.4 million tiles; a per-tile RAM index at
~16 bytes each (struct padding) is ~22MB, which is what crashed this on the Wio Tracker L1/T-Deck -
this design costs O(1) RAM (a handful of ints) regardless of how deep the bake goes, at the cost of
one small extra seek+read per tile decoded (looking up that tile's own index entry on demand)
instead of a RAM lookup - negligible next to the cost of the tile payload read itself.

*/

#include "./MapTileRenderer.h"

namespace NicheGraphics::MapTiles
{

class FileTileSource : public TileSource
{
  public:
    // Opens `path` via FSCom and reads its header (+ two sanity-check entries - see .cpp).
    // Returns false if the file is missing, too short, fails the magic-number check, or doesn't
    // look like the dense/contiguous layout this class assumes - callers should still register
    // this source via setTileSource() regardless, so hasTiles() cleanly reports false rather than
    // leaving the compiled-in MapTile.h data (also empty, in any build shipping this path) active.
    bool begin(const char *path);

    int zoomCount() override { return zHi_ >= zLo_ ? zHi_ - zLo_ + 1 : 0; }
    int zoomAt(int index) override { return zLo_ + index; }
    int tileCount() override { return (int)count_; }
    int tileZoomAt(int tileIndex) override;
    int tileTxAt(int tileIndex) override;
    int tileTyAt(int tileIndex) override;
    bool decodeTile(int tileIndex, uint8_t *outBuf) override;

    bool supportsDirectLookup() override { return true; }
    int indexOf(int zoom, int tx, int ty) override;

  private:
    // Cumulative tile count for all zooms below `zoom` (i.e. this zoom's base index), given the
    // dense/contiguous assumption: sum_{z=zLo_}^{zoom-1} 4^z.
    uint32_t baseIndexForZoom(int zoom) const;

    int zLo_ = 0;
    int zHi_ = -1; // -1 => no tiles / not begun.
    uint32_t count_ = 0;
    uint32_t payloadStart_ = 0;
    char path_[64] = {};
};

} // namespace NicheGraphics::MapTiles
