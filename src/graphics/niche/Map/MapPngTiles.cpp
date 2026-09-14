#include "./MapPngTiles.h"

#if BASEUI_MAP_PNG_TILES

#include "./MapTileSourceSD.h"
#include "DebugConfiguration.h"
#include "SPILock.h"

#include <PNGdec.h>
#include <esp_heap_caps.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace NicheGraphics::MapTiles::Png
{
namespace
{

constexpr size_t kNameSize = 20;             // uiconfig.map_data.style, so every listed style can be remembered
constexpr int kTileSlots = 9;                // decoded tiles kept, 128KB of PSRAM each
constexpr int kMissSlots = 64;               // tiles known to be absent, so they aren't looked up every frame
constexpr int kMaxOverzoom = 4;              // zoom levels up to look for a stand-in for a missing tile
constexpr size_t kMaxFileBytes = 512 * 1024; // larger files are not map tiles

char styles[kMaxStyles][kNameSize];
int numStyles = 0;
int active = -1;
uint32_t gen = 1; // starts above 0, so zeroed miss slots never match

struct TileKey {
    uint32_t gen;
    int32_t z, x, y;
    bool operator==(const TileKey &o) const { return gen == o.gen && z == o.z && x == o.x && y == o.y; }
};

struct TileSlot {
    TileKey key;
    uint32_t lastUse;
    uint16_t *pixels;
    bool valid;
};

TileSlot tiles[kTileSlots];
TileKey misses[kMissSlots];
int nextMiss = 0;
uint32_t useClock = 0;

PNG *decoder = nullptr;
uint8_t *fileBuf = nullptr;
size_t fileBufSize = 0;

void *allocLarge(size_t bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(bytes);
}

int32_t floorToTile(int32_t v)
{
    return v >= 0 ? v - v % kTileSize : -((-v + kTileSize - 1) / kTileSize) * kTileSize;
}

int drawRow(PNGDRAW *draw)
{
    if (draw->y < 0 || draw->y >= kTileSize || draw->iWidth != kTileSize)
        return 0;
    auto *out = static_cast<uint16_t *>(draw->pUser);
    decoder->getLineAsRGB565(draw, out + draw->y * kTileSize, PNG_RGB565_LITTLE_ENDIAN, 0);
    return 1;
}

bool loadTile(const TileKey &key, uint16_t *out)
{
    char path[64];
    if (styles[active][0])
        snprintf(path, sizeof(path), "/maps/%s/%d/%d/%d.png", styles[active], (int)key.z, (int)key.x, (int)key.y);
    else
        snprintf(path, sizeof(path), "/map/%d/%d/%d.png", (int)key.z, (int)key.x, (int)key.y);

    size_t size = 0;
    {
        concurrency::LockGuard g(spiLock);
        SdFs *sd = mapSdCard();
        if (!sd)
            return false;
        FsFile file = sd->open(path, O_RDONLY);
        if (!file)
            return false;
        const uint64_t fileSize = file.fileSize();
        if (fileSize == 0 || fileSize > kMaxFileBytes) {
            file.close();
            return false;
        }
        size = (size_t)fileSize;
        if (size > fileBufSize) {
            free(fileBuf);
            fileBuf = static_cast<uint8_t *>(allocLarge(size));
            fileBufSize = fileBuf ? size : 0;
        }
        const bool ok = fileBuf && file.read(fileBuf, size) == (int)size;
        file.close();
        if (!ok)
            return false;
    }

    if (!decoder) {
        void *mem = allocLarge(sizeof(PNG));
        if (!mem)
            return false;
        decoder = new (mem) PNG();
    }
    if (decoder->openRAM(fileBuf, (int)size, drawRow) != PNG_SUCCESS) {
        LOG_WARN("Map: can't open %s (%d)", path, decoder->getLastError());
        return false;
    }
    if (decoder->getWidth() != kTileSize || decoder->getHeight() != kTileSize) {
        LOG_WARN("Map: %s is %dx%d, tiles must be %d px", path, decoder->getWidth(), decoder->getHeight(), kTileSize);
        decoder->close();
        return false;
    }
    const int rc = decoder->decode(out, 0);
    decoder->close();
    if (rc != PNG_SUCCESS)
        LOG_WARN("Map: can't decode %s (%d)", path, rc);
    return rc == PNG_SUCCESS;
}

// Decoded tile, loading it on a cache miss; nullptr when it isn't on the card. Valid until the next call.
const uint16_t *fetchTile(int z, int32_t x, int32_t y)
{
    const TileKey key{gen, z, x, y};
    for (auto &t : tiles) {
        if (t.valid && t.key == key) {
            t.lastUse = ++useClock;
            return t.pixels;
        }
    }
    for (const auto &m : misses) {
        if (m == key)
            return nullptr;
    }

    TileSlot *slot = &tiles[0];
    for (auto &t : tiles) {
        if (!t.valid) {
            slot = &t;
            break;
        }
        if (t.lastUse < slot->lastUse)
            slot = &t;
    }
    if (!slot->pixels)
        slot->pixels = static_cast<uint16_t *>(allocLarge((size_t)kTileSize * kTileSize * sizeof(uint16_t)));
    if (!slot->pixels)
        return nullptr;

    slot->valid = false; // a failed decode may have half-overwritten it
    if (!loadTile(key, slot->pixels)) {
        misses[nextMiss] = key;
        nextMiss = (nextMiss + 1) % kMissSlots;
        return nullptr;
    }
    slot->key = key;
    slot->lastUse = ++useClock;
    slot->valid = true;
    return slot->pixels;
}

// Fills one tile's on-screen rect from the nearest lower zoom that has the area, scaled up. False if none does.
bool drawFromParent(uint16_t *dst, int16_t w, int16_t sx0, int16_t sx1, int16_t sy0, int16_t sy1, int32_t u0, int32_t v0,
                    int zoom, int32_t tx, int32_t ty)
{
    for (int k = 1; k <= kMaxOverzoom && k <= zoom; k++) {
        const uint16_t *parent = fetchTile(zoom - k, tx >> k, ty >> k);
        if (!parent)
            continue;
        // Position of this tile inside the parent, in this zoom's pixels.
        const int32_t baseU = (tx - ((tx >> k) << k)) * kTileSize;
        const int32_t baseV = (ty - ((ty >> k) << k)) * kTileSize;
        for (int16_t sy = sy0; sy < sy1; sy++) {
            const uint16_t *src = parent + ((baseV + v0 + (sy - sy0)) >> k) * kTileSize;
            uint16_t *row = dst + (size_t)sy * w;
            for (int16_t sx = sx0; sx < sx1; sx++)
                row[sx] = src[(baseU + u0 + (sx - sx0)) >> k];
        }
        return true;
    }
    return false;
}

void fillRect(uint16_t *dst, int16_t w, int16_t sx0, int16_t sx1, int16_t sy0, int16_t sy1, uint16_t color)
{
    for (int16_t sy = sy0; sy < sy1; sy++) {
        uint16_t *row = dst + (size_t)sy * w;
        for (int16_t sx = sx0; sx < sx1; sx++)
            row[sx] = color;
    }
}

} // namespace

int refreshStyles(const char *preferred)
{
    char wanted[kNameSize] = {};
    const bool havePreferred = preferred != nullptr;
    if (havePreferred)
        strncpy(wanted, preferred, kNameSize - 1); // may point into styles[], which the scan overwrites
    char previous[kNameSize] = {};
    if (active >= 0)
        memcpy(previous, styles[active], kNameSize);
    const bool hadActive = active >= 0;

    numStyles = 0;
    {
        concurrency::LockGuard g(spiLock);
        SdFs *sd = mapSdCard();
        if (sd) {
            FsFile dir = sd->open("/maps", O_RDONLY);
            if (dir && dir.isDir()) {
                FsFile entry;
                while (numStyles < kMaxStyles && entry.openNext(&dir, O_RDONLY)) {
                    char name[kNameSize];
                    const size_t len = entry.getName(name, sizeof(name));
                    if (entry.isDir() && len > 0 && name[0] != '.') {
                        int at = numStyles++; // kept sorted, like device-ui's std::set
                        while (at > 0 && strcmp(styles[at - 1], name) > 0) {
                            memcpy(styles[at], styles[at - 1], kNameSize);
                            at--;
                        }
                        memcpy(styles[at], name, kNameSize);
                    }
                    entry.close();
                }
            }
            dir.close();
            if (numStyles == 0) {
                FsFile map = sd->open("/map", O_RDONLY);
                if (map && map.isDir()) {
                    styles[0][0] = '\0';
                    numStyles = 1;
                }
                map.close();
            }
        }
    }

    int pick = numStyles > 0 ? 0 : -1;
    for (int i = 0; havePreferred && i < numStyles; i++) {
        if (strcmp(styles[i], wanted) == 0) {
            pick = i;
            break;
        }
    }
    active = pick;
    if (!hadActive || active < 0 || strcmp(previous, styles[active]) != 0)
        gen++;
    memset(misses, 0, sizeof(misses)); // give tiles added since the last scan another look
    LOG_INFO("Map: %d PNG tile style(s) on SD", numStyles);
    return numStyles;
}

int styleCount()
{
    return numStyles;
}

const char *styleName(int index)
{
    return (index >= 0 && index < numStyles) ? styles[index] : "";
}

int activeStyle()
{
    return active;
}

void setActiveStyle(int index)
{
    if (index < 0 || index >= numStyles || index == active)
        return;
    active = index;
    gen++;
}

uint32_t generation()
{
    return gen;
}

void renderView(uint16_t *dst, int16_t w, int16_t h, int32_t centerX, int32_t centerY, int zoom, uint16_t bg)
{
    const int32_t left = centerX - w / 2;
    const int32_t top = centerY - h / 2;
    const int32_t side = (int32_t)1 << zoom;

    for (int32_t tileTop = floorToTile(top); tileTop < top + h; tileTop += kTileSize) {
        const int32_t ty = tileTop / kTileSize;
        const int16_t sy0 = (int16_t)(tileTop > top ? tileTop - top : 0);
        const int16_t sy1 = (int16_t)(tileTop + kTileSize - top < h ? tileTop + kTileSize - top : h);
        const int32_t v0 = top + sy0 - tileTop;

        for (int32_t tileLeft = floorToTile(left); tileLeft < left + w; tileLeft += kTileSize) {
            const int16_t sx0 = (int16_t)(tileLeft > left ? tileLeft - left : 0);
            const int16_t sx1 = (int16_t)(tileLeft + kTileSize - left < w ? tileLeft + kTileSize - left : w);
            const int32_t u0 = left + sx0 - tileLeft;
            if (ty < 0 || ty >= side || active < 0) { // latitude doesn't wrap
                fillRect(dst, w, sx0, sx1, sy0, sy1, bg);
                continue;
            }
            const int32_t tx = ((tileLeft / kTileSize) % side + side) % side; // longitude does

            if (const uint16_t *tile = fetchTile(zoom, tx, ty)) {
                for (int16_t sy = sy0; sy < sy1; sy++)
                    memcpy(dst + (size_t)sy * w + sx0, tile + (v0 + (sy - sy0)) * kTileSize + u0,
                           (size_t)(sx1 - sx0) * sizeof(uint16_t));
            } else if (!drawFromParent(dst, w, sx0, sx1, sy0, sy1, u0, v0, zoom, tx, ty)) {
                fillRect(dst, w, sx0, sx1, sy0, sy1, bg);
            }
        }
    }
}

} // namespace NicheGraphics::MapTiles::Png

#endif
