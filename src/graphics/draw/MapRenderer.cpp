#include "graphics/draw/MapRenderer.h"

#if BASEUI_HAS_MAP

#include "NodeDB.h"
#include "UptimeClock.h"
#include "WaypointStore.h"
#include "gps/GeoCoord.h"
#include "graphics/SharedUIDisplay.h"
#include "graphics/TFTColorRegions.h"
#include "graphics/TFTPalette.h"
#include "graphics/draw/MapRegionBounds.h"
#include "graphics/draw/MapViewPersistence.h"
#include "graphics/images.h"
#include "graphics/niche/Map/MapTileRenderer.h"
#include "mesh/Throttle.h"
#include "meshUtils.h"
#ifdef MESHTASTIC_ENCRYPTED_STORAGE
#include "security/EncryptedStorage.h"
#endif

#if defined(ARCH_PORTDUINO) || defined(ARCH_ESP32)
#include "graphics/niche/Map/MapTileSourceFile.h"
#endif
#if defined(HAS_SDCARD)
#include "graphics/niche/Map/MapTileSourceSD.h"
#endif
#if defined(SENSECAP_INDICATOR)
#include "graphics/niche/Map/MapTileSourceIndicator.h"
#endif
#if BASEUI_NATIVE_RGB565
#include "graphics/TFTDisplay.h"
#endif
#if BASEUI_MAP_PNG_TILES
#include "graphics/niche/Map/MapPngTiles.h"
#include <esp_heap_caps.h>
#endif

#include <algorithm>
#include <math.h>
#include <stdlib.h>
#include <string.h>

using namespace graphics;

namespace
{

constexpr float kEarthRadiusMeters = 6378137.0f;
constexpr int kDefaultZoomAlone = 6; // Sensible starting zoom when only our own node is known

int16_t s_lastViewWidth = 128;
int16_t s_lastViewHeight = 64;
#if BASEUI_MAP_ONSCREEN_CONTROLS
// Where the last draw put the map body, so a tap is tested against the buttons actually on screen.
int16_t s_lastViewTop = 0;
bool s_controlsOnScreen = false;
#endif

bool s_panMode = false;
bool s_zoomMode = false;
bool s_followMe = true; // mirrors uiconfig.map_data.follow_gps, adopted on first use
bool s_followMeLoaded = false;
bool s_followMePending = false;
float s_centerLat = 0;
float s_centerLng = 0;
bool s_centerInitialized = false;

int s_zoom = -1; // -1 = not yet initialized; set from the saved home, else auto-fit, on first use.

// Saving the view to uiconfig.map_data.home: MapViewPersistence.h decides what, saveView()'s callers when.
bool s_zoomSavePending = false;
uint32_t s_lastAutosaveMs = 0;

// Whole-region fallback, for when nothing at all is positioned. The zoom is dropped once something is.
bool s_lastCenterFromRegion = false;
bool s_zoomIsRegionFit = false;
int s_regionFitCode = -1;

float metersToPxForZoom(int zoom, float latDeg)
{
    float latRad = latDeg * DEG_TO_RAD;
    float mpp = (2.0f * (float)M_PI * kEarthRadiusMeters / (256.0f * (float)(1 << zoom))) * cosf(latRad);
    return mpp > 0.0f ? 1.0f / mpp : 0.0f;
}

// Web Mercator projection, in whole world pixels at `zoom` - the same convention the basemap
// tiles themselves are cut on (see MapTileRenderer's gpxX/gpxY), just kept in double here.
// A float mantissa is 24 bits, but the world is 256 * 2^18 = 67.1M px wide at kMaxZoom, so a
// float can't even represent adjacent world pixels at deep zoom - which is exactly the resolution
// the snapping below needs to be exact at.
constexpr double kWorldPxPerTile = 256.0;
constexpr double kMercatorLatLimit = 85.05112878; // Where the projection reaches the square world edge

double worldPxAtZoom(int zoom)
{
    return kWorldPxPerTile * (double)(1u << zoom);
}

void latLngToWorldPx(double latDeg, double lngDeg, int zoom, double *wx, double *wy)
{
    const double worldPx = worldPxAtZoom(zoom);
    if (latDeg > kMercatorLatLimit)
        latDeg = kMercatorLatLimit;
    if (latDeg < -kMercatorLatLimit)
        latDeg = -kMercatorLatLimit;
    const double s = sin(latDeg * (double)DEG_TO_RAD);
    *wx = (lngDeg + 180.0) / 360.0 * worldPx;
    *wy = (0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * M_PI)) * worldPx;
}

void worldPxToLatLng(double wx, double wy, int zoom, float *latDeg, float *lngDeg)
{
    const double worldPx = worldPxAtZoom(zoom);
    *lngDeg = (float)(wx / worldPx * 360.0 - 180.0);
    // Inverse of the y term above: atanh(sin(lat)) == asinh(tan(lat)), so lat == atan(sinh(...)).
    *latDeg = (float)(atan(sinh(M_PI * (1.0 - 2.0 * wy / worldPx))) * (double)RAD_TO_DEG);
}

// Rounds the view centre to the nearest whole world pixel at `zoom`, rewriting *lat/*lng to that
// snapped position and reporting it as integer world coordinates for the basemap cache key.
//
// Follow Me re-centres on the live GPS fix every single frame, and at z14+ (a couple of metres per
// screen pixel) the ordinary sub-metre jitter between consecutive fixes shifts the projected centre
// by a fraction of a pixel every time. Nothing on screen actually moves, but the float centre keeps
// differing, which would miss the rendered-basemap cache below on every frame and - worse - keeps
// nudging the node markers' colour regions across pixel boundaries, and TFTDisplay hashes those
// regions into the frame signature it uses to decide between a per-row diff and a full-panel
// repaint (see TFTDisplay::display). Snapping to the grid the basemap is drawn on anyway makes both
// stable: the view now only changes when it changes by something actually visible.
//
// Deliberately does not write the snapped value back into s_centerLat/s_centerLng. Those stay the
// true (unsnapped) pan/GPS position, so re-snapping them next frame is a pure function of an
// unchanged input and lands on the same pixel every time - whereas feeding a snapped value back in
// could round-trip across a half-pixel boundary and oscillate between two neighbouring pixels.
void snapCenterToPixelGrid(float *lat, float *lng, int zoom, int32_t *outWx, int32_t *outWy)
{
    const double worldPx = worldPxAtZoom(zoom);
    double wx, wy;
    latLngToWorldPx(*lat, *lng, zoom, &wx, &wy);

    wx = floor(wx + 0.5);
    wy = floor(wy + 0.5);

    wx = fmod(wx, worldPx); // Longitude wraps at the antimeridian...
    if (wx < 0)
        wx += worldPx;
    if (wy < 0) // ...latitude doesn't; it just stops at the projection's edge.
        wy = 0;
    if (wy > worldPx - 1)
        wy = worldPx - 1;

    *outWx = (int32_t)wx;
    *outWy = (int32_t)wy;
    worldPxToLatLng(wx, wy, zoom, lat, lng);
}

// Cartesian-average centroid of all known node positions - mirrors InkHUD MapApplet's default
// centering logic, so a sensible view exists even with nodes spread across a wide area.
bool computeNodeCentroid(float *lat, float *lng)
{
    uint32_t count = 0;
    float xAvg = 0, yAvg = 0, zAvg = 0;
    for (uint32_t i = 0; i < nodeDB->getNumMeshNodes(); i++) {
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
        if (!nodeDB->hasValidPosition(node))
            continue;
        meshtastic_PositionLite pos;
        if (!nodeDB->copyNodePosition(node->num, pos))
            continue;
        float latRad = pos.latitude_i * 1e-7f * DEG_TO_RAD;
        float lngRad = pos.longitude_i * 1e-7f * DEG_TO_RAD;
        xAvg += cosf(latRad) * cosf(lngRad);
        yAvg += cosf(latRad) * sinf(lngRad);
        zAvg += sinf(latRad);
        count++;
    }
    if (count == 0)
        return false;
    xAvg /= count;
    yAvg /= count;
    zAvg /= count;
    *lng = atan2f(yAvg, xAvg) * RAD_TO_DEG;
    *lat = atan2f(zAvg, sqrtf(xAvg * xAvg + yAvg * yAvg)) * RAD_TO_DEG;
    return true;
}

