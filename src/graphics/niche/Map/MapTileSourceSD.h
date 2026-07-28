#pragma once

/*

TileSource backed by a file on a real SD card, read via SdFat's `SdFs` class (FAT16/FAT32/exFAT).
The plain Arduino `SD` global used elsewhere in this codebase (see setupSDCard() in FSCommon.cpp)
only supports FAT16/32 - exFAT-formatted cards (the default modern-OS format for cards over 32GB)
silently fail to mount under it. meshtastic-device-ui's own SD map cache already relies on SdFs for
exactly this reason (see its SdCard.cpp) - this mirrors that same, proven-working init so BaseUI's
Map screen isn't limited to FAT32-only cards either. Own independent SdFs instance/init (SHARED_SPI
- coexists with FSCommon's separate plain-SD.h init on the same bus/pins, same pattern
meshtastic-device-ui's SdFsCard::init() already relies on for this hardware).

Reads the same blob format as MapTileSourceFile/MapTileSourceQSPI (see bin/generate_map_tiles.py):
    u32 magic 'MTLB', u32 tile_count
    tile_count * { u8 zoom, u16 tx, u16 ty, u8 kind, u32 offset, u16 size }
    followed by concatenated payload bytes (offset is relative to the start of this region).

*/

#if defined(HAS_SDCARD)

#include "./MapTileRenderer.h"

#include <SdFat.h>
#include <vector>

namespace NicheGraphics::MapTiles
{

class SDCardTileSource : public TileSource
{
  public:
    // Mounts the card (own independent SdFs init, see .cpp) and reads `path`'s tile index into
    // RAM. Callers should register this source via setTileSource() regardless of the return
    // value, so hasTiles() cleanly reports false rather than leaving the compiled-in MapTile.h
    // data active by accident.
    bool begin(const char *path = "/MAP.BIN");

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

    SdFs sd_;
    bool sdBegun_ = false;
    std::vector<IndexEntry> index_;
    std::vector<int> zoomLevels_;
    uint32_t payloadStart_ = 0;
    char path_[64] = {};
};

} // namespace NicheGraphics::MapTiles

#endif
