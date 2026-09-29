#include "./MapTileFetch.h"

#if BASEUI_MAP_ONLINE_TILES

#include "./MapGeocodeParse.h"
#include "./MapJpegTile.h"
#include "./MapPngTiles.h"
#include "./MapRouteTiles.h"
#include "./MapTileSourceSD.h"
#include "./MapTileUrl.h"
#include "DebugConfiguration.h"
#include "NodeDB.h"
#include "SPILock.h"
#include "concurrency/OSThread.h"
#include "memory/MemAudit.h"
#include "mesh/Throttle.h"
#if defined(SENSECAP_INDICATOR)
#include "mesh/IndicatorRemoteFS.h"
#endif

#include <HTTPClient.h>
#include <WiFi.h>
#include <algorithm>
#include <atomic>
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

char urlTemplate[384]; // API keys make these long: a Mapbox token alone is ~100 chars
char tileUrl[416];     // the expanded template; static, as this runs on the main loop's stack
char templateStyle[24];
bool templateLoaded = false;

uint8_t *buffer = nullptr;

#if BASEUI_MAP_ADDRESS_SEARCH
// Nominatim's usage policy: at most one request a second, from an identified client.
constexpr uint32_t kMinSearchGapMs = 1100;
// The display task writes the query and moves Idle/Done/Failed -> Queued; this task moves Queued -> Running ->
// Done/Failed and only then touches the results, so the state is the only thing both sides race on.
std::atomic<SearchState> searchStatus{SearchState::Idle};
char searchQuery[128];
char searchUrl[512];
Geocode::Result searchHits[Geocode::kMaxResults];
int searchHitCount = 0;
uint32_t lastSearchMs = 0;
#endif

#if BASEUI_MAP_ROUTING
// Thinned to fit: a few metres between points on a city route, more on a long one, which the map can't tell apart.
constexpr uint32_t kRoutePointCap = 12000;
constexpr uint16_t kRouteManeuverCap = 256;
// A long route's turn text runs past the tile buffer; this one lives only for the request.
constexpr size_t kMaxRouteBytes = 1024 * 1024;
constexpr uint32_t kRouteTimeoutMs = 20000; // the server works out the route before the first byte
constexpr const char *kRouteDir = "/maps/.routes";

enum class RouteJob : uint8_t { Fetch, Load };
std::atomic<RouteState> routeStatus{RouteState::Idle};
RouteJob routeJob = RouteJob::Fetch;
struct {
    double fromLat, fromLon, toLat, toLon;
    Route::Mode mode;
    RouteStore::Header saveAs;
    int slot; // to save into or load from; -1 picks one
} routeRequest;
char routeUrl[1024];
int32_t *routeLat = nullptr, *routeLon = nullptr;
Route::Maneuver *routeManeuvers = nullptr;
Route::Result routeParsed{};
volatile int routeSlotUsed = -1;

std::atomic<ListState> listStatus{ListState::Idle};
SavedRoute listed[kRouteSlots];
int listedCount = 0;
std::atomic<uint32_t> pendingDeletes{0}; // a bit per slot

// The tile download along a saved route: its own copy of the points, walked one tile per turn.
std::atomic<DownloadState> downloadStatus{DownloadState::Idle};
std::atomic<bool> downloadCancel{false};
int downloadSlot = -1;
std::atomic<uint32_t> downloadDone{0}, downloadTotal{0}, downloadFailed{0};
const char *downloadReason = "";
int32_t *downloadLat = nullptr, *downloadLon = nullptr;
Route::Maneuver *downloadManeuvers = nullptr;
RouteTileWalker downloadWalker;
constexpr uint8_t kDownloadZooms[] = {12, 13, 14, 15, 16}; // driving zooms, and the 3D view's distance
constexpr int kDownloadRadius = 1;                         // tiles either side of the road
#endif

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

// Reads the first line of a small card file into buf. False if it is missing, empty, or too long for buf - a cut
// line could have lost the end of an API key, so it is refused rather than used.
bool readFirstLine(const char *path, char *buf, size_t size)
{
    buf[0] = '\0';
#if defined(SENSECAP_INDICATOR)
    uint32_t got = 0, fileSize = 0;
    if (!remoteCard().readChunk(path, 0, reinterpret_cast<uint8_t *>(buf), size - 1, &got, &fileSize))
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
    const int read = file.read(buf, size - 1);
    file.close();
#endif
    if (read <= 0) {
        buf[0] = '\0';
        return false;
    }
    buf[read] = '\0';
    bool lineEnded = false;
    for (char *p = buf; *p; p++) { // one line; strip the newline and anything after it
        if (*p == '\r' || *p == '\n') {
            *p = '\0';
            lineEnded = true;
            break;
        }
    }
    if (!lineEnded && read == (int)size - 1) {
        LOG_WARN("Map: the line in %s is longer than %u chars", path, (unsigned)size - 1);
        buf[0] = '\0';
        return false;
    }
    return buf[0] != '\0';
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

    char path[64];
    if (style[0])
        snprintf(path, sizeof(path), "/maps/%s/.url", style);
    else
        snprintf(path, sizeof(path), "/map/.url");
    if (!readFirstLine(path, urlTemplate, sizeof(urlTemplate)))
        return false;
    LOG_INFO("Map: tile URL for style '%s' is %s", style[0] ? style : "map", urlTemplate);
    return true;
}