MapRegionBounds::View regionView(const MapRegionBounds::Bounds &bounds)
{
    return MapRegionBounds::fit(bounds, s_lastViewWidth, s_lastViewHeight, MapRenderer::kMinZoom, MapRenderer::kMaxZoom);
}

// device-ui writes the same field, so the two UIs agree on one setting. proto3 bools carry no presence, so a
// config that predates this defaults to off once anything else has written map_data.
void ensureFollowMeLoaded()
{
    if (s_followMeLoaded)
        return;
    s_followMeLoaded = true;
    if (uiconfig.has_map_data)
        s_followMe = uiconfig.map_data.follow_gps;
}

// A home either UI saved. 0,0 counts as none - it is what a zeroed field reads as.
bool haveSavedHome()
{
    return uiconfig.has_map_data && uiconfig.map_data.has_home &&
           (uiconfig.map_data.home.latitude != 0 || uiconfig.map_data.home.longitude != 0);
}

// Own node position if known, else our last known location from uiconfig, else the centroid of all
// known node positions, else the whole LoRa region the radio is set for.
//
// Deliberately reads the live `localPosition` global instead of going through
// nodeDB->copyNodePosition(ourNodeNum, ...): that call looks up our own entry in the
// nodePositions satellite table, which is only refreshed when we broadcast a position packet to
// the mesh, and can be stale (e.g. from a previous test location) even while `localPosition`
// itself is current. Using the stale table entry as "where I am" was landing Follow Me somewhere
// other than the actual current position.
bool computeAutoCenter(float *lat, float *lng)
{
    s_lastCenterFromRegion = false;
    if (localPosition.latitude_i != 0 || localPosition.longitude_i != 0) {
        *lat = localPosition.latitude_i * 1e-7f;
        *lng = localPosition.longitude_i * 1e-7f;
        return true;
    }
    // No fix yet this boot: centre where we last were. The self crosshair still waits for a real fix.
    if (haveSavedHome()) {
        *lat = uiconfig.map_data.home.latitude * 1e-7f;
        *lng = uiconfig.map_data.home.longitude * 1e-7f;
        return true;
    }
    if (computeNodeCentroid(lat, lng))
        return true;
    // Nothing positioned at all: frame the region rather than leave the frame empty.
    MapRegionBounds::Bounds bounds;
    if (!MapRegionBounds::regionBounds(config.lora.region, bounds))
        return false;
    const MapRegionBounds::View view = regionView(bounds);
    *lat = view.centerLat;
    *lng = view.centerLng;
    s_lastCenterFromRegion = true;
    return true;
}

// Highest zoom whose native scale still keeps the furthest known node within the viewport.
int computeAutoFitZoom(float centerLat, float centerLng, int16_t viewWidth, int16_t viewHeight)
{
    float maxEast = 0, maxNorth = 0;
    bool any = false;
    for (uint32_t i = 0; i < nodeDB->getNumMeshNodes(); i++) {
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
        if (!nodeDB->hasValidPosition(node))
            continue;
        meshtastic_PositionLite pos;
        if (!nodeDB->copyNodePosition(node->num, pos))
            continue;
        float lat = pos.latitude_i * 1e-7f;
        float lng = pos.longitude_i * 1e-7f;
        float distance = GeoCoord::latLongToMeter(centerLat, centerLng, lat, lng);
        float bearing = GeoCoord::bearing(centerLat, centerLng, lat, lng);
        float east = fabsf(sinf(bearing) * distance);
        float north = fabsf(cosf(bearing) * distance);
        if (east > maxEast)
            maxEast = east;
        if (north > maxNorth)
            maxNorth = north;
        any = true;
    }

    if (!any || (maxEast < 1.0f && maxNorth < 1.0f))
        return kDefaultZoomAlone;

    // Required scale so the furthest node stays within ~90% of the half-viewport.
    float requiredMetersToPx = 1e9f;
    if (maxEast > 0)
        requiredMetersToPx = min(requiredMetersToPx, (viewWidth * 0.45f) / maxEast);
    if (maxNorth > 0)
        requiredMetersToPx = min(requiredMetersToPx, (viewHeight * 0.45f) / maxNorth);

    for (int z = MapRenderer::kMaxZoom; z >= MapRenderer::kMinZoom; z--) {
        if (metersToPxForZoom(z, centerLat) <= requiredMetersToPx)
            return z;
    }
    return MapRenderer::kMinZoom;
}

void ensureZoomInitialized(float centerLat, float centerLng)
{
    // The region zoom only stood in for having nothing to show: re-fit once there is, or the region changes.
    if (s_zoomIsRegionFit && (!s_lastCenterFromRegion || s_regionFitCode != (int)config.lora.region)) {
        s_zoom = -1;
        s_zoomIsRegionFit = false;
    }
    if (s_zoom >= 0)
        return;
    MapRegionBounds::Bounds bounds;
    if (haveSavedHome()) { // the zoom last used, by this UI or device-ui
        s_zoom = clamp<int>(uiconfig.map_data.home.zoom, MapRenderer::kMinZoom, MapRenderer::kMaxZoom);
    } else if (s_lastCenterFromRegion && MapRegionBounds::regionBounds(config.lora.region, bounds)) {
        s_zoom = regionView(bounds).zoom;
        s_zoomIsRegionFit = true;
        s_regionFitCode = (int)config.lora.region;
    } else {
        s_zoom = computeAutoFitZoom(centerLat, centerLng, s_lastViewWidth, s_lastViewHeight);
    }
}

void ensureCenterInitialized()
{
    if (s_centerInitialized)
        return;
    float lat, lng;
    if (computeAutoCenter(&lat, &lng)) {
        s_centerLat = lat;
        s_centerLng = lng;
        s_centerInitialized = true;
    }
}

// Nudges the view by a fraction of the current viewport, expressed directly in degrees-per-pixel
// at the current zoom (the same Web Mercator tile convention the basemap itself uses) - not a
// great-circle distance/bearing calculation. This deliberately avoids GeoCoord::pointAtDistance:
// a single joystick press should always be "move a bit less than one screen", and computing that
// through real-world meters let a distant known node (dragging auto-fit zoom down) or a sign bug
// turn one press into a jump across the planet. Plain degrees-per-pixel can't do that - the worst
// case at any zoom is still bounded to a fixed fraction of one screen.
// Shared by both pan entry points below. Moves the viewport by an exact pixel offset, where +lngPx
// is east and +latPx is north.
static void panViewportByPixels(float lngPx, float latPx)
{
    ensureCenterInitialized();
    if (!s_centerInitialized)
        return;
    ensureFollowMeLoaded();
    if (s_followMe) { // Otherwise the next redraw would immediately snap back to our own position.
        s_followMe = false;
        s_followMePending = true;
    }
    ensureZoomInitialized(s_centerLat, s_centerLng);

    const float worldPxAtZoom = 256.0f * (float)(1 << s_zoom);
    const float degPerPxLng = 360.0f / worldPxAtZoom;
    const float degPerPxLat = degPerPxLng * cosf(s_centerLat * DEG_TO_RAD);

    s_centerLat += latPx * degPerPxLat;
    s_centerLng += lngPx * degPerPxLng;

    if (s_centerLat > 85.0f)
        s_centerLat = 85.0f;
    if (s_centerLat < -85.0f)
        s_centerLat = -85.0f;
    s_centerLng = fmodf(s_centerLng + 540.0f, 360.0f) - 180.0f; // Wrap to [-180, 180).
}

void panByScreenFraction(float dxFraction, float dyFraction)
{
    constexpr float kPanFractionOfView = 0.15f;
    const float stepPx = min(s_lastViewWidth, s_lastViewHeight) * kPanFractionOfView;
    panViewportByPixels(dxFraction * stepPx, dyFraction * stepPx);
}

#if defined(HAS_SDCARD)
// Real SD card (e.g. T-Deck): large, reliable, and provisioned by just copying MAP.BIN onto the
// card with any computer. Preferred over the plain FSCom file source below when a card is
// actually present.
bool ensureSDTileSourceInitialized()
{
    static bool attempted = false;
    static bool succeeded = false;
    static NicheGraphics::MapTiles::SDCardTileSource source;
    if (!attempted) {
        attempted = true;
        if (source.begin("/MAP.BIN")) {
            NicheGraphics::MapTiles::setTileSource(&source);
            succeeded = true;
        }
    }
    return succeeded;
}
#endif

