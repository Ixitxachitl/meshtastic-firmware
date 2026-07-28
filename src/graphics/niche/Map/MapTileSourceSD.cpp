#include "./MapTileSourceSD.h"

#if defined(HAS_SDCARD)

#include "DebugConfiguration.h"
#include "configuration.h" // SDCARD_CS (variant.h)

#include <SPI.h>
#include <string.h>

using namespace NicheGraphics::MapTiles;

namespace
{
constexpr uint32_t kMagic = 0x424C544D; // 'MTLB' little-endian, matches bin/generate_map_tiles.py

#ifndef SD_SPI_FREQUENCY
#define SD_SPI_FREQUENCY 4000000U
#endif

uint16_t readU16LE(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t readU32LE(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
} // namespace

bool SDCardTileSource::begin(const char *path)
{
    index_.clear();
    zoomLevels_.clear();
    payloadStart_ = 0;

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

    FsFile file = sd_.open(path, O_RDONLY);
    if (!file) {
        LOG_WARN("Map: '%s' not found on SD card", path);
        return false;
    }

    uint8_t header[8];
    if (file.read(header, sizeof(header)) != (int)sizeof(header) || readU32LE(header) != kMagic) {
        LOG_WARN("Map: '%s' missing/bad header", path);
        file.close();
        return false;
    }
    const uint32_t count = readU32LE(header + 4);

    index_.reserve(count);
    constexpr size_t kEntrySize = 12; // u8 zoom, u16 tx, u16 ty, u8 kind, u32 offset, u16 size
    uint8_t entryBuf[kEntrySize];
    for (uint32_t i = 0; i < count; i++) {
        if (file.read(entryBuf, kEntrySize) != (int)kEntrySize) {
            LOG_WARN("Map: '%s' truncated index (tile %u/%u)", path, i, count);
            index_.clear();
            file.close();
            return false;
        }
        IndexEntry e;
        e.zoom = entryBuf[0];
        e.tx = readU16LE(entryBuf + 1);
        e.ty = readU16LE(entryBuf + 3);
        e.kind = entryBuf[5];
        e.offset = readU32LE(entryBuf + 6);
        e.size = readU16LE(entryBuf + 10);
        index_.push_back(e);

        bool haveZoom = false;
        for (int z : zoomLevels_) {
            if (z == e.zoom) {
                haveZoom = true;
                break;
            }
        }
        if (!haveZoom)
            zoomLevels_.push_back(e.zoom);
    }

    payloadStart_ = 8 + count * kEntrySize;
    file.close();

    strncpy(path_, path, sizeof(path_) - 1);
    LOG_INFO("Map: '%s' on SD card: %u tiles, %d zoom levels", path, count, (int)zoomLevels_.size());
    return true;
}

int SDCardTileSource::zoomCount()
{
    return (int)zoomLevels_.size();
}

int SDCardTileSource::zoomAt(int index)
{
    return zoomLevels_[index];
}

bool SDCardTileSource::decodeTile(int tileIndex, uint8_t *outBuf)
{
    const IndexEntry &e = index_[tileIndex];
    if (e.kind != kTileKindLZ4)
        return decodeTilePayload(e.kind, nullptr, 0, outBuf);

    if (e.size == 0 || e.size > kTileBufferBytes)
        return false;

    FsFile file = sd_.open(path_, O_RDONLY);
    if (!file)
        return false;

    uint8_t compressed[kTileBufferBytes];
    file.seekSet(payloadStart_ + e.offset);
    bool readOk = file.read(compressed, e.size) == (int)e.size;
    file.close();
    if (!readOk)
        return false;

    return decodeTilePayload(e.kind, compressed, e.size, outBuf);
}

#endif