// Writes a file on the card: to a temp name and renamed into place, so an interrupted write never leaves half a file
// to be read later. The Indicator's link has no rename, so there it is written in place and removed if any chunk fails.
bool writeCardFile(const char *dir, const char *finalPath, const char *tempPath, const uint8_t *data, size_t len)
{
#if defined(SENSECAP_INDICATOR)
    (void)dir; // the co-processor creates the folders, as device-ui's RemoteSDService::save() relies on
    (void)tempPath;
    IndicatorRemoteFS &fs = remoteCard();
    constexpr size_t kChunk = sizeof(meshtastic_FileTransfer_filedata_t::bytes);
    for (size_t offset = 0; offset < len; offset += kChunk) {
        const size_t chunk = (len - offset) < kChunk ? (len - offset) : kChunk;
        if (!fs.writeChunk(finalPath, (uint32_t)offset, data + offset, (uint32_t)chunk, offset == 0)) {
            if (offset > 0)
                fs.remove(finalPath); // a truncated file would pass as present
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

// Reads a card file into buf: all of it, or with `head` just its first cap bytes. False if it is missing, or (without
// `head`) larger than cap.
bool readCardFile(const char *path, uint8_t *buf, size_t cap, size_t &len, bool head = false)
{
    len = 0;
#if defined(SENSECAP_INDICATOR)
    IndicatorRemoteFS &fs = remoteCard();
    constexpr uint32_t kChunk = sizeof(meshtastic_FileTransfer_filedata_t::bytes);
    uint32_t got = 0, fileSize = 0;
    if (!fs.readChunk(path, 0, buf, (uint32_t)(cap < kChunk ? cap : kChunk), &got, &fileSize))
        return false;
    if (!head && fileSize > cap)
        return false;
    const size_t want = head ? (fileSize < cap ? fileSize : cap) : fileSize;
    for (len = got; len < want; len += got) {
        const uint32_t n = (want - len) < kChunk ? (uint32_t)(want - len) : kChunk;
        if (!fs.readChunk(path, (uint32_t)len, buf + len, n, &got, &fileSize) || got == 0)
            return false;
    }
    len = want;
    return true;
#else
    concurrency::LockGuard g(spiLock);
    SdFs *sd = mapSdCard();
    if (!sd)
        return false;
    FsFile file = sd->open(path, O_RDONLY);
    if (!file)
        return false;
    const uint64_t size = file.fileSize();
    if (!head && size > cap) {
        file.close();
        return false;
    }
    const size_t want = size < cap ? (size_t)size : cap;
    const int read = file.read(buf, want);
    file.close();
    if (read != (int)want)
        return false;
    len = want;
    return true;
#endif
}

bool cardFileExists(const char *path)
{
#if defined(SENSECAP_INDICATOR)
    uint8_t probe;
    uint32_t got = 0, fileSize = 0;
    return remoteCard().readChunk(path, 0, &probe, 1, &got, &fileSize) && fileSize > 0;
#else
    concurrency::LockGuard g(spiLock);
    SdFs *sd = mapSdCard();
    return sd && sd->exists(path);
#endif
}

void removeCardFile(const char *path)
{
#if defined(SENSECAP_INDICATOR)
    remoteCard().remove(path);
#else
    concurrency::LockGuard g(spiLock);
    if (SdFs *sd = mapSdCard())
        sd->remove(path);
#endif
}

// Where a tile lives: device-ui's /maps/<style>/z/x/y.png, or /map/... without style folders.
void tilePaths(const char *style, int z, int32_t x, int32_t y, char (&dir)[64], char (&finalPath)[80], char (&tempPath)[80])
{
    if (style[0])
        snprintf(dir, sizeof(dir), "/maps/%s/%d/%d", style, z, (int)x);
    else
        snprintf(dir, sizeof(dir), "/map/%d/%d", z, (int)x);
    snprintf(finalPath, sizeof(finalPath), "%s/%d.png", dir, (int)y);
    snprintf(tempPath, sizeof(tempPath), "%s/%d.part", dir, (int)y);
}

bool writeTile(const char *style, int z, int32_t x, int32_t y, const uint8_t *data, size_t len)
{
    char dir[64], finalPath[80], tempPath[80];
    tilePaths(style, z, x, y, dir, finalPath, tempPath);
    return writeCardFile(dir, finalPath, tempPath, data, len);
}

bool ensureBuffer()
{
    if (!buffer) {
        buffer = static_cast<uint8_t *>(heap_caps_malloc(kMaxTileBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!buffer)
            buffer = static_cast<uint8_t *>(malloc(kMaxTileBytes));
        if (buffer)
            memaudit::add("mapfetch", kMaxTileBytes, buffer);
    }
    return buffer != nullptr;
}

// Fetches url into dst. Returns the length, or 0 on any failure (already logged) - including a reply that fills dst,
// which may have been cut.
// Where a reply's body goes: the caller's buffer, and never past it. A full buffer makes the read fail rather than
// hand back a cut-off file.
class BufferSink : public Stream
{
  public:
    BufferSink(uint8_t *dst, size_t cap) : dst(dst), cap(cap) {}
    size_t write(uint8_t b) override { return write(&b, 1); }
    size_t write(const uint8_t *buf, size_t n) override
    {
        const size_t take = n < cap - len ? n : cap - len;
        memcpy(dst + len, buf, take);
        len += take;
        overflow |= take < n;
        return take;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
    uint8_t *const dst;
    const size_t cap;
    size_t len = 0;
    bool overflow = false;
};

// One client for every request this task makes, left connected between requests to the same server: a run of tiles
// then costs one TLS handshake instead of one each - most of a tile's time, and most of the memory churn. It is only
// ever used from this task.
HTTPClient http;
char connectedHost[96] = "";

// "tile.example.org:8080" out of "https://tile.example.org:8080/z/x/y.png".
void hostOf(const char *url, char (&host)[96])
{
    const char *start = strstr(url, "://");
    start = start ? start + 3 : url;
    size_t n = strcspn(start, "/?#");
    n = n < sizeof(host) - 1 ? n : sizeof(host) - 1;
    memcpy(host, start, n);
    host[n] = '\0';
}

// Drops the connection outright, so the next request opens a fresh one.
void closeConnection()
{
    http.setReuse(false);
    http.end();
    http.setReuse(true);
    connectedHost[0] = '\0';
}

size_t downloadRequest(const char *url, uint8_t *dst, size_t cap, uint32_t timeoutMs)
{
    // APP_VERSION arrives unquoted from the build flags; xstr() comes from configuration.h.
    static const char *userAgent = "Meshtastic/" xstr(APP_VERSION) " (+https://meshtastic.org)";
    char host[96];
    hostOf(url, host);

    for (int attempt = 0; attempt < 2; attempt++) {
        // HTTPClient reuses whatever it is connected to, whichever server the URL names - so a new host closes it first.
        const bool reusing = http.connected() && strcmp(host, connectedHost) == 0;
        if (!reusing)
            closeConnection();
        http.setReuse(true);
        http.setTimeout(timeoutMs);
        http.setConnectTimeout(kHttpTimeoutMs);
        http.setUserAgent(userAgent);
        if (!http.begin(url)) {
            LOG_WARN("Map: bad URL %s", url);
            closeConnection();
            return 0;
        }
        strncpy(connectedHost, host, sizeof(connectedHost) - 1);
        connectedHost[sizeof(connectedHost) - 1] = '\0';

        const int code = http.GET();
        if (code < 0 && reusing) {
            closeConnection(); // the server dropped it while idle: one more go on a fresh connection
            continue;
        }
        if (code != HTTP_CODE_OK) {
            LOG_WARN("Map: fetch got %d for %s", code, url);
            closeConnection(); // the body was never read, so the connection can't carry the next request
            return 0;
        }
        const int contentLength = http.getSize();
        if (contentLength > (int)cap) {
            LOG_WARN("Map: %s is %d bytes, too large", url, contentLength);
            closeConnection();
            return 0;
        }

        // writeToStream reads the whole body - by its length, chunk by chunk, or to the close - so the connection is
        // left clean for the next request.
        BufferSink sink(dst, cap);
        const int got = http.writeToStream(&sink);
        if (got < 0 || sink.overflow || (contentLength > 0 && sink.len != (size_t)contentLength)) {
            LOG_WARN("Map: reading %s failed (%d, %u bytes)", url, got, (unsigned)sink.len);
            closeConnection();
            return 0;
        }
        http.end(); // stays connected if the server allows it
        return sink.len;
    }
    return 0;
}

// Leak check for the network path, which this task drives nonstop while navigating: internal heap just before and
// after each request, summed. A total that keeps climbing means requests leave memory behind; one near zero while the
// free heap still falls puts the leak somewhere else. Other tasks allocate meanwhile, so only the trend means anything.
uint32_t requestCount = 0;
int32_t requestKept = 0;

size_t downloadInto(const char *url, uint8_t *dst, size_t cap, uint32_t timeoutMs)
{
    const size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t len = downloadRequest(url, dst, cap, timeoutMs);
    requestKept += (int32_t)before - (int32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (++requestCount % 32 == 0)
        LOG_INFO("Map fetch: %u requests, internal kept %ld; free %u, min %u, DMA block %u; task stack left %u",
                 (unsigned)requestCount, (long)requestKept, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                 (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    return len;
}

// Into the shared tile buffer.
size_t downloadRaw(const char *url)
{
    return ensureBuffer() ? downloadInto(url, buffer, kMaxTileBytes, kHttpTimeoutMs) : 0;
}

// A tile: downloadRaw, then refused unless it is an image this build decodes.
size_t download(const char *url)
{
    const size_t total = downloadRaw(url);
    if (!total)
        return 0;
    // Save only what decodes: anything else leaves a tile that fails on every draw and is never fetched again.
    static const uint8_t kPngSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (total >= sizeof(kPngSignature) && memcmp(buffer, kPngSignature, sizeof(kPngSignature)) == 0)
        return total;
#if BASEUI_MAP_JPEG_TILES
    if (Jpeg::isJpeg(buffer, total))
        return total;
    static const char *const kFormats = "PNG and JPEG";
#else
    static const char *const kFormats = "PNG";
#endif
    const char *kind = total >= 2 && buffer[0] == 0xFF && buffer[1] == 0xD8   ? "JPEG"
                       : total >= 4 && memcmp(buffer, "RIFF", 4) == 0         ? "WebP"
                       : total >= 2 && buffer[0] == 0x1F && buffer[1] == 0x8B ? "gzip"
                                                                              : "unrecognised";
    LOG_WARN("Map: tile at %s is %s (%u bytes, starts %02x %02x %02x %02x); only %s tiles work", url, kind, (unsigned)total,
             total > 0 ? buffer[0] : 0, total > 1 ? buffer[1] : 0, total > 2 ? buffer[2] : 0, total > 3 ? buffer[3] : 0,
             kFormats);
    return 0;
}

#if BASEUI_MAP_ADDRESS_SEARCH
// Runs the queued address search. Returns how long to wait before looking again.
int32_t runSearch()
{
    if (lastSearchMs && Throttle::isWithinTimespanMs(lastSearchMs, kMinSearchGapMs))
        return (int32_t)kMinSearchGapMs;
    searchStatus = SearchState::Running;
    lastSearchMs = millis();
    if (lastSearchMs == 0)
        lastSearchMs = 1;

    // A card file may point at another Nominatim-compatible server; the line must contain {q}.
    char tmpl[256];
    if (!readFirstLine("/maps/.geocode", tmpl, sizeof(tmpl))) {
        strncpy(tmpl, Geocode::kDefaultSearchUrl, sizeof(tmpl) - 1);
        tmpl[sizeof(tmpl) - 1] = '\0';
    }
    char encoded[3 * sizeof(searchQuery)];
    if (WiFi.status() != WL_CONNECTED || !Geocode::encodeQuery(searchQuery, encoded, sizeof(encoded))) {
        LOG_WARN("Map: address search not sent (WiFi down)");
        searchStatus = SearchState::Failed;
        return 1000;
    }

    // The next provider only when the one before found nothing; any answer at all counts as the search working.
    static const struct {
        Geocode::Provider provider;
        const char *url; // null: the Nominatim template above
    } kProviders[] = {{Geocode::Provider::Nominatim, nullptr},
                      {Geocode::Provider::Census, Geocode::kCensusSearchUrl},
                      {Geocode::Provider::Photon, Geocode::kPhotonSearchUrl}};
    int count = -1;
    for (const auto &p : kProviders) {
        if (!Geocode::expandSearchUrl(p.url ? p.url : tmpl, encoded, searchUrl, sizeof(searchUrl))) {
            LOG_WARN("Map: bad search URL template (/maps/.geocode needs {q})");
            continue;
        }
        const size_t len = downloadRaw(searchUrl);
        const int found =
            len ? Geocode::parseFor(p.provider, reinterpret_cast<const char *>(buffer), len, searchHits, Geocode::kMaxResults)
                : -1;
        if (found > 0) {
            count = found;
            break;
        }
        if (found == 0)
            count = 0;
    }
    if (count < 0) {
        LOG_WARN("Map: address search failed at every provider");
        searchStatus = SearchState::Failed;
        return 1000;
    }
    searchHitCount = count;
    LOG_INFO("Map: address search found %d place(s)", count);
    searchStatus = SearchState::Done;
    return 1000;
}
#endif

#if BASEUI_MAP_ROUTING
void slotPaths(int slot, char (&finalPath)[48], char (&tempPath)[48])
{
    snprintf(finalPath, sizeof(finalPath), "%s/r%d.nav", kRouteDir, slot);
    snprintf(tempPath, sizeof(tempPath), "%s/r%d.tmp", kRouteDir, slot);
}

// The header of each saved route on the card, newest first.
int readSavedHeaders(SavedRoute *out)
{
    int count = 0;
    for (int slot = 0; slot < kRouteSlots; slot++) {
        char path[48], temp[48];
        slotPaths(slot, path, temp);
        uint8_t head[RouteStore::kHeaderBytes];
        size_t len = 0;
        RouteStore::Header h;
        if (readCardFile(path, head, sizeof(head), len, true) && RouteStore::decodeHeader(head, len, h))
            out[count++] = SavedRoute{(int8_t)slot, h};
    }
    std::sort(out, out + count, [](const SavedRoute &a, const SavedRoute &b) { return a.header.sequence > b.header.sequence; });
    return count;
}

bool allocRouteArrays(int32_t *&lat, int32_t *&lon, Route::Maneuver *&turns)
{
    if (lat)
        return true;
    lat = static_cast<int32_t *>(heap_caps_malloc(kRoutePointCap * sizeof(int32_t), MALLOC_CAP_SPIRAM));
    lon = static_cast<int32_t *>(heap_caps_malloc(kRoutePointCap * sizeof(int32_t), MALLOC_CAP_SPIRAM));
    turns = static_cast<Route::Maneuver *>(heap_caps_malloc(kRouteManeuverCap * sizeof(Route::Maneuver), MALLOC_CAP_SPIRAM));
    if (lat && lon && turns) {
        memaudit::add("mapfetch", 2 * kRoutePointCap * sizeof(int32_t) + kRouteManeuverCap * sizeof(Route::Maneuver), lat);
        return true;
    }
    free(lat);
    free(lon);
    free(turns);
    lat = lon = nullptr;
    turns = nullptr;
    return false;
}

// Reads a saved route file into the given arrays. The file buffer is only for the read.
bool loadSaved(int slot, RouteStore::Header &h, int32_t *lat, int32_t *lon, Route::Maneuver *turns)
{
    char path[48], temp[48];
    slotPaths(slot, path, temp);
    const size_t cap = RouteStore::maxEncodedSize(kRoutePointCap, kRouteManeuverCap);
    uint8_t *file = static_cast<uint8_t *>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!file)
        return false;
    size_t len = 0;
    const bool ok = readCardFile(path, file, cap, len) &&
                    RouteStore::decode(file, len, h, lat, lon, kRoutePointCap, turns, kRouteManeuverCap);
    free(file);
    return ok;
}

// Saves the route just parsed: into the slot asked for, or a free one, or the oldest. Returns the slot, or -1.
int saveParsed(RouteStore::Header h, int slot)
{
    SavedRoute existing[kRouteSlots];
    const int count = readSavedHeaders(existing);
    uint32_t newest = 0;
    bool used[kRouteSlots] = {};
    for (int i = 0; i < count; i++) {
        used[existing[i].slot] = true;
        newest = std::max(newest, existing[i].header.sequence);
    }
    if (slot < 0 || slot >= kRouteSlots) {
        slot = -1;
        for (int i = 0; i < kRouteSlots && slot < 0; i++)
            if (!used[i])
                slot = i;
        if (slot < 0)
            slot = existing[count - 1].slot; // the oldest
    }
    h.sequence = newest + 1;
    h.pointCount = routeParsed.pointCount;
    h.maneuverCount = routeParsed.maneuverCount;
    h.lengthKm = routeParsed.lengthKm;
    h.timeSec = routeParsed.timeSec;

    const size_t cap = RouteStore::maxEncodedSize(h.pointCount, h.maneuverCount);
    uint8_t *file = static_cast<uint8_t *>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!file)
        return -1;
    const size_t len = RouteStore::encode(h, routeLat, routeLon, routeManeuvers, file, cap);
    char path[48], temp[48];
    slotPaths(slot, path, temp);
    const bool ok = len && writeCardFile(kRouteDir, path, temp, file, len);
    free(file);
    if (!ok)
        return -1;
    LOG_INFO("Map: route saved as %s (%u bytes)", path, (unsigned)len);
    return slot;
}

int32_t runRoute()
{
    routeStatus = RouteState::Running;
    auto fail = [](const char *why) {
        LOG_WARN("Map: route failed (%s)", why);
        routeStatus = RouteState::Failed;
        return (int32_t)1000;
    };
    if (!allocRouteArrays(routeLat, routeLon, routeManeuvers))
        return fail("no memory");

    if (routeJob == RouteJob::Load) {
        RouteStore::Header h;
        if (!loadSaved(routeRequest.slot, h, routeLat, routeLon, routeManeuvers))
            return fail("saved route unreadable");
        routeParsed = Route::Result{routeLat,          routeLon,        kRoutePointCap, h.pointCount, routeManeuvers,
                                    kRouteManeuverCap, h.maneuverCount, h.lengthKm,     h.timeSec};
        routeSlotUsed = routeRequest.slot;
        routeStatus = RouteState::Done;
        return 100;
    }

    // A card file may point at another Valhalla server; the line must contain {json}.
    char tmpl[256];
    if (!readFirstLine("/maps/.route", tmpl, sizeof(tmpl))) {
        strncpy(tmpl, Route::kDefaultRouteUrl, sizeof(tmpl) - 1);
        tmpl[sizeof(tmpl) - 1] = '\0';
    }
    if (WiFi.status() != WL_CONNECTED)
        return fail("WiFi down");
    if (!Route::buildRequestUrl(tmpl, routeRequest.fromLat, routeRequest.fromLon, routeRequest.toLat, routeRequest.toLon,
                                routeRequest.mode, routeUrl, sizeof(routeUrl)))
        return fail("bad /maps/.route, it needs {json}");

    uint8_t *reply = static_cast<uint8_t *>(heap_caps_malloc(kMaxRouteBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!reply)
        return fail("no memory for the reply");
    const size_t len = downloadInto(routeUrl, reply, kMaxRouteBytes, kRouteTimeoutMs);
    routeParsed = Route::Result{routeLat, routeLon, kRoutePointCap, 0, routeManeuvers, kRouteManeuverCap, 0, 0, 0};
    const bool ok = len && Route::parseValhalla(reinterpret_cast<const char *>(reply), len, routeParsed);
    free(reply);
    if (!ok)
        return fail(len ? "unreadable reply" : "no reply");
    LOG_INFO("Map: route of %.1f km, %u points, %u turns", routeParsed.lengthKm, (unsigned)routeParsed.pointCount,
             (unsigned)routeParsed.maneuverCount);
    routeSlotUsed = saveParsed(routeRequest.saveAs, routeRequest.slot); // the route stands even if the card is full
    routeStatus = RouteState::Done;
    return 1000;
}

int32_t runList()
{
    for (int slot = 0; slot < kRouteSlots; slot++) {
        if (pendingDeletes.fetch_and(~(1u << slot)) & (1u << slot)) {
            char path[48], temp[48];
            slotPaths(slot, path, temp);
            removeCardFile(path);
        }
    }
    if (listStatus == ListState::Queued) {
        listedCount = readSavedHeaders(listed);
        listStatus = ListState::Done;
    }
    return 100;
}

// Tile servers whose usage policy forbids downloading ahead like this.
bool bulkDownloadForbidden(const char *tmpl)
{
    return strstr(tmpl, "tile.openstreetmap.") || strstr(tmpl, "tile.osm.org");
}

// One step of the download: the whole route read and counted first, then one tile a turn at the tiles' own pace.
int32_t runDownload()
{
    auto fail = [](const char *why) {
        LOG_WARN("Map: route tile download stopped (%s)", why);
        downloadReason = why;
        downloadStatus = DownloadState::Failed;
        return (int32_t)1000;
    };
    if (downloadCancel) {
        downloadStatus = DownloadState::Idle;
        return 100;
    }
    if (downloadStatus == DownloadState::Queued) {
        if (!loadTemplate())
            return fail("this map style has no .url");
        if (bulkDownloadForbidden(urlTemplate))
            return fail("OpenStreetMap forbids bulk downloads");
        if (!allocRouteArrays(downloadLat, downloadLon, downloadManeuvers))
            return fail("no memory");
        RouteStore::Header h;
        if (!loadSaved(downloadSlot, h, downloadLat, downloadLon, downloadManeuvers))
            return fail("saved route unreadable");
        // Count once so progress has a total; the walk then runs again for real.
        downloadWalker.start(downloadLat, downloadLon, h.pointCount, kDownloadZooms, sizeof(kDownloadZooms), kDownloadRadius);
        uint32_t total = 0;
        int z;
        int32_t x, y;
        while (downloadWalker.next(z, x, y))
            total++;
        downloadTotal = total;
        downloadDone = 0;
        downloadFailed = 0;
        downloadWalker.start(downloadLat, downloadLon, h.pointCount, kDownloadZooms, sizeof(kDownloadZooms), kDownloadRadius);
        downloadStatus = DownloadState::Running;
        LOG_INFO("Map: downloading up to %u tiles along the route", (unsigned)total);
        return 100;
    }

    if (WiFi.status() != WL_CONNECTED)
        return 5000; // paused, not stopped: carries on when WiFi is back
    if (Throttle::isWithinTimespanMs(lastRequestMs, kMinRequestGapMs))
        return (int32_t)kMinRequestGapMs;
    // Tiles already on the card cost a lookup, not a request: get through a run of them in one turn.
    for (int checked = 0; checked < 32; checked++) {
        int z;
        int32_t x, y;
        if (!downloadWalker.next(z, x, y)) {
            LOG_INFO("Map: route tiles done, %u fetched or present, %u failed", (unsigned)downloadDone.load(),
                     (unsigned)downloadFailed.load());
            downloadStatus = DownloadState::Finished;
            return 1000;
        }
        char dir[64], finalPath[80], tempPath[80];
        tilePaths(activeStyleName(), z, x, y, dir, finalPath, tempPath);
        if (cardFileExists(finalPath)) {
            downloadDone++;
            continue;
        }
        lastRequestMs = millis();
        if (!expandTileUrl(urlTemplate, z, x, y, tileUrl, sizeof(tileUrl))) {
            downloadFailed++;
            return (int32_t)kMinRequestGapMs;
        }
        const size_t len = download(tileUrl);
        if (len && writeTile(activeStyleName(), z, x, y, buffer, len)) {
            downloadDone++;
            Png::noteTileArrived(z, x, y);
        } else {
            downloadFailed++;
        }
        return (int32_t)kMinRequestGapMs;
    }
    return 1;
}
#endif

class TileFetcher : private concurrency::OSThread
{
  public:
    TileFetcher() : concurrency::OSThread("MapTileFetch")
    {
        // HTTPClient blocks, so this must not run on the cooperative main loop.
        // Core 0, not the UI's: a TLS handshake per tile is heavy work, and at the loop task's priority on the loop's
        // core it time-slices with every frame and keypress - while navigating, when tiles never stop, that halves the
        // UI. Core 0's WiFi and Bluetooth tasks outrank it, so it only takes what they leave.
        setFreeRTOSTask(true, 8192, tskIDLE_PRIORITY + 1, 0);
        if (!startFreeRTOSTask())
            LOG_WARN("Map: tile fetch has no task; downloads will stall the main loop");
    }

    void poke() { wakeFreeRTOSTask(); }

  private:
    virtual int32_t runOnce() override
    {
#if BASEUI_MAP_ADDRESS_SEARCH
        if (searchStatus == SearchState::Queued) // asked for by hand, so ahead of any tiles
            return runSearch();
#endif
#if BASEUI_MAP_ROUTING
        if (routeStatus == RouteState::Queued)
            return runRoute();
        if (pendingDeletes || listStatus == ListState::Queued)
            return runList();
        // The download gets every other turn while the map is asking for tiles too, and all of them otherwise: a map on
        // screen always wants something, so waiting for its queue to empty would starve the download for the whole
        // drive. Its setup (reading the route, counting tiles) runs straight away, so progress has a total.
        const DownloadState routeTiles = downloadStatus;
        static bool downloadsTurn = false;
        downloadsTurn = !downloadsTurn;
        if (routeTiles == DownloadState::Queued || (routeTiles == DownloadState::Running && (head == tail || downloadsTurn)))
            return runDownload();
#endif
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

        // The map keeps asking for a tile while it downloads, and the queue can only drop repeats of what is still
        // waiting - so a request can arrive for a tile that has just been saved. Found on the card, it costs no request.
        char dir[64], finalPath[80], tempPath[80];
        tilePaths(activeStyleName(), request.z, request.x, request.y, dir, finalPath, tempPath);
        if (cardFileExists(finalPath)) {
            Png::noteTileArrived(request.z, request.x, request.y);
            return 1;
        }
        lastRequestMs = millis();

        if (!expandTileUrl(urlTemplate, request.z, request.x, request.y, tileUrl, sizeof(tileUrl)))
            return (int32_t)kMinRequestGapMs;

        const size_t len = download(tileUrl);
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

#if BASEUI_MAP_ADDRESS_SEARCH
bool startSearch(const char *query)
{
    const SearchState state = searchStatus;
    if (state == SearchState::Queued || state == SearchState::Running || !query || !query[0] || WiFi.status() != WL_CONNECTED)
        return false;
    strncpy(searchQuery, query, sizeof(searchQuery) - 1);
    searchQuery[sizeof(searchQuery) - 1] = '\0';
    searchHitCount = 0;
    searchStatus = SearchState::Queued;
    if (!fetcher)
        fetcher = new TileFetcher();
    fetcher->poke();
    return true;
}

SearchState searchState()
{
    return searchStatus;
}

int searchResults(const Geocode::Result *&results)
{
    results = searchHits;
    return searchStatus == SearchState::Done ? searchHitCount : 0;
}

void clearSearch()
{
    const SearchState state = searchStatus;
    if (state == SearchState::Done || state == SearchState::Failed)
        searchStatus = SearchState::Idle;
}
#endif

#if BASEUI_MAP_ROUTING
namespace
{
bool routeJobFree()
{
    const RouteState state = routeStatus;
    return state != RouteState::Queued && state != RouteState::Running;
}

void wake()
{
    if (!fetcher)
        fetcher = new TileFetcher();
    fetcher->poke();
}
} // namespace

bool startRoute(double fromLat, double fromLon, double toLat, double toLon, Route::Mode mode, const RouteStore::Header &saveAs,
                int saveSlot)
{
    if (!routeJobFree() || WiFi.status() != WL_CONNECTED)
        return false;
    routeRequest.fromLat = fromLat;
    routeRequest.fromLon = fromLon;
    routeRequest.toLat = toLat;
    routeRequest.toLon = toLon;
    routeRequest.mode = mode;
    routeRequest.saveAs = saveAs;
    routeRequest.slot = saveSlot;
    routeJob = RouteJob::Fetch;
    routeSlotUsed = -1;
    routeStatus = RouteState::Queued;
    wake();
    return true;
}

bool startLoadRoute(int slot)
{
    if (!routeJobFree() || slot < 0 || slot >= kRouteSlots)
        return false;
    routeRequest.slot = slot;
    routeJob = RouteJob::Load;
    routeSlotUsed = -1;
    routeStatus = RouteState::Queued;
    wake();
    return true;
}

RouteState routeState()
{
    return routeStatus;
}

const Route::Result *routeResult()
{
    return routeStatus == RouteState::Done ? &routeParsed : nullptr;
}

int routeSlot()
{
    return routeSlotUsed;
}

void clearRoute()
{
    const RouteState state = routeStatus;
    if (state == RouteState::Done || state == RouteState::Failed)
        routeStatus = RouteState::Idle;
}

bool startListRoutes()
{
    if (listStatus == ListState::Queued)
        return false;
    listStatus = ListState::Queued;
    wake();
    return true;
}

ListState listState()
{
    return listStatus;
}

int savedRoutes(const SavedRoute *&routes)
{
    routes = listed;
    return listStatus == ListState::Done ? listedCount : 0;
}

void clearList()
{
    const ListState state = listStatus;
    if (state == ListState::Done || state == ListState::Failed)
        listStatus = ListState::Idle;
}

void deleteRoute(int slot)
{
    if (slot < 0 || slot >= kRouteSlots)
        return;
    pendingDeletes |= 1u << slot;
    wake();
}

bool startRouteDownload(int slot)
{
    const DownloadState state = downloadStatus;
    if (state == DownloadState::Queued || state == DownloadState::Running || slot < 0 || slot >= kRouteSlots)
        return false;
    downloadSlot = slot;
    downloadCancel = false;
    downloadReason = "";
    downloadDone = 0;
    downloadTotal = 0;
    downloadFailed = 0;
    downloadStatus = DownloadState::Queued;
    wake();
    return true;
}

DownloadProgress downloadProgress()
{
    return DownloadProgress{downloadStatus, downloadDone, downloadTotal, downloadFailed, downloadReason};
}

void cancelDownload()
{
    downloadCancel = true;
    if (fetcher)
        fetcher->poke();
}

void clearDownload()
{
    const DownloadState state = downloadStatus;
    if (state == DownloadState::Finished || state == DownloadState::Failed)
        downloadStatus = DownloadState::Idle;
}
#endif

} // namespace NicheGraphics::MapTiles::Fetch

#endif // BASEUI_MAP_ONLINE_TILES