#if defined(SENSECAP_INDICATOR)
// SenseCAP Indicator: the SD card is on the RP2040 co-processor, read over the interdevice link.
// Same MAP.BIN, same precedence over LittleFS as a directly-attached card.
bool ensureIndicatorTileSourceInitialized()
{
    static bool attempted = false;
    static bool succeeded = false;
    static NicheGraphics::MapTiles::IndicatorTileSource source;
    if (!attempted) {
        attempted = true;
        if (source.begin("/MAP.BIN")) {
            NicheGraphics::MapTiles::setTileSource(&source);
            succeeded = true;
        }
    }
    return succeeded;
}
#endif

#if defined(ARCH_PORTDUINO) || defined(ARCH_ESP32)
// On platforms with a filesystem that has room to spare (ESP32's LittleFS, or portduino's host
// filesystem passthrough), the basemap is just a normal file. Attempted once, lazily, on first
// draw; if MAP.BIN isn't present this quietly leaves MapTiles with zero tiles (the existing "no
// basemap baked in" fallback), which is the expected state until someone provisions a file there.
void ensureFileTileSourceInitialized()
{
#if defined(HAS_SDCARD)
    if (ensureSDTileSourceInitialized())
        return; // Card present and readable - don't also compete for LittleFS space.
#endif
#if defined(SENSECAP_INDICATOR)
    if (ensureIndicatorTileSourceInitialized())
        return; // Same, for the co-processor's card.
#endif
    static bool attempted = false;
    static NicheGraphics::MapTiles::FileTileSource source;
    if (attempted)
        return;
    attempted = true;
    if (source.begin("/MAP.BIN"))
        NicheGraphics::MapTiles::setTileSource(&source);
}
#endif

// Rendered-basemap cache: the tile background for one (snapped centre, zoom, viewport size) kept
// as a screen-sized 1bpp bitmap, so a view that hasn't moved repaints from RAM.
//
// drawMapFrame is a plain frame callback, so it re-runs on every ui->update(): once a second when
// idle, but at SCREEN_TRANSITION_FRAMERATE (30fps) while animating between frames, and again for
// every forced repaint - a button press, the header clock ticking over. Regenerating the tile
// background each of those times meant re-reading and re-LZ4-decompressing every tile in view,
// which is what made the Map frame stall the whole display task at deep zoom. A tile is 512 stored
// pixels but only 256 *screen* pixels wide, so any viewport wider than that spans two tile columns
// (and two rows), while MapTileRenderer's own cache holds exactly one decoded tile - and since the
// tiles are visited in the same order each frame, the one left in that slot is always the last one
// the next frame wants, i.e. it missed on every tile of every frame. Tiles at z14+ are dense enough
// that LZ4 barely compresses them, so each of those misses is a read of up to kTileBufferBytes.
//
// Caching the *rendered* result rather than the decoded tiles is both far smaller (a 1bpp viewport
// is a fraction of even one 32KB tile buffer) and covers the whole cost, decode and blit alike.
uint8_t *s_basemapBits = nullptr; // 1bpp, row-major, s_basemapStride bytes per row
size_t s_basemapCapacity = 0;
int16_t s_basemapStride = 0;
bool s_basemapValid = false;

// Identifies exactly what s_basemapBits currently holds. Anything that would change a single
// basemap pixel has to be in here.
struct BasemapKey {
    int32_t worldX, worldY; // Snapped centre, in whole world pixels at `zoom`
    int16_t zoom;
    int16_t width, height;
    bool haveTiles; // A tile source can appear after the first draw - see ensureFileTileSourceInitialized

    bool operator==(const BasemapKey &o) const
    {
        return worldX == o.worldX && worldY == o.worldY && zoom == o.zoom && width == o.width && height == o.height &&
               haveTiles == o.haveTiles;
    }
};
BasemapKey s_basemapKey{};

// Returns false if there's no room for the cache, in which case the caller falls back to rendering
// tiles straight into the display buffer (the original, uncached behaviour) rather than losing the
// basemap entirely.
bool ensureBasemapBuffer(int16_t viewWidth, int16_t viewHeight)
{
    if (viewWidth <= 0 || viewHeight <= 0)
        return false;

    const int16_t stride = (int16_t)((viewWidth + 7) / 8);
    const size_t needed = (size_t)stride * (size_t)viewHeight;

    if (!s_basemapBits || s_basemapCapacity < needed) {
        free(s_basemapBits);
        s_basemapBits = (uint8_t *)malloc(needed);
        s_basemapCapacity = s_basemapBits ? needed : 0;
        s_basemapValid = false;
        if (!s_basemapBits)
            return false;
    }
    if (stride != s_basemapStride) {
        s_basemapStride = stride; // Different row layout - whatever's in there no longer decodes.
        s_basemapValid = false;
    }
    return true;
}

struct BasemapPlotCtx {
    uint8_t *bits;
    int16_t stride, width, height;
};

void plotIntoBasemap(void *ctx, int16_t px, int16_t py)
{
    auto *c = static_cast<BasemapPlotCtx *>(ctx);
    if (px < 0 || px >= c->width || py < 0 || py >= c->height)
        return;
    c->bits[(size_t)py * c->stride + (px >> 3)] |= (uint8_t)(1 << (px & 7));
}

void blitBasemap(OLEDDisplay *display, int16_t offX, int16_t offY, int16_t viewWidth, int16_t viewHeight)
{
    for (int16_t py = 0; py < viewHeight; py++) {
        const uint8_t *row = s_basemapBits + (size_t)py * s_basemapStride;
        for (int16_t bx = 0; bx < s_basemapStride; bx++) {
            uint8_t bits = row[bx];
            if (!bits) // The common case by far on a 1bpp basemap - skip 8 pixels at a time.
                continue;
            const int16_t baseX = (int16_t)(bx * 8);
            for (int b = 0; b < 8; b++) {
                if (baseX + b >= viewWidth)
                    break;
                if (bits & (1 << b))
                    display->setPixel(offX + baseX + b, offY + py);
            }
        }
    }
}

#if BASEUI_MAP_PNG_TILES
// Colour counterpart of the basemap cache above for PNG tiles: the rendered viewport, native-endian RGB565.
uint16_t *s_colorBasemap = nullptr;
size_t s_colorBasemapCapacity = 0;
bool s_colorBasemapValid = false;
bool s_mapStylesScanned = false;
bool s_useBinaryMap = false; // MAP.BIN picked over the PNG tiles
// Saved in uiconfig for MAP.BIN. ':' can't appear in a FAT name, so no style folder can collide with it.
constexpr const char *kBinaryMapStyle = ":MAP.BIN";

bool savedStyleIsBinary()
{
    return uiconfig.has_map_data && strcmp(uiconfig.map_data.style, kBinaryMapStyle) == 0;
}

struct ColorBasemapKey {
    int32_t worldX, worldY;
    int16_t zoom, width, height;
    uint16_t background;
    uint32_t generation;

    bool operator==(const ColorBasemapKey &o) const
    {
        return worldX == o.worldX && worldY == o.worldY && zoom == o.zoom && width == o.width && height == o.height &&
               background == o.background && generation == o.generation;
    }
};
ColorBasemapKey s_colorBasemapKey{};

void ensureMapStylesScanned()
{
    if (s_mapStylesScanned)
        return;
    s_mapStylesScanned = true;
    s_useBinaryMap = savedStyleIsBinary();
    NicheGraphics::MapTiles::Png::refreshStyles((uiconfig.has_map_data && !s_useBinaryMap) ? uiconfig.map_data.style : nullptr);
}

