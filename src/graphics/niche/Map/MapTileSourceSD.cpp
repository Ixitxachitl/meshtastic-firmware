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
constexpr size_t kEntrySize = 12;        // u8 zoom, u16 tx, u16 ty, u8 kind, u32 offset, u16 size

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

struct RawEntry {
    uint8_t zoom;
    uint16_t tx;
    uint16_t ty;
    uint8_t kind;
    uint32_t offset;
    uint16_t size;
};

bool readEntryAt(FsFile &file, uint32_t entryIndex, RawEntry &out)
{
    uint8_t buf[kEntrySize];
    if (!file.seekSet(8 + (uint64_t)entryIndex * kEntrySize))
        return false;
    if (file.read(buf, kEntrySize) != (int)kEntrySize)
        return false;
    out.zoom = buf[0];
    out.tx = readU16LE(buf + 1);
    out.ty = readU16LE(buf + 3);
    out.kind = buf[5];
    out.offset = readU32LE(buf + 6);
    out.size = readU16LE(buf + 10);
    return true;
}
} // namespace

bool SDCardTileSource::begin(const char *path)
{
    zLo_ = 0;
    zHi_ = -1;
    count_ = 0;
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
    if (count == 0) {
        file.close();
        LOG_INFO("Map: '%s' on SD card: empty", path);
        return true;
    }

    // No per-tile index is kept in RAM (see MapTileSourceSD.h) - bin/generate_map_tiles.py always
    // emits every (zoom, tx, ty) for a requested zoom range, densely, in ascending (zoom, ty, tx)
    // order, so the whole index's shape is fully determined by just the first and last entries.
    RawEntry first{};
    if (!readEntryAt(file, 0, first)) {
        LOG_WARN("Map: '%s' truncated index", path);
        file.close();
        return false;
    }
    const int zLo = first.zoom;

    // Solve for the highest zoom present from the dense-range tile-count identity:
    // count == sum_{z=zLo}^{zHi} 4^z == (4^(zHi+1) - 4^zLo) / 3.
    int zHi = zLo;
    uint64_t cumulative = 1ULL << (2 * zLo);
    while (cumulative < count) {
        zHi++;
        cumulative += 1ULL << (2 * zHi);
    }
    if (cumulative != count) {
        LOG_WARN("Map: '%s': %u tiles isn't a dense zoom range starting at z%d", path, count, zLo);
        file.close();
        return false;
    }

    // Sanity-check the last entry matches where the dense layout predicts it: zoom zHi, at the
    // last (bottom-right-most) raster position. Catches a sparse/malformed blob before it causes
    // silently-wrong tile lookups rather than a clean "not supported" failure.
    RawEntry last{};
    const int lastSide = 1 << zHi;
    if (!readEntryAt(file, count - 1, last) || last.zoom != zHi || last.tx != lastSide - 1 || last.ty != lastSide - 1) {
        LOG_WARN("Map: '%s': index layout doesn't match the expected dense z%d-%d range", path, zLo, zHi);
        file.close();
        return false;
    }

    payloadStart_ = 8 + count * kEntrySize;
    file.close();

    zLo_ = zLo;
    zHi_ = zHi;
    count_ = count;
    strncpy(path_, path, sizeof(path_) - 1);
    LOG_INFO("Map: '%s' on SD card: %u tiles, z%d-%d", path, count, zLo_, zHi_);
    return true;
}

uint32_t SDCardTileSource::baseIndexForZoom(int zoom) const
{
    return (uint32_t)(((1ULL << (2 * zoom)) - (1ULL << (2 * zLo_))) / 3);
}

int SDCardTileSource::indexOf(int zoom, int tx, int ty)
{
    if (zoom < zLo_ || zoom > zHi_)
        return -1;
    const int side = 1 << zoom;
    if (tx < 0 || tx >= side || ty < 0 || ty >= side)
        return -1;
    const uint32_t i = baseIndexForZoom(zoom) + (uint32_t)ty * side + tx;
    return i < count_ ? (int)i : -1;
}

int SDCardTileSource::tileZoomAt(int tileIndex)
{
    for (int z = zLo_; z <= zHi_; z++) {
        if ((uint32_t)tileIndex < baseIndexForZoom(z + 1))
            return z;
    }
    return zHi_;
}

int SDCardTileSource::tileTxAt(int tileIndex)
{
    const int zoom = tileZoomAt(tileIndex);
    const uint32_t local = (uint32_t)tileIndex - baseIndexForZoom(zoom);
    return (int)(local % (1u << zoom));
}

int SDCardTileSource::tileTyAt(int tileIndex)
{
    const int zoom = tileZoomAt(tileIndex);
    const uint32_t local = (uint32_t)tileIndex - baseIndexForZoom(zoom);
    return (int)(local / (1u << zoom));
}

bool SDCardTileSource::decodeTile(int tileIndex, uint8_t *outBuf)
{
    FsFile file = sd_.open(path_, O_RDONLY);
    if (!file)
        return false;

    RawEntry e{};
    if (!readEntryAt(file, (uint32_t)tileIndex, e)) {
        file.close();
        return false;
    }

    if (e.kind != kTileKindLZ4) {
        file.close();
        return decodeTilePayload(e.kind, nullptr, 0, outBuf);
    }

    if (e.size == 0 || e.size > kTileBufferBytes) {
        file.close();
        return false;
    }

    uint8_t compressed[kTileBufferBytes];
    file.seekSet(payloadStart_ + e.offset);
    bool readOk = file.read(compressed, e.size) == (int)e.size;
    file.close();
    if (!readOk)
        return false;

    return decodeTilePayload(e.kind, compressed, e.size, outBuf);
}

#endif
