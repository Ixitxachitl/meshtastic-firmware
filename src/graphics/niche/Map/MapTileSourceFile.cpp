#include "./MapTileSourceFile.h"

// FSCommon.h (via configuration.h) is what actually defines ARCH_ESP32 - it's not a compiler -D
// flag like ARCH_PORTDUINO is, so it must be included before the guard below can see it, or this
// whole file silently preprocesses away to nothing on ESP32 targets (link errors, no compile error).
#include "FSCommon.h"

#if defined(ARCH_PORTDUINO) || defined(ARCH_ESP32)

#include "./MapTileBlobFormat.h"

#include <string.h>

using namespace NicheGraphics::MapTiles;

namespace
{
bool readEntryAt(File &file, uint32_t entryIndex, TileBlobEntry &out)
{
    uint8_t buf[kTileBlobEntrySize];
    if (!file.seek(kTileBlobHeaderSize + (uint64_t)entryIndex * kTileBlobEntrySize))
        return false;
    if (file.read(buf, kTileBlobEntrySize) != (int)kTileBlobEntrySize)
        return false;
    decodeTileBlobEntry(buf, out);
    return true;
}
} // namespace

bool FileTileSource::begin(const char *path)
{
    zLo_ = 0;
    zHi_ = -1;
    count_ = 0;
    payloadStart_ = 0;
    if (file_)
        file_.close();

    // Opened once and kept open for decodeTile() to reuse (see file_'s doc comment) - only closed
    // again below on a failure path, where nothing will call decodeTile() anyway.
    file_ = FSCom.open(path, FILE_O_READ);
    if (!file_) {
        LOG_WARN("Map tile file '%s' not found", path);
        return false;
    }

    uint8_t header[kTileBlobHeaderSize];
    if (file_.read(header, sizeof(header)) != sizeof(header) || readTileBlobU32LE(header) != kTileBlobMagic) {
        LOG_WARN("Map tile file '%s' missing/bad header", path);
        file_.close();
        return false;
    }
    const uint32_t count = readTileBlobU32LE(header + 4);
    if (count == 0) {
        LOG_INFO("Map tile file '%s': empty", path);
        return true;
    }

    // No per-tile index is kept in RAM (see MapTileSourceFile.h) - bin/generate_map_tiles.py always
    // emits every (zoom, tx, ty) for a requested zoom range, densely, in ascending (zoom, ty, tx)
    // order, so the whole index's shape is fully determined by just the first and last entries.
    TileBlobEntry first{};
    if (!readEntryAt(file_, 0, first)) {
        LOG_WARN("Map tile file '%s' truncated index", path);
        file_.close();
        return false;
    }
    const int zLo = first.zoom;

    int zHi = 0;
    if (!solveTileBlobZoomRange(count, zLo, zHi)) {
        LOG_WARN("Map tile file '%s': %u tiles isn't a dense zoom range starting at z%d", path, count, zLo);
        file_.close();
        return false;
    }

    // Sanity-check the last entry matches where the dense layout predicts it: zoom zHi, at the
    // last (bottom-right-most) raster position. Catches a sparse/malformed blob before it causes
    // silently-wrong tile lookups rather than a clean "not supported" failure.
    TileBlobEntry last{};
    const int lastSide = 1 << zHi;
    if (!readEntryAt(file_, count - 1, last) || last.zoom != zHi || last.tx != lastSide - 1 || last.ty != lastSide - 1) {
        LOG_WARN("Map tile file '%s': index layout doesn't match the expected dense z%d-%d range", path, zLo, zHi);
        file_.close();
        return false;
    }

    payloadStart_ = kTileBlobHeaderSize + count * kTileBlobEntrySize;

    zLo_ = zLo;
    zHi_ = zHi;
    count_ = count;
    strncpy(path_, path, sizeof(path_) - 1);
    LOG_INFO("Map tile file '%s': %u tiles, z%d-%d", path, count, zLo_, zHi_);
    return true;
}

int FileTileSource::indexOf(int zoom, int tx, int ty)
{
    return tileBlobIndexOf(zoom, tx, ty, zLo_, zHi_, count_);
}

int FileTileSource::tileZoomAt(int tileIndex)
{
    return tileBlobZoomAt(tileIndex, zLo_, zHi_);
}

int FileTileSource::tileTxAt(int tileIndex)
{
    return tileBlobTxAt(tileIndex, zLo_, zHi_);
}

int FileTileSource::tileTyAt(int tileIndex)
{
    return tileBlobTyAt(tileIndex, zLo_, zHi_);
}

bool FileTileSource::decodeTile(int tileIndex, uint8_t *outBuf)
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
    file_.seek(payloadStart_ + e.offset);
    if (file_.read(compressed, e.size) != e.size)
        return false;

    return decodeTilePayload(e.kind, compressed, e.size, outBuf);
}

#endif
