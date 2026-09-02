#pragma once

/*

TileSource for the SenseCAP Indicator, whose SD card hangs off the RP2040 co-processor rather than
the ESP32. Reads the same blob format as SDCardTileSource (see MapTileBlobFormat.h) from the same
file the user writes to the card, but every byte range comes over the interdevice serial link via
SensecapIndicator::file_read() in 4KB chunks instead of SdFat seek+read. No SPI is touched, so
unlike the SD source this takes no spiLock.

Same RAM discipline as the other sources: no per-tile index in memory, just the small zoom-range
table and O(1) arithmetic lookup.

*/

#include "configuration.h"

#if defined(SENSECAP_INDICATOR) && BASEUI_HAS_MAP

#include "./MapTileBlobFormat.h" // TileBlobZoomRange, kTileBlobMaxDistinctZooms
#include "./MapTileRenderer.h"

#include <memory>

namespace NicheGraphics::MapTiles
{

class IndicatorTileSource : public TileSource
{
  public:
    // Reads `path`'s header (+ zoom-range table + a sanity-check entry) over the co-processor link.
    // Register the source via setTileSource() regardless of the return value, so hasTiles() reports
    // false rather than leaving the compiled-in MapTile.h data active by accident.
    bool begin(const char *path = "/MAP.BIN");

    int zoomCount() override { return zoomCount_; }
    int zoomAt(int index) override { return zooms_[index]; }
    int tileCount() override { return (int)count_; }
    int tileZoomAt(int tileIndex) override;
    int tileTxAt(int tileIndex) override;
    int tileTyAt(int tileIndex) override;
    bool decodeTile(int tileIndex, uint8_t *outBuf) override;

    bool supportsDirectLookup() override { return true; }
    int indexOf(int zoom, int tx, int ty) override;

  private:
    // Read exactly `len` bytes at `offset` into buf, chunked at the link's per-request limit.
    // fileSize (if non-null) receives the file's total size as reported by the co-processor.
    bool readAt(uint32_t offset, uint8_t *buf, uint32_t len, uint64_t *fileSize = nullptr);

    bool opened_ = false;
    std::unique_ptr<TileBlobZoomRange[]> ranges_;
    int rangeCount_ = 0;
    uint8_t zooms_[kTileBlobMaxDistinctZooms] = {};
    int zoomCount_ = 0;
    uint32_t count_ = 0;
    uint32_t indexTableStart_ = 0;
    uint32_t payloadStart_ = 0;
    uint32_t payloadBytes_ = 0;
    char path_[64] = {};
};

} // namespace NicheGraphics::MapTiles

#endif
