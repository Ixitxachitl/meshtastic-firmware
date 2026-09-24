#include "./MapPngTiles.h"

#if BASEUI_MAP_PNG_TILES

#include "./MapTileFetch.h"
#include "./MapTileSourceSD.h"
#include "DebugConfiguration.h"
#include "SPILock.h"
#include "mesh/Throttle.h"

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
    uint16_t *const row = out + draw->y * kTileSize;
    decoder->getLineAsRGB565(draw, row, PNG_RGB565_LITTLE_ENDIAN, 0);

    // Transparent pixels come out as the black they were blended onto, and some tile downloaders leave a whole edge
    // column transparent - a dark line down the map. Take each from its nearest opaque neighbour in the row instead.
    if (draw->iHasAlpha || draw->iPixelType == PNG_PIXEL_TRUECOLOR_ALPHA || draw->iPixelType == PNG_PIXEL_GRAY_ALPHA) {
        uint8_t mask[kTileSize / 8];
        if (!decoder->getAlphaMask(draw, mask, 128))
            return 1;                                                                 // nothing opaque in the row to borrow from
        auto opaque = [&](int x) { return (mask[x >> 3] & (0x80 >> (x & 7))) != 0; }; // MSB first
        int16_t nearestLeft[kTileSize];
        for (int x = 0, last = -1; x < kTileSize; x++) {
            if (opaque(x))
                last = x;
            nearestLeft[x] = (int16_t)last;
        }
        for (int x = kTileSize - 1, right = -1; x >= 0; x--) {
            if (opaque(x)) {
                right = x;
                continue;
            }
            const int left = nearestLeft[x];
            const int from = (left >= 0 && (right < 0 || x - left <= right - x)) ? left : right;
            if (from >= 0)
                row[x] = row[from];
        }
    }
    return 1;
}

// Missing is remembered so it isn't looked up every frame; Failed (a bad read or decode) is tried again later.
enum class TileLoad { Loaded, Missing, Failed };

// Reads the whole file into fileBuf.
TileLoad readTileFile(const char *path, size_t &size)
{
    concurrency::LockGuard g(spiLock);
    SdFs *sd = mapSdCard();
    if (!sd)
        return TileLoad::Failed;
    FsFile file = sd->open(path, O_RDONLY);
    if (!file)
        return TileLoad::Missing;
    const uint64_t fileSize = file.fileSize();
    if (fileSize == 0 || fileSize > kMaxFileBytes) {
        file.close();
        return TileLoad::Missing;
    }
    size = (size_t)fileSize;
    if (size > fileBufSize) {
        free(fileBuf);
        fileBuf = static_cast<uint8_t *>(allocLarge(size));
        fileBufSize = fileBuf ? size : 0;
    }
    const bool ok = fileBuf && file.read(fileBuf, size) == (int)size;
    file.close();
    return ok ? TileLoad::Loaded : TileLoad::Failed;
}

TileLoad loadTile(const TileKey &key, uint16_t *out)
{
    char path[64];
    if (styles[active][0])
        snprintf(path, sizeof(path), "/maps/%s/%d/%d/%d.png", styles[active], (int)key.z, (int)key.x, (int)key.y);
    else
        snprintf(path, sizeof(path), "/map/%d/%d/%d.png", (int)key.z, (int)key.x, (int)key.y);

    if (!decoder) {
        void *mem = allocLarge(sizeof(PNG));
        if (!mem)
            return TileLoad::Failed;
        decoder = new (mem) PNG();
    }

    // Unchecked, a byte corrupted in the read decodes into a streak down the tile (each PNG row builds on the one above).
    // With the checksum on it fails instead, and the card gets a second read.
    for (int attempt = 0; attempt < 2; attempt++) {
        size_t size = 0;
        const TileLoad read = readTileFile(path, size);
        if (read == TileLoad::Missing)
            return TileLoad::Missing;
        if (read == TileLoad::Failed) {
            LOG_WARN("Map: short read of %s%s", path, attempt == 0 ? ", reading it again" : "");
            continue;
        }
        if (decoder->openRAM(fileBuf, (int)size, drawRow) != PNG_SUCCESS) {
            LOG_WARN("Map: can't open %s (%d)%s", path, decoder->getLastError(), attempt == 0 ? ", reading it again" : "");
            continue;
        }
        if (decoder->getWidth() != kTileSize || decoder->getHeight() != kTileSize) {
            LOG_WARN("Map: %s is %dx%d, tiles must be %d px", path, decoder->getWidth(), decoder->getHeight(), kTileSize);
            decoder->close();
            return TileLoad::Missing;
        }
        const int rc = decoder->decode(out, PNG_CHECK_CRC);
        decoder->close();
        if (rc == PNG_SUCCESS)
            return TileLoad::Loaded;
        LOG_WARN("Map: %s failed to decode (%d)%s", path, rc, attempt == 0 ? ", reading it again" : "");
    }
    return TileLoad::Failed;
}

// Decoded tile, loading it on a cache miss; nullptr when it isn't on the card or couldn't be read. Valid until the
// next call.
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
    const TileLoad result = loadTile(key, slot->pixels);
    if (result != TileLoad::Loaded) {
        if (result == TileLoad::Missing) {
            misses[nextMiss] = key;
            nextMiss = (nextMiss + 1) % kMissSlots;
#if BASEUI_MAP_ONLINE_TILES
            // The miss slot is what keeps this to one request per tile rather than one per frame.
            Fetch::requestTile(z, x, y);
#endif
        }
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

// Fetched tiles arrive in bursts - a view at one zoom pulls its neighbours and the lower zooms behind it.
// The renderer keys its cached basemap on generation(), and rebuilding that costs a full screen of tile
// decoding: 400-700ms on a large panel. Bumping per tile meant paying it once per arrival, back to back,
// with the screen thread pinned and touch starved for the whole download. Publish once the burst goes
// quiet instead, with a ceiling so a long run of fetches still shows progress.
constexpr uint32_t kArrivalSettleMs = 750;    // quiet period before a burst is published
constexpr uint32_t kArrivalMaxDeferMs = 4000; // ...and the longest a steady stream can hold it off
bool arrivalsPending = false;
uint32_t lastArrivalMs = 0;
uint32_t arrivalsSinceMs = 0;

void noteTileArrived(int z, int32_t x, int32_t y)
{
    for (auto &m : misses) {
        if (m.gen == gen && m.z == z && m.x == x && m.y == y)
            m = TileKey{};
    }
    // Only the bookkeeping here; generation() decides when the screen is told, so a tile landing while the
    // next is already downloading does not cost a rebuild of its own.
    if (!arrivalsPending) {
        arrivalsPending = true;
        arrivalsSinceMs = millis();
    }
    lastArrivalMs = millis();
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
    // Publishing here rather than in noteTileArrived() keeps the decision on the thread that draws: the
    // new tiles are already on the card, so all that is deferred is the rebuild that reveals them.
    if (arrivalsPending &&
        (Throttle::hasElapsed(lastArrivalMs, kArrivalSettleMs) || Throttle::hasElapsed(arrivalsSinceMs, kArrivalMaxDeferMs))) {
        arrivalsPending = false;
        gen++;
    }
    return gen;
}

void renderView(uint16_t *dst, int16_t w, int16_t h, int32_t centerX, int32_t centerY, int zoom, uint16_t bg)
{
#if BASEUI_MAP_ONLINE_TILES
    Fetch::noteMapDrawn(); // fetching runs only while the map is the frame on screen
#endif
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