// Draws the PNG basemap. False when MAP.BIN was picked (and is loaded) or the card has no PNG tiles, so the caller
// draws MAP.BIN instead.
bool drawColorBasemap(OLEDDisplay *display, int16_t offX, int16_t offY, int16_t viewWidth, int16_t viewHeight, int32_t worldX,
                      int32_t worldY, int zoom)
{
    namespace Png = NicheGraphics::MapTiles::Png;
    ensureMapStylesScanned();
    if ((s_useBinaryMap && NicheGraphics::MapTiles::hasTiles()) || Png::activeStyle() < 0 || viewWidth <= 0 || viewHeight <= 0)
        return false;

    const size_t needed = (size_t)viewWidth * (size_t)viewHeight * sizeof(uint16_t);
    if (s_colorBasemapCapacity < needed) {
        free(s_colorBasemap);
        s_colorBasemap = static_cast<uint16_t *>(heap_caps_malloc(needed, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        s_colorBasemapCapacity = s_colorBasemap ? needed : 0;
        s_colorBasemapValid = false;
        if (!s_colorBasemap)
            return false;
    }

    const ColorBasemapKey key{worldX, worldY, (int16_t)zoom, viewWidth, viewHeight, getThemeCanvasBg(), Png::generation()};
    if (!s_colorBasemapValid || !(s_colorBasemapKey == key)) {
#ifdef UI_PERF_DEBUG
        const uint32_t startMs = millis();
#endif
        Png::renderView(s_colorBasemap, viewWidth, viewHeight, worldX, worldY, zoom, key.background);
#ifdef UI_PERF_DEBUG
        LOG_INFO("map PNG basemap rebuild: %u ms, z%d", (unsigned)(millis() - startMs), zoom);
#endif
        s_colorBasemapKey = key;
        s_colorBasemapValid = true;
    }
    static_cast<TFTDisplay *>(display)->drawRGB565(offX, offY, viewWidth, viewHeight, s_colorBasemap);
    return true;
}
#endif

// Offsets used to build a 1px halo by blitting a glyph/icon 8 times before the real draw - see
// drawHaloXbm/drawHaloString below.
constexpr int8_t kHaloOffsets[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};

// Draws xbm/text with a solid WHITE halo behind a BLACK fill, so it stays readable against any
// part of the basemap (dense tile art, blank background, etc.) without depending on XOR/INVERSE
// against whatever's underneath. Replaces the old drawn-INVERSE approach, which read fine against
// any single background but caused overlapping elements to XOR-cancel back to background.
// Drawn at destW x destH, with the halo scaled to match (one halo pixel per source pixel of stretch).
#if !BASEUI_NATIVE_RGB565
void drawHaloXbm(OLEDDisplay *display, int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t *xbm, int16_t destW,
                 int16_t destH)
{
    const int16_t halo = std::max<int16_t>(1, destW / w);
    display->setColor(WHITE);
    for (auto &o : kHaloOffsets)
        graphics::drawStretchedXbm(display, x + o[0] * halo, y + o[1] * halo, w, h, xbm, destW, destH);
    display->setColor(BLACK);
    graphics::drawStretchedXbm(display, x, y, w, h, xbm, destW, destH);
}
#endif

#if BASEUI_NATIVE_RGB565
// Map pins, drawn exactly as painted: a black-edged white pin with a red (node) or green (waypoint) ring. Both share
// one mask, which also covers the black edge and fill.
constexpr int16_t kMapPinWidth = 16;
constexpr int16_t kMapPinHeight = 16;
const uint16_t nodeMarker_rgb565[] PROGMEM = {
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0xF800, 0xF800, 0x0000, 0x0000, 0xFFFF,
    0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0xF800, 0xF800, 0xF800, 0xF800, 0x0000,
    0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0xF800, 0xF800, 0x0000, 0x0000, 0xF800,
    0xF800, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0xF800, 0xF800, 0x0000, 0x0000,
    0xF800, 0xF800, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0xF800, 0xF800,
    0xF800, 0xF800, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000,
    0xF800, 0xF800, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF,
    0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0xFFFF, 0xFFFF, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000,
};
const uint16_t waypointMarker_rgb565[] PROGMEM = {
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x07E0, 0x07E0, 0x0000, 0x0000, 0xFFFF,
    0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x0000,
    0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x07E0, 0x07E0, 0x0000, 0x0000, 0x07E0,
    0x07E0, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x07E0, 0x07E0, 0x0000, 0x0000,
    0x07E0, 0x07E0, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x07E0, 0x07E0,
    0x07E0, 0x07E0, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000,
    0x07E0, 0x07E0, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF,
    0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0xFFFF, 0xFFFF, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000,
};
const uint8_t kMapPinMask[] PROGMEM = {
    0xE0, 0x07, 0xF0, 0x0F, 0xF8, 0x1F, 0xFC, 0x3F, 0xFC, 0x3F, 0xFC, 0x3F, 0xFC, 0x3F, 0xFC, 0x3F,
    0xFC, 0x3F, 0xF8, 0x1F, 0xF0, 0x0F, 0xE0, 0x07, 0xE0, 0x07, 0xC0, 0x03, 0xC0, 0x03, 0x80, 0x01,
};

// The pin's tip (bottom centre) sits on the position; each row goes out as runs of masked pixels.
void drawMapPin(OLEDDisplay *display, int16_t tipX, int16_t tipY, const uint16_t *pixels)
{
    TFTDisplay *const panel = static_cast<TFTDisplay *>(display);
    const int16_t left = tipX - kMapPinWidth / 2;
    const int16_t top = tipY - (kMapPinHeight - 1);
    const int16_t maskRowBytes = (kMapPinWidth + 7) / 8;
    for (int16_t row = 0; row < kMapPinHeight; ++row) {
        const uint8_t *maskRow = kMapPinMask + row * maskRowBytes;
        int16_t col = 0;
        while (col < kMapPinWidth) {
            while (col < kMapPinWidth && !(pgm_read_byte(maskRow + (col >> 3)) & (1U << (col & 7))))
                ++col;
            const int16_t runStart = col;
            while (col < kMapPinWidth && (pgm_read_byte(maskRow + (col >> 3)) & (1U << (col & 7))))
                ++col;
            if (col > runStart)
                panel->drawRGB565(left + runStart, top + row, col - runStart, 1, pixels + row * kMapPinWidth + runStart);
        }
    }
}
#endif

// On-screen size of a node/waypoint marker: the pin on colour builds, else the 1-bit ring at the icon scale.
int16_t mapMarkerSize()
{
#if BASEUI_NATIVE_RGB565
    return kMapPinWidth;
#else
    return 8 * BASEUI_ICON_SCALE;
#endif
}

void drawHaloString(OLEDDisplay *display, int16_t x, int16_t y, const char *text)
{
    display->setColor(WHITE);
    for (auto &o : kHaloOffsets)
        display->drawString(x + o[0], y + o[1], text);
    display->setColor(BLACK);
    display->drawString(x, y, text);
}

#if GRAPHICS_TFT_COLORING_ENABLED && !BASEUI_NATIVE_RGB565
// Colour screens tint only the 2x2 dot at the centre of each node marker red. Everything else -
// the marker ring, its halo, and the name labels - is left exactly as the monochrome drawing above
// produces it (black glyph, white halo).
//
// icon_map_node is a ring whose centre dot is the middle quarter of its box (2x2 of 8x8, 4x4 of the 16x16 colour
// version), so a marker of size s centred on (mx, my) has its dot at (mx - s/8, my - s/8), s/4 across. drawHaloXbm
// renders the glyph itself with BLACK, i.e. as *cleared* pixels, so the region maps unset -> red to catch the dot.
// set -> white matches what TFTDisplay already paints set pixels with, so the surrounding halo is unaffected.
//
// colorRegions[] is a fixed global pool shared with the header, and it silently evicts the oldest
// entry once full, so marker tints get everything the pool has left after a reserve for this
// screen's own chrome (header background/title/status, footer link icon, nav bar + arrows).
// Markers past the budget still draw, just with a black centre, rather than pushing those out.
//
// Derived from MAX_TFT_COLOR_REGIONS rather than hardcoded, so growing the pool actually reaches
// the markers - a fixed 24 here silently stayed the binding limit when the pool went 48 -> 255.
// Small pools (the nRF52840 TFT boards) keep the original 24 instead of dropping below it.
constexpr int kChromeColorRegionReserve = 32;
constexpr int kMaxNodeColorRegions =
    (MAX_TFT_COLOR_REGIONS > kChromeColorRegionReserve + 24) ? (int)MAX_TFT_COLOR_REGIONS - kChromeColorRegionReserve : 24;

void tintMarkerCenter(int16_t centerX, int16_t centerY, int16_t markerSize, int &budget, uint16_t centerColor = TFTPalette::Red)
{
    if (budget <= 0)
        return;
    const int16_t dot = markerSize / 4;
    registerTFTColorRegionDirect(centerX - markerSize / 8, centerY - markerSize / 8, dot, dot, TFTPalette::White, centerColor);
    budget--;
}
#endif

#if BASEUI_MAP_ONSCREEN_CONTROLS
// A column down the right edge: the header owns the top, the nav bar sweeps the bottom on every frame change, and
// on a rounded panel mid-height is the widest part of the glass.
enum class MapControl : uint8_t { ZoomIn, ZoomOut, Pan, FollowMe, Count };
constexpr int kMapControlCount = (int)MapControl::Count;
constexpr int16_t kMapControlRadius = 7; // the keyboard's key caps

struct MapControlRect {
    int16_t x, y, w, h;
};

// Shared by the draw and the hit test, so a button can't be drawn anywhere other than where it is pressed.
void layoutMapControls(int16_t x, int16_t y, int16_t viewWidth, int16_t viewHeight, MapControlRect out[kMapControlCount])
{
    constexpr int16_t gap = 4;
    const int16_t w = std::max<int16_t>(34, viewWidth / 7);
    const int16_t h = std::max<int16_t>(24, viewHeight / 10);
    const int16_t inset = std::max<int16_t>(2, viewWidth / 40);
    const int16_t left = x + viewWidth - inset - w;
    const int16_t stackHeight = kMapControlCount * h + (kMapControlCount - 1) * gap;
    const int16_t top = std::max<int16_t>(y, y + (viewHeight - stackHeight) / 2);

    for (int i = 0; i < kMapControlCount; ++i)
        out[i] = {left, (int16_t)(top + i * (h + gap)), w, h};
}

void drawMapControls(OLEDDisplay *display, int16_t x, int16_t y, int16_t viewWidth, int16_t viewHeight)
{
    MapControlRect rects[kMapControlCount];
    layoutMapControls(x, y, viewWidth, viewHeight, rects);

    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_CENTER);

    for (int i = 0; i < kMapControlCount; ++i) {
        const MapControlRect &r = rects[i];
        const MapControl control = (MapControl)i;
        // The toggles stay filled while on, like the keyboard's shift key; the zoom steps never fill.
        const bool latched = (control == MapControl::Pan && s_panMode) || (control == MapControl::FollowMe && s_followMe);

        // Always filled, never a bare outline, or the tile art shows straight through the cap.
        if (latched) {
            display->setColor(WHITE);
            fillRoundedRect(display, r.x, r.y, r.w, r.h, kMapControlRadius);
            display->setColor(BLACK);
        } else {
            display->setColor(BLACK);
            fillRoundedRect(display, r.x, r.y, r.w, r.h, kMapControlRadius);
            display->setColor(WHITE);
            drawRoundedRect(display, r.x, r.y, r.w, r.h, kMapControlRadius);
        }

        const int16_t cx = r.x + r.w / 2;
        const int16_t cy = r.y + r.h / 2;
        // Strokes rather than glyphs: '+' and '-' are small and off-centre in most faces.
        const int16_t arm = std::min<int16_t>(r.w, r.h) / 4;
        const int16_t stroke = std::max<int16_t>(2, r.h / 12);
        switch (control) {
        case MapControl::ZoomIn:
            display->fillRect(cx - arm, cy - stroke / 2, arm * 2, stroke);
            display->fillRect(cx - stroke / 2, cy - arm, stroke, arm * 2);
            break;
        case MapControl::ZoomOut:
            display->fillRect(cx - arm, cy - stroke / 2, arm * 2, stroke);
            break;
        default:
            display->drawString(cx, r.y + (r.h - FONT_HEIGHT_SMALL) / 2, control == MapControl::Pan ? "PAN" : "ME");
            break;
        }

        display->setColor(WHITE);
    }

    display->setTextAlignment(TEXT_ALIGN_LEFT);
}
#endif // BASEUI_MAP_ONSCREEN_CONTROLS

} // namespace

