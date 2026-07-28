#include "./MapTileSourceFile.h"

// FSCommon.h (via configuration.h) is what actually defines ARCH_ESP32 - it's not a compiler -D
// flag like ARCH_PORTDUINO is, so it must be included before the guard below can see it, or this
// whole file silently preprocesses away to nothing on ESP32 targets (link errors, no compile error).
#include "FSCommon.h"

#if defined(ARCH_PORTDUINO) || defined(ARCH_ESP32)

#include <string.h>

using namespace NicheGraphics::MapTiles;

namespace
{
constexpr uint32_t kMagic = 0x424C544D; // 'MTLB' little-endian, matches bin/generate_map_tiles.py

uint16_t readU16LE(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t readU32LE(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
} // namespace

bool FileTileSource::begin(const char *path)
{
    index_.clear();
    zoomLevels_.clear();
    payloadStart_ = 0;

    File file = FSCom.open(path, FILE_O_READ);
    if (!file) {
        LOG_WARN("Map tile file '%s' not found", path);
        return false;
    }

    uint8_t header[8];
    if (file.read(header, sizeof(header)) != sizeof(header) || readU32LE(header) != kMagic) {
        LOG_WARN("Map tile file '%s' missing/bad header", path);
        file.close();
        return false;
    }
    const uint32_t count = readU32LE(header + 4);

    index_.reserve(count);
    constexpr size_t kEntrySize = 12; // u8 zoom, u16 tx, u16 ty, u8 kind, u32 offset, u16 size
    uint8_t entryBuf[kEntrySize];
    for (uint32_t i = 0; i < count; i++) {
        if (file.read(entryBuf, kEntrySize) != kEntrySize) {
            LOG_WARN("Map tile file '%s' truncated index (tile %u/%u)", path, i, count);
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

    payloadStart_ = 8 + count * kEntrySize; // header + index, exactly what we just read
    file.close();

    strncpy(path_, path, sizeof(path_) - 1);
    LOG_INFO("Map tile file '%s': %u tiles, %d zoom levels", path, count, (int)zoomLevels_.size());
    return true;
}

int FileTileSource::zoomCount()
{
    return (int)zoomLevels_.size();
}

int FileTileSource::zoomAt(int index)
{
    return zoomLevels_[index];
}

bool FileTileSource::decodeTile(int tileIndex, uint8_t *outBuf)
{
    const IndexEntry &e = index_[tileIndex];
    if (e.kind != kTileKindLZ4)
        return decodeTilePayload(e.kind, nullptr, 0, outBuf);

    if (e.size == 0 || e.size > kTileBufferBytes)
        return false;

    File file = FSCom.open(path_, FILE_O_READ);
    if (!file)
        return false;

    uint8_t compressed[kTileBufferBytes];
    file.seek(payloadStart_ + e.offset);
    bool readOk = file.read(compressed, e.size) == e.size;
    file.close();
    if (!readOk)
        return false;

    return decodeTilePayload(e.kind, compressed, e.size, outBuf);
}

#endif
