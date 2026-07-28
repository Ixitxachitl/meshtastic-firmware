#pragma once

/*

TileSource backed by a file on the external QSPI flash chip's existing FAT filesystem - the
same filesystem the Adafruit nRF52 bootloader exposes as a USB mass-storage drive for firmware
drag-and-drop updates.

This deliberately does NOT claim raw ownership of the QSPI chip or partition it: the bootloader's
FAT filesystem already occupies most of it (confirmed ~1.82MB of the ~2MB chip on the Wio Tracker
L1), so a separate filesystem alongside it isn't an option. Instead, the basemap is provisioned by
dragging MAP.BIN onto that same USB drive (bootloader mode), and this reads it back as an ordinary
file at runtime via SdFat, read-only.

See MapTileSourceFile.h for the much simpler ESP32/native-portduino equivalent, where the
filesystem isn't shared with a bootloader and has room to spare.

Reads the same blob format as MapTileSourceFile (see bin/generate_map_tiles.py):
    u32 magic 'MTLB', u32 tile_count
    tile_count * { u8 zoom, u16 tx, u16 ty, u8 kind, u32 offset, u16 size }
    followed by concatenated payload bytes (offset is relative to the start of this region).

*/

#if defined(HAS_QSPI_MAP_TILES)

#include "./MapTileRenderer.h"

#include <vector>

namespace NicheGraphics::MapTiles
{

class QSPIFatTileSource : public TileSource
{
  public:
    // Mounts the QSPI flash's FAT filesystem and reads `filename`'s tile index into RAM.
    // Returns false (leaving this source reporting zero tiles) if the flash chip, filesystem,
    // or file can't be found/read - the map screen then falls back to markers-only, same as if
    // no basemap had been baked in at all.
    bool begin(const char *filename = "MAP.BIN");

    int zoomCount() override;
    int zoomAt(int index) override;
    int tileCount() override { return (int)index_.size(); }
    int tileZoomAt(int tileIndex) override { return index_[tileIndex].zoom; }
    int tileTxAt(int tileIndex) override { return index_[tileIndex].tx; }
    int tileTyAt(int tileIndex) override { return index_[tileIndex].ty; }
    bool decodeTile(int tileIndex, uint8_t *outBuf) override;

  private:
    struct IndexEntry {
        uint8_t zoom;
        uint16_t tx;
        uint16_t ty;
        uint8_t kind;
        uint32_t offset;
        uint16_t size;
    };

    std::vector<IndexEntry> index_;
    std::vector<int> zoomLevels_;
    uint32_t payloadStart_ = 0;
    char filename_[32] = {};
    bool mounted_ = false;
};

} // namespace NicheGraphics::MapTiles

#endif