bool MapRenderer::isPanModeEnabled()
{
    return s_panMode;
}

void MapRenderer::setPanModeEnabled(bool enabled)
{
    s_panMode = enabled;
    if (enabled)
        s_zoomMode = false; // Mutually exclusive - up/down/left/right can't mean both at once.
}

void MapRenderer::panUp()
{
    panByScreenFraction(0.0f, 1.0f);
}

void MapRenderer::panDown()
{
    panByScreenFraction(0.0f, -1.0f);
}

void MapRenderer::panLeft()
{
    panByScreenFraction(-1.0f, 0.0f);
}

void MapRenderer::panRight()
{
    panByScreenFraction(1.0f, 0.0f);
}

void MapRenderer::panByFingerDelta(float dxPx, float dyPx)
{
    // The map travels with the finger, so the viewport moves the opposite way: dragging the map
    // rightwards reveals what was off the left edge, which is further west. Screen y grows
    // downward, so dragging down already walks the viewport north - hence only the x term is
    // negated.
    //
    // Deliberately the opposite sense to panLeft()/panRight() above: a joystick press means "move
    // the view that way", where a finger means "move the map that way".
    panViewportByPixels(-dxPx, dyPx);
}

bool MapRenderer::isFollowMeEnabled()
{
    ensureFollowMeLoaded();
    return s_followMe;
}

void MapRenderer::setFollowMeEnabled(bool enabled)
{
    ensureFollowMeLoaded();
    if (s_followMe != enabled)
        s_followMePending = true;
    s_followMe = enabled;
    if (!enabled)
        ensureCenterInitialized(); // Freezing the view - make sure there's somewhere concrete to freeze it.
}

bool MapRenderer::isZoomModeEnabled()
{
    return s_zoomMode;
}

void MapRenderer::setZoomModeEnabled(bool enabled)
{
    s_zoomMode = enabled;
    if (enabled) {
        s_panMode = false; // Mutually exclusive with Pan Mode.
        ensureZoomInitialized(s_centerInitialized ? s_centerLat : 0.0f, s_centerInitialized ? s_centerLng : 0.0f);
    }
}

int MapRenderer::zoom()
{
    return s_zoom < 0 ? kDefaultZoomAlone : s_zoom;
}

void MapRenderer::setZoom(int zoom)
{
    if (zoom < kMinZoom)
        zoom = kMinZoom;
    if (zoom > kMaxZoom)
        zoom = kMaxZoom;
    if (zoom == s_zoom)
        return;
    s_zoom = zoom;
    s_zoomIsRegionFit = false; // a zoom the user picked stands, even over the region view
    s_zoomSavePending = true;  // only a user's choice is saved, never auto-fit
}

void MapRenderer::zoomIn()
{
    setZoom(zoom() + 1);
}

void MapRenderer::zoomOut()
{
    setZoom(zoom() - 1);
}

#ifndef MAP_VIEW_AUTOSAVE_INTERVAL_SEC
#define MAP_VIEW_AUTOSAVE_INTERVAL_SEC (2 * 60 * 60) // the message and waypoint stores' cadence
#endif

void MapRenderer::saveView()
{
    namespace Persist = MapViewPersistence;
    const bool haveLive = localPosition.latitude_i != 0 || localPosition.longitude_i != 0;
    const bool haveSaved = haveSavedHome();

    Persist::Inputs in{};
    in.zoomPending = s_zoomSavePending;
    in.followMePending = s_followMePending;
    in.haveLivePosition = haveLive;
    in.haveSavedLocation = haveSaved;
    if (haveLive && haveSaved)
        in.metersFromSaved =
            GeoCoord::latLongToMeter(localPosition.latitude_i * 1e-7, localPosition.longitude_i * 1e-7,
                                     uiconfig.map_data.home.latitude * 1e-7, uiconfig.map_data.home.longitude * 1e-7);
#ifdef MESHTASTIC_ENCRYPTED_STORAGE
    in.locationAllowed = !EncryptedStorage::isLockdownActive();
#else
    in.locationAllowed = true;
#endif

    const Persist::Decision decision = Persist::decide(in);
    if (!decision.write)
        return;

    uiconfig.has_map_data = true;
    uiconfig.map_data.has_home = true;
    if (decision.writeLocation) {
        uiconfig.map_data.home.latitude = localPosition.latitude_i;
        uiconfig.map_data.home.longitude = localPosition.longitude_i;
    }
    if (decision.writeZoom)
        uiconfig.map_data.home.zoom = (int8_t)zoom();
    if (decision.writeFollowMe)
        uiconfig.map_data.follow_gps = s_followMe;
    if (!nodeDB->saveProto(uiconfigFileName, meshtastic_DeviceUIConfig_size, &meshtastic_DeviceUIConfig_msg, &uiconfig))
        return; // the zoom stays pending for the next save
    if (decision.writeZoom)
        s_zoomSavePending = false;
    if (decision.writeFollowMe)
        s_followMePending = false;
}

