#include "./MapTileSourceSD.h"

#if defined(HAS_SDCARD)

#include "DebugConfiguration.h"
#include "SPILock.h"
#include "configuration.h" // SDCARD_CS (variant.h)

#include "./MapTileBlobFormat.h"

#include <SPI.h>
#include <string.h>

using namespace NicheGraphics::MapTiles;

// Boards that share the SPI1/HSPI bus with another device (e.g. LoRa - see RadioInterface.cpp)
// must reuse FSCommon's SPI_HSPI instance rather than starting a second SPIClass(HSPI): a second
// spi_bus_initialize() on the same host returns ESP_ERR_INVALID_STATE and leaves that instance's
// handle unusable, hanging any transaction that follows (see t-watch-ultra, where LoRa and the SD
// card are wired to the same SCK/MOSI/MISO pins).
//
// Guard matches FSCommon.h's own extern for SPI_HSPI exactly: FSCommon only defines that instance
// when it's actually driving the card over hardware SPI, so a board that ever combined
// SDCARD_USE_SPI1 with SDCARD_USE_SOFT_SPI would otherwise fail to link here.
#if defined(SDCARD_USE_SPI1) && !defined(SDCARD_USE_SOFT_SPI)
extern SPIClass SPI_HSPI;
#define MapSDHandler SPI_HSPI
#else
#define MapSDHandler SPI
#endif

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
    // Everything below talks to the card. On the boards this actually matters for, that bus is
    // shared with the display and/or LoRa (see MapSDHandler above), and spiLock is what every other
    // user of it - TFTDisplay::display, RadioInterface, FSCommon - serialises on. Called once,
    // lazily, from the display task's first Map draw, which holds no lock of its own (OLEDDisplayUi
    // runs the frame callbacks before display->display(), not inside it), so taking it here can't
    // re-enter: concurrency::Lock is a plain binary semaphore with no recursion.
    concurrency::LockGuard g(spiLock);

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
        sdBegun_ = sd_.begin(SdSpiConfig(SDCARD_CS, SHARED_SPI, SD_SPI_FREQUENCY, &MapSDHandler));
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
    uint8_t *compressed = nullptr;

    {
        // Held across both transfers (index entry, then payload) so they land on the card as one
        // uninterrupted operation - see begin()'s comment for why the lock is needed and why taking
        // it here is re-entry-safe. Released before the LZ4 decompress below, which touches no
        // hardware: a payload is up to kTileBufferBytes, and decompressing it under the lock would
        // stall every other bus user for the whole of it to no purpose.
        concurrency::LockGuard g(spiLock);

        if (!readEntryAt(file_, indexTableStart_, (uint32_t)tileIndex, e))
            return false;

        if (e.kind == kTileKindLZ4) {
            if (e.size == 0 || e.size > kTileBufferBytes)
                return false;
            compressed = tileCompressedScratchBuffer();
            if (!file_.seekSet(payloadStart_ + e.offset))
                return false;
            if (file_.read(compressed, e.size) != (int)e.size)
                return false;
        }
    }

    // Solid-colour kinds carry no payload at all, and decodeTilePayload ignores compressed/size
    // for them - so the nullptr left above is what it already expects.
    return decodeTilePayload(e.kind, compressed, e.size, outBuf);
}

#endif
