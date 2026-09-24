#include "./MapTileFetch.h"

#if BASEUI_MAP_ONLINE_TILES

#include "./MapPngTiles.h"
#include "./MapTileSourceSD.h"
#include "./MapTileUrl.h"
#include "DebugConfiguration.h"
#include "NodeDB.h"
#include "SPILock.h"
#include "concurrency/OSThread.h"
#include "mesh/Throttle.h"
#if defined(SENSECAP_INDICATOR)
#include "mesh/IndicatorRemoteFS.h"
#endif

#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>

namespace NicheGraphics::MapTiles::Fetch
{
namespace
{

// OSM's tile usage policy: identify the client, and don't hammer. One request at a time, half a second apart.
constexpr uint32_t kMinRequestGapMs = 500;
constexpr uint32_t kHttpTimeoutMs = 5000;
constexpr uint32_t kMapIdleMs = 10000;       // stop fetching this long after the map stops drawing
constexpr size_t kMaxTileBytes = 256 * 1024; // larger than any sane 256px tile; a bad URL can't fill the card
constexpr uint8_t kQueueSlots = 8;

struct Request {
    int16_t z;
    int32_t x, y;
};

// Single producer (display task) and single consumer (the fetch task), so head/tail need no mutex.
Request queue[kQueueSlots];
volatile uint8_t head = 0, tail = 0;

uint32_t lastDrawnMs = 0;
uint32_t lastRequestMs = 0;

char urlTemplate[160];
char templateStyle[24];
bool templateLoaded = false;

uint8_t *buffer = nullptr;

#if defined(SENSECAP_INDICATOR)
// The card is on the RP2040, reached over the interdevice link with device-ui's backend. This task's own
// instance: its response buffers are not shared with the display task reading tiles.
IndicatorRemoteFS &remoteCard()
{
    static IndicatorRemoteFS *fs = new IndicatorRemoteFS();
    return *fs;
}
#endif

bool onlineEnabled()
{
    return uiconfig.has_map_data && uiconfig.map_data.online_tiles;
}

bool mapOnScreen()
{
    return lastDrawnMs != 0 && Throttle::isWithinTimespanMs(lastDrawnMs, kMapIdleMs);
}

const char *activeStyleName()
{
    const int style = Png::activeStyle();
    return style >= 0 ? Png::styleName(style) : "";
}

// Reads /maps/<style>/.url once per style. device-ui writes the same file, so a card set up for one UI works
// in the other untouched.
bool loadTemplate()
{
    const char *style = activeStyleName();
    if (templateLoaded && strncmp(templateStyle, style, sizeof(templateStyle) - 1) == 0)
        return urlTemplate[0] != '\0';

    templateLoaded = true;
    strncpy(templateStyle, style, sizeof(templateStyle) - 1);
    templateStyle[sizeof(templateStyle) - 1] = '\0';
    urlTemplate[0] = '\0';

    char path[64];
    if (style[0])
        snprintf(path, sizeof(path), "/maps/%s/.url", style);
    else
        snprintf(path, sizeof(path), "/map/.url");

#if defined(SENSECAP_INDICATOR)
    uint32_t got = 0, fileSize = 0;
    if (!remoteCard().readChunk(path, 0, reinterpret_cast<uint8_t *>(urlTemplate), sizeof(urlTemplate) - 1, &got, &fileSize))
        return false;
    const int read = (int)got;
#else
    concurrency::LockGuard g(spiLock);
    SdFs *sd = mapSdCard();
    if (!sd)
        return false;
    FsFile file = sd->open(path, O_RDONLY);
    if (!file)
        return false;
    const int read = file.read(urlTemplate, sizeof(urlTemplate) - 1);
    file.close();
#endif
    if (read <= 0) {
        urlTemplate[0] = '\0';
        return false;
    }
    urlTemplate[read] = '\0';
    for (char *p = urlTemplate; *p; p++) { // one line; strip the newline and anything after it
        if (*p == '\r' || *p == '\n') {
            *p = '\0';
            break;
        }
    }
    LOG_INFO("Map: tile URL for style '%s' is %s", style[0] ? style : "map", urlTemplate);
    return urlTemplate[0] != '\0';
}

// Writes to a temp name and renames, so an interrupted download never leaves a half PNG to be decoded later.
bool writeTile(const char *style, int z, int32_t x, int32_t y, const uint8_t *data, size_t len)
{
    char dir[64], finalPath[80], tempPath[80];
    if (style[0])
        snprintf(dir, sizeof(dir), "/maps/%s/%d/%d", style, z, (int)x);
    else
        snprintf(dir, sizeof(dir), "/map/%d/%d", z, (int)x);
    snprintf(finalPath, sizeof(finalPath), "%s/%d.png", dir, (int)y);
    snprintf(tempPath, sizeof(tempPath), "%s/%d.part", dir, (int)y);

#if defined(SENSECAP_INDICATOR)
    // As device-ui's RemoteSDService::save(): the co-processor creates the folders, and the link has no
    // rename, so the tile is written in place and removed again if any chunk fails.
    IndicatorRemoteFS &fs = remoteCard();
    constexpr size_t kChunk = sizeof(meshtastic_FileTransfer_filedata_t::bytes);
    for (size_t offset = 0; offset < len; offset += kChunk) {
        const size_t chunk = (len - offset) < kChunk ? (len - offset) : kChunk;
        if (!fs.writeChunk(finalPath, (uint32_t)offset, data + offset, (uint32_t)chunk, offset == 0)) {
            if (offset > 0)
                fs.remove(finalPath); // a truncated tile would pass as present and never be fetched again
            LOG_WARN("Map: can't write %s", finalPath);
            return false;
        }
    }
    return true;
#else
    concurrency::LockGuard g(spiLock);
    SdFs *sd = mapSdCard();
    if (!sd)
        return false;
    if (!sd->exists(dir) && !sd->mkdir(dir, true)) {
        LOG_WARN("Map: can't create %s", dir);
        return false;
    }
    sd->remove(tempPath);
    FsFile file = sd->open(tempPath, O_WRONLY | O_CREAT | O_TRUNC);
    if (!file) {
        LOG_WARN("Map: can't open %s", tempPath);
        return false;
    }
    const bool ok = file.write(data, len) == len;
    file.close();
    if (!ok) {
        sd->remove(tempPath);
        LOG_WARN("Map: short write of %s", tempPath);
        return false;
    }
    sd->remove(finalPath); // rename() will not replace an existing file
    if (!sd->rename(tempPath, finalPath)) {
        sd->remove(tempPath);
        LOG_WARN("Map: can't rename %s", tempPath);
        return false;
    }
    return true;
#endif
}

// Returns the byte count, or 0. Body is staged in PSRAM rather than streamed to the card, so spiLock is taken
// once for a quick write instead of being held across the whole transfer while the display waits.
size_t download(const char *url)
{
    if (!buffer) {
        buffer = static_cast<uint8_t *>(heap_caps_malloc(kMaxTileBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!buffer)
            buffer = static_cast<uint8_t *>(malloc(kMaxTileBytes));
        if (!buffer)
            return 0;
    }

    // APP_VERSION arrives unquoted from the build flags; xstr() comes from configuration.h.
    static const char *userAgent = "Meshtastic/" xstr(APP_VERSION) " (+https://meshtastic.org)";

    HTTPClient http;
    http.setReuse(true);
    http.setTimeout(kHttpTimeoutMs);
    http.setConnectTimeout(kHttpTimeoutMs);
    http.setUserAgent(userAgent);
    if (!http.begin(url)) {
        LOG_WARN("Map: bad tile URL %s", url);
        return 0;
    }

    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        LOG_WARN("Map: tile fetch got %d for %s", code, url);
        http.end();
        return 0;
    }

    const int contentLength = http.getSize();
    if (contentLength > (int)kMaxTileBytes) {
        LOG_WARN("Map: tile at %s is %d bytes, too large", url, contentLength);
        http.end();
        return 0;
    }

    WiFiClient *stream = http.getStreamPtr();
    size_t total = 0;
    uint32_t idleSince = millis();
    while (http.connected() && (contentLength < 0 || total < (size_t)contentLength)) {
        const size_t available = stream->available();
        if (available) {
            const size_t room = kMaxTileBytes - total;
            if (room == 0)
                break;
            const int got = stream->readBytes(buffer + total, available < room ? available : room);
            if (got <= 0)
                break;
            total += (size_t)got;
            idleSince = millis();
        } else {
            if (!Throttle::isWithinTimespanMs(idleSince, kHttpTimeoutMs))
                break;
            delay(2);
        }
    }
    http.end();

    if (contentLength > 0 && total != (size_t)contentLength) {
        LOG_WARN("Map: tile fetch truncated at %u of %d bytes", (unsigned)total, contentLength);
        return 0;
    }
    return total;
}

class TileFetcher : private concurrency::OSThread
{
  public:
    TileFetcher() : concurrency::OSThread("MapTileFetch")
    {
        // HTTPClient blocks, so this must not run on the cooperative main loop.
        setFreeRTOSTask(true, 8192, tskIDLE_PRIORITY + 1, 1);
        if (!startFreeRTOSTask())
            LOG_WARN("Map: tile fetch has no task; downloads will stall the main loop");
    }

