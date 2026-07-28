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

*/

#include "./MapTileRenderer.h"

#include <vector>

namespace NicheGraphics::MapTiles
{

class FileTileSource : public TileSource
{
  public:
    // Opens `path` via FSCom and reads its tile index into RAM (small: ~12 bytes/tile).
    // Returns false if the file is missing, too short, or fails the magic-number check - callers
    // should still register this source via setTileSource() in that case, since an empty source
    // reports hasTiles()==false cleanly rather than silently leaving the compiled-in MapTile.h
    // data (also empty, in any build that ships this file-backed path) active by accident.
    bool begin(const char *path);

    int zoomCount() override;
    int zoomAt(int index) override;
    int tileCount() override { return (int)index_.size(); }
    int tileZoomAt(int tileIndex) override { return index_[tileIndex].zoom; }
    int tileTxAt(int tileIndex) override { return index_[tileIndex].tx; }
    int tileTyAt(int tileIndex) override { return index_[tileIndex].ty; }
    bool decodeTile(int tileIndex, uint8_t *outBuf) override;

  private:
    struct IndexEntry {
        uint8_t zoom;
        uint16_t tx;
        uint16_t ty;
        uint8_t kind;
        uint32_t offset;
        uint16_t size;
    };

    std::vector<IndexEntry> index_;
    std::vector<int> zoomLevels_; // distinct zoom values present, ascending
    uint32_t payloadStart_ = 0;
    char path_[64] = {};
};

} // namespace NicheGraphics::MapTiles