void MapRenderer::autosaveTick()
{
    if (s_lastAutosaveMs == 0) { // start the interval at boot, as WaypointStore does, rather than save on the first tick
        s_lastAutosaveMs = Time::getMillis();
        return;
    }
    Throttle::execute(&s_lastAutosaveMs, (uint32_t)MAP_VIEW_AUTOSAVE_INTERVAL_SEC * 1000UL, MapRenderer::saveView);
}

#if BASEUI_MAP_PNG_TILES
static_assert(MapRenderer::kMaxMapStyles == NicheGraphics::MapTiles::Png::kMaxStyles, "style list sizes must match");

int MapRenderer::refreshMapStyles()
{
    namespace Png = NicheGraphics::MapTiles::Png;
#if defined(ARCH_PORTDUINO) || defined(ARCH_ESP32)
    ensureFileTileSourceInitialized(); // so MAP.BIN is known even if the Map frame hasn't drawn yet
#endif
    if (!s_mapStylesScanned)
        s_useBinaryMap = savedStyleIsBinary();
    s_mapStylesScanned = true;
    const int current = Png::activeStyle();
    const int pngCount = (current >= 0)
                             ? Png::refreshStyles(Png::styleName(current))
                             : Png::refreshStyles((uiconfig.has_map_data && !s_useBinaryMap) ? uiconfig.map_data.style : nullptr);
    return pngCount + (NicheGraphics::MapTiles::hasTiles() ? 1 : 0);
}

const char *MapRenderer::mapStyleLabel(int index)
{
    namespace Png = NicheGraphics::MapTiles::Png;
    if (index == Png::styleCount())
        return "MAP.BIN";
    const char *name = Png::styleName(index);
    return name[0] ? name : "map";
}

int MapRenderer::activeMapStyle()
{
    namespace Png = NicheGraphics::MapTiles::Png;
    // MAP.BIN is what draws when it was picked, or when there are no PNG tiles to prefer over it.
    if (NicheGraphics::MapTiles::hasTiles() && (s_useBinaryMap || Png::activeStyle() < 0))
        return Png::styleCount();
    return Png::activeStyle();
}

void MapRenderer::setMapStyle(int index)
{
    namespace Png = NicheGraphics::MapTiles::Png;
    s_useBinaryMap = (index == Png::styleCount());
    if (!s_useBinaryMap)
        Png::setActiveStyle(index);
    uiconfig.has_map_data = true;
    strncpy(uiconfig.map_data.style, s_useBinaryMap ? kBinaryMapStyle : Png::styleName(index),
            sizeof(uiconfig.map_data.style) - 1);
    uiconfig.map_data.style[sizeof(uiconfig.map_data.style) - 1] = '\0';
}
#endif

void MapRenderer::drawMapFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
#if defined(ARCH_PORTDUINO) || defined(ARCH_ESP32)
    ensureFileTileSourceInitialized();
#endif

    graphics::clearForFrame(display, state);
    // WHITE is the lit/visible pixel color on OLEDDisplay (unlike InkHUD's e-ink convention,
    // where BLACK means ink) - everything below needs to actually show up on real hardware.
    display->setColor(WHITE);

    int16_t viewWidth = display->getWidth();
    int16_t viewHeight = display->getHeight();

    // Shared battery/time header, same as every other BaseUI screen - reserve its height and
    // shift the map viewport down so tiles/markers/overlays never draw underneath it.
    // Matches drawCommonHeader's own internal footprint exactly (SharedUIDisplay.cpp: headerHeight
    // = highlightHeight + 2, highlightHeight = FONT_HEIGHT_SMALL - 1 + BASEUI_HEADER_MARGIN, so
    // FONT_HEIGHT_SMALL + 1 + BASEUI_HEADER_MARGIN). Deliberately no added BASEUI_BELOW_HEADER_MARGIN
    // gap here (unlike NodeListRenderer/UIRenderer) - the map should hug the header exactly, not
    // leave list-style breathing room. BASEUI_HEADER_MARGIN is 0 on every board except the ones
    // (like t-watch-ultra) that define it, so this is a no-op elsewhere.
    const int16_t kHeaderHeight = FONT_HEIGHT_SMALL + 1 + BASEUI_HEADER_MARGIN;
    drawCommonHeader(display, x, y, "Map");
    display->setColor(WHITE); // drawCommonHeader leaves its own color state active
    y += kHeaderHeight;
    viewHeight -= kHeaderHeight;

    ensureFollowMeLoaded();
    s_lastViewWidth = viewWidth;
    s_lastViewHeight = viewHeight;
#if BASEUI_MAP_ONSCREEN_CONTROLS
    s_lastViewTop = y;
    s_controlsOnScreen = false; // raised once they are actually drawn, so an early return leaves nothing to tap
#endif

    float centerLat, centerLng;
    bool haveCenter;
    if (s_followMe) {
        haveCenter = computeAutoCenter(&centerLat, &centerLng);
        // Keep s_center in sync with whatever is actually on screen, so that the *first* pan
        // (which flips Follow Me off) starts from exactly this position instead of separately
        // recomputing auto-center at that later moment and potentially landing somewhere else.
        if (haveCenter) {
            s_centerLat = centerLat;
            s_centerLng = centerLng;
            s_centerInitialized = true;
        }
    } else {
        ensureCenterInitialized();
        centerLat = s_centerLat;
        centerLng = s_centerLng;
        haveCenter = s_centerInitialized;
    }

    if (!haveCenter) {
        display->setTextAlignment(TEXT_ALIGN_CENTER);
        display->setFont(FONT_SMALL);
        display->drawString(x + viewWidth / 2, y + viewHeight / 2 - FONT_HEIGHT_SMALL / 2, "No node positions yet");
        return;
    }

    ensureZoomInitialized(centerLat, centerLng);
    const int zoom = s_zoom;

    // Round the centre onto the basemap's own pixel grid before anything is projected from it, so
    // that sub-pixel GPS jitter can't move the view - see snapCenterToPixelGrid. Everything below
    // (basemap, markers, the self crosshair, the coordinate label) then works from the snapped
    // centre, so they all stay pinned to each other and to the tiles.
    int32_t snappedWorldX = 0, snappedWorldY = 0;
    snapCenterToPixelGrid(&centerLat, &centerLng, zoom, &snappedWorldX, &snappedWorldY);

    const float metersToPx = metersToPxForZoom(zoom, centerLat);

    struct PlotCtx {
        OLEDDisplay *display;
        int16_t offX, offY;
    } plotCtx{display, x, y};

    const BasemapKey basemapKey{snappedWorldX, snappedWorldY, (int16_t)zoom,
                                viewWidth,     viewHeight,    NicheGraphics::MapTiles::hasTiles()};

    bool colorBasemap = false;
#if BASEUI_MAP_PNG_TILES
    colorBasemap = drawColorBasemap(display, x, y, viewWidth, viewHeight, snappedWorldX, snappedWorldY, zoom);
