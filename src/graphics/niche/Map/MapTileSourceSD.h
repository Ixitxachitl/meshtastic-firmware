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

Reads the blob format documented in MapTileBlobFormat.h - each zoom present spans one rectangle
(xMin/yMin/width/height), tiles emitted densely in ascending (zoom, ty, tx) order within it.

Holds NO per-tile index in RAM - see MapTileSourceFile.h for why (same reasoning, same small
per-zoom-rectangle-table + O(1)-arithmetic lookup, just via SdFat instead of FSCom). This is what
crashed a worldwide z0-10 bake (1.4M tiles) on the T-Deck: the old version loaded every tile's
12-byte record into a std::vector, ~22MB for that many tiles - far more than fits in RAM.

*/

// configuration.h (via variant.h) is what actually defines HAS_SDCARD - it's not a compiler -D
// flag, so it must be included before the guard below can see it, or this whole header (and any
// .cpp that includes it first, before anything else pulls in configuration.h) silently
// preprocesses away to nothing (link errors, no compile error) - see MapTileSourceFile.cpp's
// FSCommon.h include for the same trap with ARCH_ESP32/ARCH_PORTDUINO.
#include "configuration.h"

// BASEUI_HAS_MAP too: this source exists solely to feed BaseUI's map frame (MapRenderer.cpp is the
// only caller of setTileSource). InkHUD's map applet uses the compiled-in MapTile.h fallback
// instead, so gating here keeps map-disabled builds from pulling SdFat in via the include below.
#if defined(HAS_SDCARD) && BASEUI_HAS_MAP

#include "./MapTileBlobFormat.h" // TileBlobZoomRange, kTileBlobMaxZoomRanges
#include "./MapTileRenderer.h"

#include <SdFat.h>

namespace NicheGraphics::MapTiles
{

class SDCardTileSource : public TileSource
{
  public:
    // Mounts the card (own independent SdFs init, see .cpp) and reads `path`'s header (+ zoom-
    // range table + a sanity-check entry - see .cpp). Callers should register this source via
    // setTileSource() regardless of the return value, so hasTiles() cleanly reports false rather
    // than leaving the compiled-in MapTile.h data active by accident.
    bool begin(const char *path = "/MAP.BIN");

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
    SdFs sd_;
    // Kept open for the lifetime of a successful begin() rather than reopened per decodeTile()
    // call - SD file opens walk the FAT to resolve the path and are the dominant per-tile cost
    // (far more than the read itself or the LZ4 decompression), so reopening by path on every
    // single tile of every single frame redraw was the main cause of sluggish map rendering.
    FsFile file_;
    bool sdBegun_ = false;
    TileBlobZoomRange ranges_[kTileBlobMaxZoomRanges] = {};
    int rangeCount_ = 0;
    uint32_t count_ = 0;
    uint32_t indexTableStart_ = 0; // where the tile-entry table starts (after header + zoom-range table)
    uint32_t payloadStart_ = 0;
    uint32_t payloadBytes_ = 0; // size of the payload region, so an entry's offset/size can be bounds-checked
    char path_[64] = {};
};

} // namespace NicheGraphics::MapTiles

#endif
