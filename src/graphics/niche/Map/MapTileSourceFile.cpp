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
constexpr size_t kEntrySize = 12;        // u8 zoom, u16 tx, u16 ty, u8 kind, u32 offset, u16 size

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

bool readEntryAt(File &file, uint32_t entryIndex, RawEntry &out)
{
    uint8_t buf[kEntrySize];
    if (!file.seek(8 + (uint64_t)entryIndex * kEntrySize))
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

bool FileTileSource::begin(const char *path)
{
    zLo_ = 0;
    zHi_ = -1;
    count_ = 0;
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
    if (count == 0) {
        file.close();
        LOG_INFO("Map tile file '%s': empty", path);
        return true;
    }

    // No per-tile index is kept in RAM (see MapTileSourceFile.h) - bin/generate_map_tiles.py always
    // emits every (zoom, tx, ty) for a requested zoom range, densely, in ascending (zoom, ty, tx)
    // order, so the whole index's shape is fully determined by just the first and last entries.
    RawEntry first{};
    if (!readEntryAt(file, 0, first)) {
        LOG_WARN("Map tile file '%s' truncated index", path);
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
        LOG_WARN("Map tile file '%s': %u tiles isn't a dense zoom range starting at z%d", path, count, zLo);
        file.close();
        return false;
    }

    // Sanity-check the last entry matches where the dense layout predicts it: zoom zHi, at the
    // last (bottom-right-most) raster position. Catches a sparse/malformed blob before it causes
    // silently-wrong tile lookups rather than a clean "not supported" failure.
    RawEntry last{};
    const int lastSide = 1 << zHi;
    if (!readEntryAt(file, count - 1, last) || last.zoom != zHi || last.tx != lastSide - 1 || last.ty != lastSide - 1) {
        LOG_WARN("Map tile file '%s': index layout doesn't match the expected dense z%d-%d range", path, zLo, zHi);
        file.close();
        return false;
    }

    payloadStart_ = 8 + count * kEntrySize;
    file.close();

    zLo_ = zLo;
    zHi_ = zHi;
    count_ = count;
    strncpy(path_, path, sizeof(path_) - 1);
    LOG_INFO("Map tile file '%s': %u tiles, z%d-%d", path, count, zLo_, zHi_);
    return true;
}

uint32_t FileTileSource::baseIndexForZoom(int zoom) const
{
    return (uint32_t)(((1ULL << (2 * zoom)) - (1ULL << (2 * zLo_))) / 3);
}

int FileTileSource::indexOf(int zoom, int tx, int ty)
{
    if (zoom < zLo_ || zoom > zHi_)
        return -1;
    const int side = 1 << zoom;
    if (tx < 0 || tx >= side || ty < 0 || ty >= side)
        return -1;
    const uint32_t i = baseIndexForZoom(zoom) + (uint32_t)ty * side + tx;
    return i < count_ ? (int)i : -1;
}

int FileTileSource::tileZoomAt(int tileIndex)
{
    for (int z = zLo_; z <= zHi_; z++) {
        if ((uint32_t)tileIndex < baseIndexForZoom(z + 1))
            return z;
    }
    return zHi_;
}

int FileTileSource::tileTxAt(int tileIndex)
{
    const int zoom = tileZoomAt(tileIndex);
    const uint32_t local = (uint32_t)tileIndex - baseIndexForZoom(zoom);
    return (int)(local % (1u << zoom));
}

int FileTileSource::tileTyAt(int tileIndex)
{
    const int zoom = tileZoomAt(tileIndex);
    const uint32_t local = (uint32_t)tileIndex - baseIndexForZoom(zoom);
    return (int)(local / (1u << zoom));
}

bool FileTileSource::decodeTile(int tileIndex, uint8_t *outBuf)
{
    File file = FSCom.open(path_, FILE_O_READ);
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
    file.seek(payloadStart_ + e.offset);
    bool readOk = file.read(compressed, e.size) == e.size;
    file.close();
    if (!readOk)
        return false;

    return decodeTilePayload(e.kind, compressed, e.size, outBuf);
}

#endif
