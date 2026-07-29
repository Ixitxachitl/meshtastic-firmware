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

bool readEntryAt(FsFile &file, uint32_t entryIndex, TileBlobEntry &out)
{
    uint8_t buf[kTileBlobEntrySize];
    if (!file.seekSet(kTileBlobHeaderSize + (uint64_t)entryIndex * kTileBlobEntrySize))
        return false;
    if (file.read(buf, kTileBlobEntrySize) != (int)kTileBlobEntrySize)
        return false;
    decodeTileBlobEntry(buf, out);
    return true;
}
} // namespace

bool SDCardTileSource::begin(const char *path)
{
    zLo_ = 0;
    zHi_ = -1;
    count_ = 0;
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

    // No per-tile index is kept in RAM (see MapTileSourceSD.h) - bin/generate_map_tiles.py always
    // emits every (zoom, tx, ty) for a requested zoom range, densely, in ascending (zoom, ty, tx)
    // order, so the whole index's shape is fully determined by just the first and last entries.
    TileBlobEntry first{};
    if (!readEntryAt(file_, 0, first)) {
        LOG_WARN("Map: '%s' truncated index", path);
        file_.close();
        return false;
    }
    const int zLo = first.zoom;

    int zHi = 0;
    if (!solveTileBlobZoomRange(count, zLo, zHi)) {
        LOG_WARN("Map: '%s': %u tiles isn't a dense zoom range starting at z%d", path, count, zLo);
        file_.close();
        return false;
    }

    // Sanity-check the last entry matches where the dense layout predicts it: zoom zHi, at the
    // last (bottom-right-most) raster position. Catches a sparse/malformed blob before it causes
    // silently-wrong tile lookups rather than a clean "not supported" failure.
    TileBlobEntry last{};
    const int lastSide = 1 << zHi;
    if (!readEntryAt(file_, count - 1, last) || last.zoom != zHi || last.tx != lastSide - 1 || last.ty != lastSide - 1) {
        LOG_WARN("Map: '%s': index layout doesn't match the expected dense z%d-%d range", path, zLo, zHi);
        file_.close();
        return false;
    }

    payloadStart_ = kTileBlobHeaderSize + count * kTileBlobEntrySize;

    zLo_ = zLo;
    zHi_ = zHi;
    count_ = count;
    strncpy(path_, path, sizeof(path_) - 1);
    LOG_INFO("Map: '%s' on SD card: %u tiles, z%d-%d", path, count, zLo_, zHi_);
    return true;
}

int SDCardTileSource::indexOf(int zoom, int tx, int ty)
{
    return tileBlobIndexOf(zoom, tx, ty, zLo_, zHi_, count_);
}

int SDCardTileSource::tileZoomAt(int tileIndex)
{
    return tileBlobZoomAt(tileIndex, zLo_, zHi_);
}

int SDCardTileSource::tileTxAt(int tileIndex)
{
    return tileBlobTxAt(tileIndex, zLo_, zHi_);
}

int SDCardTileSource::tileTyAt(int tileIndex)
{
    return tileBlobTyAt(tileIndex, zLo_, zHi_);
}

bool SDCardTileSource::decodeTile(int tileIndex, uint8_t *outBuf)
{
    if (!file_)
        return false;

    TileBlobEntry e{};
    if (!readEntryAt(file_, (uint32_t)tileIndex, e))
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
