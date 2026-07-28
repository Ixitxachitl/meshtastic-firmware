#include "./MapTileSourceQSPI.h"

#if defined(HAS_QSPI_MAP_TILES)

#include "DebugConfiguration.h"

#include <Adafruit_SPIFlash.h>
#include <SdFat.h>

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

// The same physical flash chip (and FAT filesystem) the Adafruit nRF52 bootloader exposes as a
// USB mass-storage drive for firmware drag-and-drop updates - see MapTileSourceQSPI.h. Mounted
// read-only here so the app can read a file the user dragged onto that same drive.
Adafruit_FlashTransport_QSPI s_flashTransport;
Adafruit_SPIFlash s_flash(&s_flashTransport);
FatFileSystem s_fatfs;
bool s_flashMounted = false;
} // namespace

bool QSPIFatTileSource::begin(const char *filename)
{
    index_.clear();
    zoomLevels_.clear();
    payloadStart_ = 0;
    mounted_ = false;

    if (!s_flashMounted) {
        if (!s_flash.begin()) {
            LOG_WARN("Map: QSPI flash init failed");
            return false;
        }
        if (!s_fatfs.begin(&s_flash)) {
            LOG_WARN("Map: could not mount QSPI FAT filesystem (bootloader's UF2 drive)");
            return false;
        }
        s_flashMounted = true;
    }

    File32 file = s_fatfs.open(filename, FILE_READ);
    if (!file) {
        LOG_WARN("Map: '%s' not found on QSPI FAT filesystem - drag it onto the USB drive in bootloader mode", filename);
        return false;
    }

    uint8_t header[8];
    if (file.read(header, sizeof(header)) != (int)sizeof(header) || readU32LE(header) != kMagic) {
        LOG_WARN("Map: '%s' missing/bad header", filename);
        file.close();
        return false;
    }
    const uint32_t count = readU32LE(header + 4);

    index_.reserve(count);
    constexpr size_t kEntrySize = 12; // u8 zoom, u16 tx, u16 ty, u8 kind, u32 offset, u16 size
    uint8_t entryBuf[kEntrySize];
    for (uint32_t i = 0; i < count; i++) {
        if (file.read(entryBuf, kEntrySize) != (int)kEntrySize) {
            LOG_WARN("Map: '%s' truncated index (tile %u/%u)", filename, i, count);
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

    strncpy(filename_, filename, sizeof(filename_) - 1);
    mounted_ = true;
    LOG_INFO("Map: '%s' on QSPI FAT filesystem: %u tiles, %d zoom levels", filename, count, (int)zoomLevels_.size());
    return true;
}

int QSPIFatTileSource::zoomCount()
{
    return (int)zoomLevels_.size();
}

int QSPIFatTileSource::zoomAt(int index)
{
    return zoomLevels_[index];
}

bool QSPIFatTileSource::decodeTile(int tileIndex, uint8_t *outBuf)
{
    if (!mounted_)
        return false;

    const IndexEntry &e = index_[tileIndex];
    if (e.kind != kTileKindLZ4)
        return decodeTilePayload(e.kind, nullptr, 0, outBuf);

    if (e.size == 0 || e.size > kTileBufferBytes)
        return false;

    File32 file = s_fatfs.open(filename_, FILE_READ);
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