#endif

    if (colorBasemap) {
        // PNG tiles are already on screen.
    } else if (ensureBasemapBuffer(viewWidth, viewHeight)) {
        if (!s_basemapValid || !(s_basemapKey == basemapKey)) {
            memset(s_basemapBits, 0, (size_t)s_basemapStride * (size_t)viewHeight);
            BasemapPlotCtx basemapCtx{s_basemapBits, s_basemapStride, viewWidth, viewHeight};
#ifdef UI_PERF_DEBUG
            const uint32_t tileStartMs = millis();
#endif
            NicheGraphics::MapTiles::drawTileBackground(centerLat, centerLng, zoom, metersToPx, viewWidth, viewHeight,
                                                        plotIntoBasemap, &basemapCtx);
#ifdef UI_PERF_DEBUG
            // Only logged on a basemap miss, which is every frame while panning - the key includes
            // the centre, so any movement at all rebuilds the whole viewport from tiles.
            uint32_t tileDecodes = 0, tileCacheHits = 0;
            NicheGraphics::MapTiles::lastTileStats(&tileDecodes, &tileCacheHits);
            LOG_INFO("map basemap rebuild: %u ms, z%d, %u tile decodes, %u cache hits", (unsigned)(millis() - tileStartMs), zoom,
                     (unsigned)tileDecodes, (unsigned)tileCacheHits);
#endif
            s_basemapKey = basemapKey;
            s_basemapValid = true;
        }
        blitBasemap(display, x, y, viewWidth, viewHeight);
    } else {
        // No room for the cache - render tiles straight into the display buffer, as before.
        NicheGraphics::MapTiles::drawTileBackground(
            centerLat, centerLng, zoom, metersToPx, viewWidth, viewHeight,
            [](void *ctx, int16_t px, int16_t py) {
                auto *c = static_cast<PlotCtx *>(ctx);
                c->display->setPixel(c->offX + px, c->offY + py);
            },
            &plotCtx);
    }

    // Known node markers (self is drawn separately, last, so it's always on top).
    const NodeNum ourNodeNum = nodeDB->getNodeNum();

    // Everything below is drawn with drawHaloXbm/drawHaloString (solid WHITE halo behind a BLACK
    // fill) rather than a fixed color or XOR/INVERSE, so markers, labels, and both text overlays
    // stay readable no matter what part of the basemap they land on. Markers still dedupe against
    // nearby already-drawn marker positions below - not needed for correctness anymore (halo draws
    // don't cancel out like XOR did), but it still avoids wasted draws and visual clutter when many
    // nodes collapse onto nearly the same screen position at low zoom.
    const int16_t markerSize = mapMarkerSize();
    const int16_t markerDedupeRadius = markerSize / 2;
#if BASEUI_NATIVE_RGB565
    const int16_t markerHeadDy = -(markerSize / 2 + 1); // pins stand above their position; labels sit by the head
#else
    constexpr int16_t markerHeadDy = 0;
#endif
    constexpr int kMaxDedupeTracked = 64;
    int16_t drawnMx[kMaxDedupeTracked];
    int16_t drawnMy[kMaxDedupeTracked];
    int drawnCount = 0;

    // Short-name labels are skipped only when they'd actually overlap a label already placed -
    // no fixed cap on how many can show at once, so zooming out keeps names visible as long as
    // there's room for them.
    constexpr int kMaxLabelsTracked = 64;
    int16_t labelX[kMaxLabelsTracked];
    int16_t labelY[kMaxLabelsTracked];
    int16_t labelW[kMaxLabelsTracked];
    int16_t labelH[kMaxLabelsTracked];
    int labelCount = 0;

#if GRAPHICS_TFT_COLORING_ENABLED && !BASEUI_NATIVE_RGB565
    // Waypoints draw after the nodes, so a busy mesh would spend the whole budget first and leave them black-centred,
    // indistinguishable from nodes. Reserve them a slice up front.
#if MESHTASTIC_EXCLUDE_WAYPOINT
    constexpr int kWaypointColorRegions = 0;
#else
    constexpr int kWaypointColorRegions =
        (WAYPOINT_HISTORY_LIMIT < kMaxNodeColorRegions / 4) ? WAYPOINT_HISTORY_LIMIT : kMaxNodeColorRegions / 4;
    int waypointColorRegions = kWaypointColorRegions; // green waypoint centres
#endif
    int nodeColorRegions = kMaxNodeColorRegions - kWaypointColorRegions; // red node centres
