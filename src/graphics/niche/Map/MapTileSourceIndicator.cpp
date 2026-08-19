#include "./MapTileSourceIndicator.h"

#if defined(SENSECAP_INDICATOR) && BASEUI_HAS_MAP

#include "DebugConfiguration.h"
#include "mesh/IndicatorSerial.h"

#include "./MapTileBlobFormat.h"

#include <new>
#include <string.h>
#include <utility>

extern SensecapIndicator *sensecapIndicator;

using namespace NicheGraphics::MapTiles;

namespace
{
// Per-request payload cap of the interdevice link (FileTransfer.filedata max_size).
constexpr uint32_t kLinkChunkBytes = sizeof(((meshtastic_FileTransfer *)nullptr)->filedata.bytes);
} // namespace

bool IndicatorTileSource::readAt(uint32_t offset, uint8_t *buf, uint32_t len, uint64_t *fileSize)
{
    if (!sensecapIndicator)
        return false;
    // One statically-sized response per request; the bridge copies the chunk into it.
    static meshtastic_FileTransfer result;
    uint32_t done = 0;
    while (done < len) {
        const uint32_t want = (len - done) < kLinkChunkBytes ? (len - done) : kLinkChunkBytes;
        memset(&result, 0, sizeof(result));
        if (!sensecapIndicator->file_read(path_, offset + done, want, &result))
            return false;
        if (result.status != meshtastic_FileStatus_FILE_OK)
            return false;
        uint32_t n = result.filedata.size;
        if (n == 0 || n > want)
            return false; // short read past EOF, or a chunk larger than asked for - both mean a bad offset/size
        memcpy(buf + done, result.filedata.bytes, n);
        done += n;
        if (fileSize)
            *fileSize = result.file_size;
    }
    return true;
}

bool IndicatorTileSource::begin(const char *path)
{
    ranges_.reset();
    rangeCount_ = 0;
    zoomCount_ = 0;
    count_ = 0;
    indexTableStart_ = 0;
    payloadStart_ = 0;
    opened_ = false;
    strncpy(path_, path, sizeof(path_) - 1);
    path_[sizeof(path_) - 1] = 0;

    if (!sensecapIndicator) {
        LOG_WARN("Map: co-processor link not up, no SD card access");
        return false;
    }

    // Header read doubles as the existence check: FILE_NOT_FOUND / FILE_NO_CARD both fail it.
    uint8_t header[kTileBlobHeaderSize];
    uint64_t fileSize = 0;
    if (!readAt(0, header, sizeof(header), &fileSize) || readTileBlobU32LE(header) != kTileBlobMagic) {
        LOG_WARN("Map: '%s' not readable on the co-processor's SD card, or bad header", path);
        return false;
    }
    const uint32_t count = readTileBlobU32LE(header + 4);
    if (count == 0) {
        LOG_INFO("Map: '%s' on SD card: empty", path);
        opened_ = true;
        return true;
    }

    uint8_t rangeCountByte;
    if (!readAt(kTileBlobHeaderSize, &rangeCountByte, 1)) {
        LOG_WARN("Map: '%s' truncated (no zoom-range table)", path);
        return false;
    }
    const int rangeCount = rangeCountByte;
    if (rangeCount <= 0 || rangeCount > kTileBlobMaxZoomRanges) {
        LOG_WARN("Map: '%s': implausible zoom-range count %d", path, rangeCount);
        return false;
    }

    // The whole range table in one link round trip (up to 2.3KB, under the chunk cap), decoded
    // record by record into an exactly-sized table.
    const size_t rangeBytes = (size_t)rangeCount * kTileBlobZoomRangeEntrySize;
    std::unique_ptr<TileBlobZoomRange[]> ranges(new (std::nothrow) TileBlobZoomRange[rangeCount]);
    std::unique_ptr<uint8_t[]> rangeRaw(new (std::nothrow) uint8_t[rangeBytes]);
    if (!ranges || !rangeRaw) {
        LOG_WARN("Map: '%s': no memory for %d zoom range(s)", path, rangeCount);
        return false;
    }
    if (!readAt(kTileBlobHeaderSize + 1, rangeRaw.get(), (uint32_t)rangeBytes)) {
        LOG_WARN("Map: '%s' truncated zoom-range table", path);
        return false;
    }
    for (int i = 0; i < rangeCount; i++)
        decodeTileBlobZoomRange(rangeRaw.get() + (size_t)i * kTileBlobZoomRangeEntrySize, ranges[i]);

    if (!validateTileBlobZoomRanges(ranges.get(), rangeCount, count)) {
        LOG_WARN("Map: '%s': %u tiles doesn't match its zoom-range table", path, count);
        return false;
    }

    const uint32_t indexTableStart = kTileBlobHeaderSize + 1 + (uint32_t)rangeBytes;

    // Sanity-check the last entry against where the range table predicts it - see SDCardTileSource.
    TileBlobEntry last{};
    {
        uint8_t buf[kTileBlobEntrySize];
        const TileBlobZoomRange &lastRange = ranges[rangeCount - 1];
        if (!readAt(indexTableStart + (count - 1) * kTileBlobEntrySize, buf, kTileBlobEntrySize)) {
            LOG_WARN("Map: '%s': can't read tile-entry table", path);
            return false;
        }
        decodeTileBlobEntry(buf, last);
        if (last.zoom != lastRange.zoom || last.tx != lastRange.xMin + lastRange.width - 1 ||
            last.ty != lastRange.yMin + lastRange.height - 1) {
            LOG_WARN("Map: '%s': tile-entry table doesn't match its zoom-range table", path);
            return false;
        }
    }

    // `count` is bytes off a removable card: compute the table extent in 64-bit and check it
    // against the co-processor's reported file size before deriving 32-bit offsets from it.
    const uint64_t payloadStart = (uint64_t)indexTableStart + (uint64_t)count * kTileBlobEntrySize;
    if (payloadStart > fileSize || payloadStart > UINT32_MAX) {
        LOG_WARN("Map: '%s': tile-entry table doesn't fit the file", path);
        return false;
    }

    payloadStart_ = (uint32_t)payloadStart;
    payloadBytes_ = (uint32_t)(fileSize - payloadStart);
    indexTableStart_ = indexTableStart;

    zoomCount_ = tileBlobDistinctZooms(ranges.get(), rangeCount, zooms_);
    ranges_ = std::move(ranges);
    rangeCount_ = rangeCount;
    count_ = count;
    opened_ = true;
    LOG_INFO("Map: '%s' on co-processor SD card: %u tiles across %d zoom range(s), %d zoom level(s)", path, count, rangeCount_,
             zoomCount_);
    return true;
}

