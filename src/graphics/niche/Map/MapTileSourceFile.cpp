#include "./MapTileSourceFile.h"

// FSCommon.h (via configuration.h) is what actually defines ARCH_ESP32 - it's not a compiler -D
// flag like ARCH_PORTDUINO is, so it must be included before the guard below can see it, or this
// whole file silently preprocesses away to nothing on ESP32 targets (link errors, no compile error).
#include "FSCommon.h"

// BASEUI_HAS_MAP too - see MapTileSourceSD.h for why this source is BaseUI-only.
#if BASEUI_HAS_MAP && (defined(ARCH_PORTDUINO) || defined(ARCH_ESP32))

#include "./MapTileBlobFormat.h"

#include <new>
#include <string.h>
#include <utility>

using namespace NicheGraphics::MapTiles;

namespace
{
bool readEntryAt(File &file, uint32_t indexTableStart, uint32_t entryIndex, TileBlobEntry &out)
{
    uint8_t buf[kTileBlobEntrySize];
    if (!file.seek(indexTableStart + (uint64_t)entryIndex * kTileBlobEntrySize))
        return false;
    if (file.read(buf, kTileBlobEntrySize) != (int)kTileBlobEntrySize)
        return false;
    decodeTileBlobEntry(buf, out);
    return true;
}
} // namespace

bool FileTileSource::begin(const char *path)
{
    ranges_.reset();
    rangeCount_ = 0;
    zoomCount_ = 0;
    count_ = 0;
    indexTableStart_ = 0;
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

    uint8_t rangeCountByte;
    if (file_.read(&rangeCountByte, 1) != 1) {
        LOG_WARN("Map tile file '%s' truncated (no zoom-range table)", path);
        file_.close();
        return false;
    }
    const int rangeCount = rangeCountByte;
    if (rangeCount <= 0 || rangeCount > kTileBlobMaxZoomRanges) {
        LOG_WARN("Map tile file '%s': implausible zoom-range count %d", path, rangeCount);
        file_.close();
        return false;
    }

    // Decoded a record at a time straight into the (exactly-sized) destination: at 255 ranges the
    // whole table is 2.3KB, far too much to stage through a stack buffer on the tasks this runs on.
    const size_t rangeBytes = (size_t)rangeCount * kTileBlobZoomRangeEntrySize;
    std::unique_ptr<TileBlobZoomRange[]> ranges(new (std::nothrow) TileBlobZoomRange[rangeCount]);
    if (!ranges) {
        LOG_WARN("Map tile file '%s': no memory for %d zoom range(s)", path, rangeCount);
        file_.close();
        return false;
    }
    for (int i = 0; i < rangeCount; i++) {
        uint8_t rangeBuf[kTileBlobZoomRangeEntrySize];
        if (file_.read(rangeBuf, sizeof(rangeBuf)) != (int)sizeof(rangeBuf)) {
            LOG_WARN("Map tile file '%s' truncated zoom-range table", path);
            file_.close();
            return false;
        }
        decodeTileBlobZoomRange(rangeBuf, ranges[i]);
    }

    if (!validateTileBlobZoomRanges(ranges.get(), rangeCount, count)) {
        LOG_WARN("Map tile file '%s': %u tiles doesn't match its zoom-range table", path, count);
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
        LOG_WARN("Map tile file '%s': tile-entry table doesn't match its zoom-range table", path);
        file_.close();
        return false;
    }

    // `count` comes straight out of the file, so compute the table extent in 64-bit and check it
    // against the file's real size: a hostile count would otherwise wrap the uint32 payload base
    // (count > ~357M is enough), and a truncated file would leave payloadStart_ past the end. The
    // uint32 ceiling is explicit because every offset built on payloadStart_ below is 32-bit.
    const uint64_t fileSize = (uint64_t)file_.size();
    const uint64_t payloadStart = (uint64_t)indexTableStart + (uint64_t)count * kTileBlobEntrySize;
    if (payloadStart > fileSize || payloadStart > UINT32_MAX) {
        LOG_WARN("Map tile file '%s': tile-entry table doesn't fit the file", path);
        file_.close();
        return false;
    }

    payloadStart_ = (uint32_t)payloadStart;
    payloadBytes_ = (uint32_t)(fileSize - payloadStart);
    indexTableStart_ = indexTableStart;

    zoomCount_ = tileBlobDistinctZooms(ranges.get(), rangeCount, zooms_);
    ranges_ = std::move(ranges);
    rangeCount_ = rangeCount;
    count_ = count;
    strncpy(path_, path, sizeof(path_) - 1);
    LOG_INFO("Map tile file '%s': %u tiles across %d zoom range(s), %d zoom level(s)", path, count, rangeCount_, zoomCount_);
    return true;
}

int FileTileSource::indexOf(int zoom, int tx, int ty)
{
    return tileBlobIndexOf(ranges_.get(), rangeCount_, count_, zoom, tx, ty);
}

int FileTileSource::tileZoomAt(int tileIndex)
{
    return tileBlobZoomAt(ranges_.get(), rangeCount_, tileIndex);
}

int FileTileSource::tileTxAt(int tileIndex)
{
    return tileBlobTxAt(ranges_.get(), rangeCount_, tileIndex);
}

int FileTileSource::tileTyAt(int tileIndex)
{
    return tileBlobTyAt(ranges_.get(), rangeCount_, tileIndex);
}

bool FileTileSource::decodeTile(int tileIndex, uint8_t *outBuf)
{
    if (!file_)
        return false;

    // Every current caller passes either a validated indexOf() result or an index from iterating
    // 0..tileCount()-1, so this shouldn't trigger - but an out-of-range index would otherwise seek
    // past the index table and decode 12 arbitrary payload bytes as if they were an entry.
    if (tileIndex < 0 || (uint32_t)tileIndex >= count_)
        return false;

    TileBlobEntry e{};
    if (!readEntryAt(file_, indexTableStart_, (uint32_t)tileIndex, e))
        return false;

    if (e.kind != kTileKindLZ4)
        return decodeTilePayload(e.kind, nullptr, 0, outBuf);

    if (e.size == 0 || e.size > kTileBufferBytes)
        return false;

    // The entry's payload has to lie inside the payload region - offset and size are both file
    // data, so neither can be trusted to point anywhere sensible on its own.
    if ((uint64_t)e.offset + e.size > payloadBytes_)
        return false;

    uint8_t *compressed = tileCompressedScratchBuffer();
    file_.seek(payloadStart_ + e.offset);
    if (file_.read(compressed, e.size) != e.size)
        return false;

    return decodeTilePayload(e.kind, compressed, e.size, outBuf);
}

#endif