    void poke() { wakeFreeRTOSTask(); }

  private:
    virtual int32_t runOnce() override
    {
        if (head == tail)
            return 1000;

        // Conditions can lapse between queueing and running - drop the backlog rather than fetch behind the map.
        if (!onlineEnabled() || !mapOnScreen() || WiFi.status() != WL_CONNECTED) {
            tail = head;
            return 1000;
        }
        if (Throttle::isWithinTimespanMs(lastRequestMs, kMinRequestGapMs))
            return (int32_t)kMinRequestGapMs;
        if (!loadTemplate()) {
            tail = head; // no .url for this style, so nothing here is fetchable
            return 1000;
        }

        const Request request = queue[tail];
        tail = (uint8_t)((tail + 1) % kQueueSlots);
        lastRequestMs = millis();

        char url[200];
        if (!expandTileUrl(urlTemplate, request.z, request.x, request.y, url, sizeof(url)))
            return (int32_t)kMinRequestGapMs;

        const size_t len = download(url);
        if (len && writeTile(activeStyleName(), request.z, request.x, request.y, buffer, len)) {
            LOG_INFO("Map: fetched z%d/%d/%d (%u bytes)", request.z, (int)request.x, (int)request.y, (unsigned)len);
            Png::noteTileArrived(request.z, request.x, request.y);
        }
        return (int32_t)kMinRequestGapMs;
    }
};

TileFetcher *fetcher = nullptr;

} // namespace

void noteMapDrawn()
{
    lastDrawnMs = millis();
}

void requestTile(int z, int32_t x, int32_t y)
{
    if (z < 0 || z > 19 || !onlineEnabled() || !mapOnScreen() || WiFi.status() != WL_CONNECTED)
        return;

    const uint8_t next = (uint8_t)((head + 1) % kQueueSlots);
    if (next == tail) // full: the view has moved on by the time a backlog this deep would be served
        return;
    for (uint8_t i = tail; i != head; i = (uint8_t)((i + 1) % kQueueSlots)) {
        if (queue[i].z == z && queue[i].x == x && queue[i].y == y)
            return;
    }

    queue[head] = Request{(int16_t)z, x, y};
    head = next;

    if (!fetcher)
        fetcher = new TileFetcher(); // first miss with everything in place; never on a card-only build
    fetcher->poke();
}

} // namespace NicheGraphics::MapTiles::Fetch

#endif // BASEUI_MAP_ONLINE_TILES