int IndicatorTileSource::indexOf(int zoom, int tx, int ty)
{
    return tileBlobIndexOf(ranges_.get(), rangeCount_, count_, zoom, tx, ty);
}

int IndicatorTileSource::tileZoomAt(int tileIndex)
{
    return tileBlobZoomAt(ranges_.get(), rangeCount_, tileIndex);
}

int IndicatorTileSource::tileTxAt(int tileIndex)
{
    return tileBlobTxAt(ranges_.get(), rangeCount_, tileIndex);
}

int IndicatorTileSource::tileTyAt(int tileIndex)
{
    return tileBlobTyAt(ranges_.get(), rangeCount_, tileIndex);
}

bool IndicatorTileSource::decodeTile(int tileIndex, uint8_t *outBuf)
{
    if (!opened_)
        return false;
    if (tileIndex < 0 || (uint32_t)tileIndex >= count_)
        return false;

    TileBlobEntry e{};
    {
        uint8_t buf[kTileBlobEntrySize];
        if (!readAt(indexTableStart_ + (uint32_t)tileIndex * kTileBlobEntrySize, buf, kTileBlobEntrySize))
            return false;
        decodeTileBlobEntry(buf, e);
    }

    uint8_t *compressed = nullptr;
    if (e.kind == kTileKindLZ4) {
        if (e.size == 0 || e.size > kTileBufferBytes)
            return false;
        if ((uint64_t)e.offset + e.size > payloadBytes_)
            return false;
        compressed = tileCompressedScratchBuffer();
        if (!compressed)
            return false;
        if (!readAt(payloadStart_ + e.offset, compressed, e.size))
            return false;
    }

    // Solid-colour kinds carry no payload; decodeTilePayload ignores compressed/size for them.
    return decodeTilePayload(e.kind, compressed, e.size, outBuf);
}

#endif