#endif

    // FONT_SMALL_LOCAL rather than FONT_SMALL deliberately: on TFT/HAS_SPI_TFT builds FONT_SMALL is
    // redirected to the 19px-tall medium font (bigger screen, so BaseUI normally wants bigger text)
    // - far too large for a map label sitting next to a marker, where many names need to fit close
    // together without overlapping.
    display->setFont(FONT_SMALL_LOCAL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    const int16_t labelHeight = _fontHeight(FONT_SMALL_LOCAL);

    // Draws a label unless it would land on one already placed. Shared by node and waypoint markers, so their names
    // respect each other instead of stacking.
    auto placeLabel = [&](int16_t lx, int16_t ly, const char *text) {
        const int16_t lw = (int16_t)display->getStringWidth(text);
        for (int li = 0; li < labelCount; li++) {
            if (lx < labelX[li] + labelW[li] && lx + lw > labelX[li] && ly < labelY[li] + labelH[li] &&
                ly + labelHeight > labelY[li])
                return;
        }

        drawHaloString(display, lx, ly, text);
        if (labelCount < kMaxLabelsTracked) {
            labelX[labelCount] = lx;
            labelY[labelCount] = ly;
            labelW[labelCount] = lw;
            labelH[labelCount] = labelHeight;
            labelCount++;
        }
    };

    for (uint32_t i = 0; i < nodeDB->getNumMeshNodes(); i++) {
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
        if (!nodeDB->hasValidPosition(node) || node->num == ourNodeNum)
            continue;
        meshtastic_PositionLite pos;
        if (!nodeDB->copyNodePosition(node->num, pos))
            continue;

        float lat = pos.latitude_i * 1e-7f;
        float lng = pos.longitude_i * 1e-7f;
        float distance = GeoCoord::latLongToMeter(centerLat, centerLng, lat, lng);
        float bearing = GeoCoord::bearing(centerLat, centerLng, lat, lng);
        float northMeters = cosf(bearing) * distance;
        float eastMeters = sinf(bearing) * distance;

        int16_t mx = x + viewWidth / 2 + (int16_t)(eastMeters * metersToPx);
        int16_t my = y + viewHeight / 2 - (int16_t)(northMeters * metersToPx);
        if (mx < x - 2 || mx > x + viewWidth + 1 || my < y - 2 || my > y + viewHeight + 1)
            continue;

        bool tooClose = false;
        for (int d = 0; d < drawnCount; d++) {
            if (abs(drawnMx[d] - mx) <= markerDedupeRadius && abs(drawnMy[d] - my) <= markerDedupeRadius) {
                tooClose = true;
                break;
            }
        }
        if (tooClose)
            continue;
        if (drawnCount < kMaxDedupeTracked) {
            drawnMx[drawnCount] = mx;
            drawnMy[drawnCount] = my;
            drawnCount++;
        }

#if BASEUI_NATIVE_RGB565
        drawMapPin(display, mx, my, nodeMarker_rgb565);
#else
        drawHaloXbm(display, mx - markerSize / 2, my - markerSize / 2, 8, 8, icon_map_node, markerSize, markerSize);
#if GRAPHICS_TFT_COLORING_ENABLED
        tintMarkerCenter(mx, my, markerSize, nodeColorRegions);
#endif
#endif

        if (node->short_name[0] != '\0')
            placeLabel(mx + markerSize / 2 + 1, my + markerHeadDy - labelHeight / 2, node->short_name);
    }

    // Stored waypoints: the node marker in green, after the nodes and before the self crosshair. The same
    // non-expired filter as the waypoint screen, and not deduped against nodes - a waypoint on a node still shows.
#if !MESHTASTIC_EXCLUDE_WAYPOINT
    for (const StoredWaypoint &entry : waypointStore.getWaypoints()) {
        const meshtastic_Waypoint &wp = entry.waypoint;
        if (WaypointStore::isExpired(entry))
            continue;
        if (!wp.has_latitude_i || !wp.has_longitude_i || (wp.latitude_i == 0 && wp.longitude_i == 0))
            continue;

        const float wlat = wp.latitude_i * 1e-7f;
        const float wlng = wp.longitude_i * 1e-7f;
        const float wdistance = GeoCoord::latLongToMeter(centerLat, centerLng, wlat, wlng);
        const float wbearing = GeoCoord::bearing(centerLat, centerLng, wlat, wlng);
        const int16_t wx = x + viewWidth / 2 + (int16_t)(sinf(wbearing) * wdistance * metersToPx);
        const int16_t wy = y + viewHeight / 2 - (int16_t)(cosf(wbearing) * wdistance * metersToPx);
        if (wx < x - 2 || wx > x + viewWidth + 1 || wy < y - 2 || wy > y + viewHeight + 1)
            continue;

#if BASEUI_NATIVE_RGB565
        drawMapPin(display, wx, wy, waypointMarker_rgb565);
#else
        drawHaloXbm(display, wx - markerSize / 2, wy - markerSize / 2, 8, 8, icon_map_node, markerSize, markerSize);
#if GRAPHICS_TFT_COLORING_ENABLED
        tintMarkerCenter(wx, wy, markerSize, waypointColorRegions, TFTPalette::MeshtasticGreen);
#endif
#endif
        if (wp.name[0] != '\0')
            placeLabel(wx + markerSize / 2 + 1, wy + markerHeadDy - labelHeight / 2, wp.name);
    }
#endif

    // Self marker: crosshair, drawn last so it's always visible when on-screen. Uses the live
    // `localPosition` global (see computeAutoCenter's comment) rather than nodeDB->copyNodePosition,
    // so the crosshair always lands exactly on the same position Follow Me centered on.
    if (localPosition.latitude_i != 0 || localPosition.longitude_i != 0) {
        float lat = localPosition.latitude_i * 1e-7f;
        float lng = localPosition.longitude_i * 1e-7f;
        float distance = GeoCoord::latLongToMeter(centerLat, centerLng, lat, lng);
        float bearingRad = GeoCoord::bearing(centerLat, centerLng, lat, lng);
        float northMeters = cosf(bearingRad) * distance;
        float eastMeters = sinf(bearingRad) * distance;

        int16_t sx = x + viewWidth / 2 + (int16_t)(eastMeters * metersToPx);
        int16_t sy = y + viewHeight / 2 - (int16_t)(northMeters * metersToPx);
        if (sx >= x && sx <= x + viewWidth && sy >= y && sy <= y + viewHeight) {
            // Plain crosshair: circle with full-length lines crossing straight through it. The
            // previous version kept the ticks detached from the circle to dodge a XOR-cancellation
            // artifact from the old INVERSE draw mode; now that everything below draws with a solid
            // WHITE halo instead of XOR, overlapping strokes just paint over each other normally, so
            // there's no reason not to draw the crosshair the straightforward way.
            constexpr int16_t kRadius = 3;
            constexpr int16_t kOuter = 9;
            display->setColor(WHITE);
            display->fillCircle(sx, sy, kRadius + 2);
            // Halo the arms too (1px above/below and left/right), so they stay visible past the
            // edge of the bullseye's halo circle, same as the labels/icons above.
            display->drawLine(sx - kOuter, sy - 1, sx + kOuter, sy - 1);
            display->drawLine(sx - kOuter, sy + 1, sx + kOuter, sy + 1);
            display->drawLine(sx - 1, sy - kOuter, sx - 1, sy + kOuter);
            display->drawLine(sx + 1, sy - kOuter, sx + 1, sy + kOuter);
            display->setColor(BLACK);
            display->drawCircle(sx, sy, kRadius);
            display->drawLine(sx - kOuter, sy, sx + kOuter, sy);
            display->drawLine(sx, sy - kOuter, sx, sy + kOuter);
        }
    }

    // Center coordinates - a concrete reference for "where am I", especially useful
    // before any basemap tiles are baked in, or after panning away from every known node.
    // Rounded panels clip their corners, so both labels are centred on those instead of
    // being pinned to a corner.
    display->setFont(FONT_SMALL);
    char coordLabel[24];
    snprintf(coordLabel, sizeof(coordLabel), "%.4f,%.4f", centerLat, centerLng);
    char statusLabel[24];
    snprintf(statusLabel, sizeof(statusLabel), "z%d%s%s%s", zoom, s_panMode ? " PAN" : "", s_zoomMode ? " ZOOM" : "",
             s_followMe ? " ME" : "");

#if ROUNDED_SCREEN
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    drawHaloString(display, x + viewWidth / 2, y, coordLabel);
    drawHaloString(display, x + viewWidth / 2, y + viewHeight - FONT_HEIGHT_SMALL - 1, statusLabel);
#else
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    drawHaloString(display, x + 1, y, coordLabel);

    // Status label, bottom-right corner: zoom level, plus mode/follow indicators.
    display->setTextAlignment(TEXT_ALIGN_RIGHT);
    drawHaloString(display, x + viewWidth - 2, y + viewHeight - FONT_HEIGHT_SMALL - 1, statusLabel);
#endif

    // Zoom ruler: shown only while Zoom Mode is active (entered from the menu, held until
    // Back/Cancel). A vertical gauge on the right edge, current level marked, so up/down's effect
    // is visible at a glance without needing to read the numeric label. Haloed the same as
    // everything else above, so it stays readable over whatever basemap tile art is behind it.
    if (s_zoomMode) {
        constexpr int16_t kMargin = 10;
        const int16_t rulerX = x + viewWidth - 6;
        const int16_t rulerTop = y + kMargin;
        const int16_t rulerBottom = y + viewHeight - kMargin;

        const float frac = (float)(zoom - kMinZoom) / (float)(kMaxZoom - kMinZoom);
        const int16_t indicatorY = rulerBottom - (int16_t)(frac * (rulerBottom - rulerTop));

        display->setColor(WHITE);
        display->drawLine(rulerX - 1, rulerTop, rulerX - 1, rulerBottom);
        display->drawLine(rulerX + 1, rulerTop, rulerX + 1, rulerBottom);
        display->drawLine(rulerX - 4, rulerTop - 1, rulerX + 4, rulerTop - 1);
        display->drawLine(rulerX - 4, rulerTop + 1, rulerX + 4, rulerTop + 1);
        display->drawLine(rulerX - 4, rulerBottom - 1, rulerX + 4, rulerBottom - 1);
        display->drawLine(rulerX - 4, rulerBottom + 1, rulerX + 4, rulerBottom + 1);
        display->fillRect(rulerX - 5, indicatorY - 3, 11, 7);

        display->setColor(BLACK);
        display->drawLine(rulerX, rulerTop, rulerX, rulerBottom);
        display->drawLine(rulerX - 3, rulerTop, rulerX + 3, rulerTop);
        display->drawLine(rulerX - 3, rulerBottom, rulerX + 3, rulerBottom);
        display->fillRect(rulerX - 4, indicatorY - 2, 9, 5);

        char zoomText[8];
        snprintf(zoomText, sizeof(zoomText), "z%d", zoom);
        display->setTextAlignment(TEXT_ALIGN_RIGHT);
        display->setFont(FONT_SMALL);
        drawHaloString(display, rulerX - 7, indicatorY - FONT_HEIGHT_SMALL / 2, zoomText);
    }

#if BASEUI_MAP_ONSCREEN_CONTROLS
    // Last, so nothing is drawn over what the finger is aiming at.
    drawMapControls(display, x, y, viewWidth, viewHeight);
    s_controlsOnScreen = true;
#endif
}

#if BASEUI_MAP_ONSCREEN_CONTROLS
bool MapRenderer::handleControlTap(int16_t tapX, int16_t tapY)
{
    if (!s_controlsOnScreen)
        return false;

    // x is 0, not the draw's frame origin: that is only non-zero mid-transition, and a tap lands on the frame at rest.
    MapControlRect rects[kMapControlCount];
    layoutMapControls(0, s_lastViewTop, s_lastViewWidth, s_lastViewHeight, rects);

    for (int i = 0; i < kMapControlCount; ++i) {
        const MapControlRect &r = rects[i];
        if (tapX < r.x || tapX >= r.x + r.w || tapY < r.y || tapY >= r.y + r.h)
            continue;

        switch ((MapControl)i) {
        case MapControl::ZoomIn:
            zoomIn();
            return true;
        case MapControl::ZoomOut:
            zoomOut();
            return true;
        case MapControl::Pan:
            setPanModeEnabled(!s_panMode);
            return true;
        case MapControl::FollowMe:
            setFollowMeEnabled(!s_followMe);
            return true;
        default:
            return false;
        }
    }
    return false;
}
#endif

#endif // BASEUI_HAS_MAP
