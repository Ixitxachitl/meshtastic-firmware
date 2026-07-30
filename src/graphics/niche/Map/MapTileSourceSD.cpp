#include "./MapTileSourceSD.h"

#if defined(HAS_SDCARD)

#include "DebugConfiguration.h"
#include "configuration.h" // SDCARD_CS (variant.h)

#include "./MapTileBlobFormat.h"

#include <SPI.h>
#include <string.h>

using namespace NicheGraphics::MapTiles;

namespace
{
#ifndef SD_SPI_FREQUENCY
#define SD_SPI_FREQUENCY 4000000U
#endif

bool readEntryAt(FsFile &file, uint32_t indexTableStart, uint32_t entryIndex, TileBlobEntry &out)
{
    uint8_t buf[kTileBlobEntrySize];
    if (!file.seekSet(indexTableStart + (uint64_t)entryIndex * kTileBlobEntrySize))
        return false;
    if (file.read(buf, kTileBlobEntrySize) != (int)kTileBlobEntrySize)
        return false;
    decodeTileBlobEntry(buf, out);
    return true;
}
} // namespace

bool SDCardTileSource::begin(const char *path)
{
    rangeCount_ = 0;
    count_ = 0;
    indexTableStart_ = 0;
    payloadStart_ = 0;
    if (file_)
        file_.close();

    if (!sdBegun_) {
        // FSCommon's setupSDCard() already brought up the SPI bus pins at boot (plain Arduino SD
        // library, FAT16/32 only) - SHARED_SPI here lets SdFs mount the same card independently
        // (its own CMD0/init handshake) without fighting over the bus, exactly like
        // meshtastic-device-ui's SdFsCard::init() already does successfully on this hardware.
        sdBegun_ = sd_.begin(SdSpiConfig(SDCARD_CS, SHARED_SPI, SD_SPI_FREQUENCY, &SPI));
    }
    if (!sdBegun_) {
        LOG_WARN("Map: no SD card detected");
        return false;
    }

    // Opened once and kept open for decodeTile() to reuse (see file_'s doc comment) - only closed
    // again below on a failure path, where nothing will call decodeTile() anyway.
    file_ = sd_.open(path, O_RDONLY);
    if (!file_) {
        LOG_WARN("Map: '%s' not found on SD card", path);
        return false;
    }

    uint8_t header[kTileBlobHeaderSize];
    if (file_.read(header, sizeof(header)) != (int)sizeof(header) || readTileBlobU32LE(header) != kTileBlobMagic) {
        LOG_WARN("Map: '%s' missing/bad header", path);
        file_.close();
        return false;
    }
    const uint32_t count = readTileBlobU32LE(header + 4);
    if (count == 0) {
        LOG_INFO("Map: '%s' on SD card: empty", path);
        return true;
    }

    uint8_t rangeCountByte;
    if (file_.read(&rangeCountByte, 1) != 1) {
        LOG_WARN("Map: '%s' truncated (no zoom-range table)", path);
        file_.close();
        return false;
    }
    const int rangeCount = rangeCountByte;
    if (rangeCount <= 0 || rangeCount > kTileBlobMaxZoomRanges) {
        LOG_WARN("Map: '%s': implausible zoom-range count %d", path, rangeCount);
        file_.close();
        return false;
    }

    uint8_t rangeBuf[kTileBlobMaxZoomRanges * kTileBlobZoomRangeEntrySize];
    const size_t rangeBytes = (size_t)rangeCount * kTileBlobZoomRangeEntrySize;
    if (file_.read(rangeBuf, rangeBytes) != (int)rangeBytes) {
        LOG_WARN("Map: '%s' truncated zoom-range table", path);
        file_.close();
        return false;
    }
    TileBlobZoomRange ranges[kTileBlobMaxZoomRanges];
    for (int i = 0; i < rangeCount; i++)
        decodeTileBlobZoomRange(rangeBuf + i * kTileBlobZoomRangeEntrySize, ranges[i]);

    if (!validateTileBlobZoomRanges(ranges, rangeCount, count)) {
        LOG_WARN("Map: '%s': %u tiles doesn't match its zoom-range table", path, count);
        file_.close();
        return false;
    }

    const uint32_t indexTableStart = kTileBlobHeaderSize + 1 + (uint32_t)rangeBytes;

    // Sanity-check the last entry matches where the range table predicts it: the last range's
    // zoom, at its bottom-right-most raster position. Catches a truncated/malformed tile-entry
    // table before it causes a silently-wrong tile lookup rather than a clean "not supported"
    // failure.
    TileBlobEntry last{};
    const TileBlobZoomRange &lastRange = ranges[rangeCount - 1];
    if (!readEntryAt(file_, indexTableStart, count - 1, last) || last.zoom != lastRange.zoom ||
        last.tx != lastRange.xMin + lastRange.width - 1 || last.ty != lastRange.yMin + lastRange.height - 1) {
        LOG_WARN("Map: '%s': tile-entry table doesn't match its zoom-range table", path);
        file_.close();
        return false;
    }

    payloadStart_ = indexTableStart + count * kTileBlobEntrySize;
    indexTableStart_ = indexTableStart;

    for (int i = 0; i < rangeCount; i++)
        ranges_[i] = ranges[i];
    rangeCount_ = rangeCount;
    count_ = count;
    strncpy(path_, path, sizeof(path_) - 1);
    LOG_INFO("Map: '%s' on SD card: %u tiles across %d zoom range(s)", path, count, rangeCount_);
    return true;
}

int SDCardTileSource::indexOf(int zoom, int tx, int ty)
{
    return tileBlobIndexOf(ranges_, rangeCount_, count_, zoom, tx, ty);
}

int SDCardTileSource::tileZoomAt(int tileIndex)
{
    return tileBlobZoomAt(ranges_, rangeCount_, tileIndex);
}

int SDCardTileSource::tileTxAt(int tileIndex)
{
    return tileBlobTxAt(ranges_, rangeCount_, tileIndex);
}

int SDCardTileSource::tileTyAt(int tileIndex)
{
    return tileBlobTyAt(ranges_, rangeCount_, tileIndex);
}

bool SDCardTileSource::decodeTile(int tileIndex, uint8_t *outBuf)
{
    if (!file_)
        return false;

    TileBlobEntry e{};
    if (!readEntryAt(file_, indexTableStart_, (uint32_t)tileIndex, e))
        return false;

    if (e.kind != kTileKindLZ4)
        return decodeTilePayload(e.kind, nullptr, 0, outBuf);

    if (e.size == 0 || e.size > kTileBufferBytes)
        return false;

    uint8_t *compressed = tileCompressedScratchBuffer();
    file_.seekSet(payloadStart_ + e.offset);
    if (file_.read(compressed, e.size) != (int)e.size)
        return false;

    return decodeTilePayload(e.kind, compressed, e.size, outBuf);
}

#endif
