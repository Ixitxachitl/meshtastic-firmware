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

Reads the blob format written by bin/generate_map_tiles.py:
    u32 magic 'MTLB', u32 tile_count
    tile_count * { u8 zoom, u16 tx, u16 ty, u8 kind, u32 offset, u16 size }
    followed by concatenated payload bytes (offset is relative to the start of this region).

Holds NO per-tile index in RAM - see MapTileSourceFile.h for why (same reasoning, same dense/
contiguous-zoom-range assumption about the blob layout, same O(1)-memory algebraic lookup). This is
what crashed a worldwide z0-10 bake (1.4M tiles) on the T-Deck: the old version loaded every tile's
12-byte record into a std::vector, ~22MB for that many tiles - far more than fits in RAM.

*/

#if defined(HAS_SDCARD)

#include "./MapTileRenderer.h"

#include <SdFat.h>

namespace NicheGraphics::MapTiles
{

class SDCardTileSource : public TileSource
{
  public:
    // Mounts the card (own independent SdFs init, see .cpp) and reads `path`'s header (+ two
    // sanity-check entries - see .cpp). Callers should register this source via setTileSource()
    // regardless of the return value, so hasTiles() cleanly reports false rather than leaving the
    // compiled-in MapTile.h data active by accident.
    bool begin(const char *path = "/MAP.BIN");

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
    uint32_t baseIndexForZoom(int zoom) const;

    SdFs sd_;
    bool sdBegun_ = false;
    int zLo_ = 0;
    int zHi_ = -1; // -1 => no tiles / not begun.
    uint32_t count_ = 0;
    uint32_t payloadStart_ = 0;
    char path_[64] = {};
};

} // namespace NicheGraphics::MapTiles

#endif
