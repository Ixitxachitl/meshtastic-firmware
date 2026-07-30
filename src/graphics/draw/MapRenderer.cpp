#include "graphics/draw/MapRenderer.h"

#include "NodeDB.h"
#include "gps/GeoCoord.h"
#include "graphics/SharedUIDisplay.h"
#include "graphics/images.h"
#include "graphics/niche/Map/MapTileRenderer.h"

#if defined(ARCH_PORTDUINO) || defined(ARCH_ESP32)
#include "graphics/niche/Map/MapTileSourceFile.h"
#endif
#if defined(HAS_SDCARD)
#include "graphics/niche/Map/MapTileSourceSD.h"
#endif

#include <math.h>

using namespace graphics;

namespace
{

constexpr float kEarthRadiusMeters = 6378137.0f;
constexpr int kDefaultZoomAlone = 6; // Sensible starting zoom when only our own node is known

int16_t s_lastViewWidth = 128;
int16_t s_lastViewHeight = 64;

bool s_panMode = false;
bool s_zoomMode = false;
bool s_followMe = true;
float s_centerLat = 0;
float s_centerLng = 0;
bool s_centerInitialized = false;

int s_zoom = -1; // -1 = not yet initialized; set to an auto-fit value on first use.

float metersToPxForZoom(int zoom, float latDeg)
{
    float latRad = latDeg * DEG_TO_RAD;
    float mpp = (2.0f * (float)M_PI * kEarthRadiusMeters / (256.0f * (float)(1 << zoom))) * cosf(latRad);
    return mpp > 0.0f ? 1.0f / mpp : 0.0f;
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

// Own node position if known, else the centroid of all known node positions.
//
// Deliberately reads the live `localPosition` global instead of going through
// nodeDB->copyNodePosition(ourNodeNum, ...): that call looks up our own entry in the
// nodePositions satellite table, which is only refreshed when we broadcast a position packet to
// the mesh, and can be stale (e.g. from a previous test location) even while `localPosition`
// itself is current. Using the stale table entry as "where I am" was landing Follow Me somewhere
// other than the actual current position.
bool computeAutoCenter(float *lat, float *lng)
{
    if (localPosition.latitude_i != 0 || localPosition.longitude_i != 0) {
        *lat = localPosition.latitude_i * 1e-7f;
        *lng = localPosition.longitude_i * 1e-7f;
        return true;
    }
    return computeNodeCentroid(lat, lng);
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
    if (s_zoom < 0)
        s_zoom = computeAutoFitZoom(centerLat, centerLng, s_lastViewWidth, s_lastViewHeight);
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
void panByScreenFraction(float dxFraction, float dyFraction)
{
    ensureCenterInitialized();
    if (!s_centerInitialized)
        return;
    s_followMe = false; // Otherwise the next redraw would immediately snap back to our own position.
    ensureZoomInitialized(s_centerLat, s_centerLng);

    const float worldPxAtZoom = 256.0f * (float)(1 << s_zoom);
    const float degPerPxLng = 360.0f / worldPxAtZoom;
    const float degPerPxLat = degPerPxLng * cosf(s_centerLat * DEG_TO_RAD);

    constexpr float kPanFractionOfView = 0.15f;
    const float stepPx = min(s_lastViewWidth, s_lastViewHeight) * kPanFractionOfView;

    s_centerLat += dyFraction * stepPx * degPerPxLat;
    s_centerLng += dxFraction * stepPx * degPerPxLng;

    if (s_centerLat > 85.0f)
        s_centerLat = 85.0f;
    if (s_centerLat < -85.0f)
        s_centerLat = -85.0f;
    s_centerLng = fmodf(s_centerLng + 540.0f, 360.0f) - 180.0f; // Wrap to [-180, 180).
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
    static bool attempted = false;
    static NicheGraphics::MapTiles::FileTileSource source;
    if (attempted)
        return;
    attempted = true;
    if (source.begin("/MAP.BIN"))
        NicheGraphics::MapTiles::setTileSource(&source);
}
#endif

// Offsets used to build a 1px halo by blitting a glyph/icon 8 times before the real draw - see
// drawHaloXbm/drawHaloString below.
constexpr int8_t kHaloOffsets[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};

// Draws xbm/text with a solid WHITE halo behind a BLACK fill, so it stays readable against any
// part of the basemap (dense tile art, blank background, etc.) without depending on XOR/INVERSE
// against whatever's underneath. Replaces the old drawn-INVERSE approach, which read fine against
// any single background but caused overlapping elements to XOR-cancel back to background.
void drawHaloXbm(OLEDDisplay *display, int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t *xbm)
{
    display->setColor(WHITE);
    for (auto &o : kHaloOffsets)
        display->drawXbm(x + o[0], y + o[1], w, h, xbm);
    display->setColor(BLACK);
    display->drawXbm(x, y, w, h, xbm);
}

void drawHaloString(OLEDDisplay *display, int16_t x, int16_t y, const char *text)
{
    display->setColor(WHITE);
    for (auto &o : kHaloOffsets)
        display->drawString(x + o[0], y + o[1], text);
    display->setColor(BLACK);
    display->drawString(x, y, text);
}

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

bool MapRenderer::isFollowMeEnabled()
{
    return s_followMe;
}

void MapRenderer::setFollowMeEnabled(bool enabled)
{
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
    s_zoom = zoom;
}

void MapRenderer::zoomIn()
{
    setZoom(zoom() + 1);
}

void MapRenderer::zoomOut()
{
    setZoom(zoom() - 1);
}

void MapRenderer::drawMapFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
#if defined(ARCH_PORTDUINO) || defined(ARCH_ESP32)
    ensureFileTileSourceInitialized();
#endif

    display->clear();
    // WHITE is the lit/visible pixel color on OLEDDisplay (unlike InkHUD's e-ink convention,
    // where BLACK means ink) - everything below needs to actually show up on real hardware.
    display->setColor(WHITE);

    int16_t viewWidth = display->getWidth();
    int16_t viewHeight = display->getHeight();

    // Shared battery/time header, same as every other BaseUI screen - reserve its height and
    // shift the map viewport down so tiles/markers/overlays never draw underneath it.
    // Matches drawCommonHeader's own internal footprint exactly (SharedUIDisplay.cpp: headerHeight
    // = highlightHeight + 2, highlightHeight = FONT_HEIGHT_SMALL - 1, so FONT_HEIGHT_SMALL + 1) -
    // NodeListRenderer's COMMON_HEADER_HEIGHT (FONT_HEIGHT_SMALL - 1) is 2px short of that, which
    // left the map drawing over the header's bottom edge and XOR-inverting it.
    const int16_t kHeaderHeight = FONT_HEIGHT_SMALL + 1;
    drawCommonHeader(display, x, y, "Map");
    display->setColor(WHITE); // drawCommonHeader leaves its own color state active
    y += kHeaderHeight;
    viewHeight -= kHeaderHeight;

    s_lastViewWidth = viewWidth;
    s_lastViewHeight = viewHeight;

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
    const float metersToPx = metersToPxForZoom(zoom, centerLat);

    struct PlotCtx {
        OLEDDisplay *display;
        int16_t offX, offY;
    } plotCtx{display, x, y};

    NicheGraphics::MapTiles::drawTileBackground(
        centerLat, centerLng, zoom, metersToPx, viewWidth, viewHeight,
        [](void *ctx, int16_t px, int16_t py) {
            auto *c = static_cast<PlotCtx *>(ctx);
            c->display->setPixel(c->offX + px, c->offY + py);
        },
        &plotCtx);

    // Known node markers (self is drawn separately, last, so it's always on top).
    const NodeNum ourNodeNum = nodeDB->getNodeNum();

    // Everything below is drawn with drawHaloXbm/drawHaloString (solid WHITE halo behind a BLACK
    // fill) rather than a fixed color or XOR/INVERSE, so markers, labels, and both text overlays
    // stay readable no matter what part of the basemap they land on. Markers still dedupe against
    // nearby already-drawn marker positions below - not needed for correctness anymore (halo draws
    // don't cancel out like XOR did), but it still avoids wasted draws and visual clutter when many
    // nodes collapse onto nearly the same screen position at low zoom.
    constexpr int16_t kMarkerDedupeRadius = 4;
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

    // FONT_SMALL_LOCAL rather than FONT_SMALL deliberately: on TFT/HAS_SPI_TFT builds FONT_SMALL is
    // redirected to the 19px-tall medium font (bigger screen, so BaseUI normally wants bigger text)
    // - far too large for a map label sitting next to a marker, where many names need to fit close
    // together without overlapping.
    display->setFont(FONT_SMALL_LOCAL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    const int16_t labelHeight = _fontHeight(FONT_SMALL_LOCAL);

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
            if (abs(drawnMx[d] - mx) <= kMarkerDedupeRadius && abs(drawnMy[d] - my) <= kMarkerDedupeRadius) {
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

        drawHaloXbm(display, mx - 4, my - 4, 8, 8, icon_map_node);

        if (node->short_name[0] != '\0') {
            int16_t lx = mx + 5;
            int16_t ly = my - labelHeight / 2;
            int16_t lw = (int16_t)display->getStringWidth(node->short_name);

            bool overlaps = false;
            for (int li = 0; li < labelCount; li++) {
                if (lx < labelX[li] + labelW[li] && lx + lw > labelX[li] && ly < labelY[li] + labelH[li] &&
                    ly + labelHeight > labelY[li]) {
                    overlaps = true;
                    break;
                }
            }

            if (!overlaps) {
                drawHaloString(display, lx, ly, node->short_name);
                if (labelCount < kMaxLabelsTracked) {
                    labelX[labelCount] = lx;
                    labelY[labelCount] = ly;
                    labelW[labelCount] = lw;
                    labelH[labelCount] = labelHeight;
                    labelCount++;
                }
            }
        }
    }

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

    // Center coordinates, top-left - a concrete reference for "where am I", especially useful
    // before any basemap tiles are baked in, or after panning away from every known node.
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);
    char coordLabel[24];
    snprintf(coordLabel, sizeof(coordLabel), "%.4f,%.4f", centerLat, centerLng);
    drawHaloString(display, x + 1, y, coordLabel);

    // Status label, bottom-right corner: zoom level, plus mode/follow indicators.
    display->setTextAlignment(TEXT_ALIGN_RIGHT);
    char statusLabel[24];
    snprintf(statusLabel, sizeof(statusLabel), "z%d%s%s%s", zoom, s_panMode ? " PAN" : "", s_zoomMode ? " ZOOM" : "",
             s_followMe ? " ME" : "");
    drawHaloString(display, x + viewWidth - 2, y + viewHeight - FONT_HEIGHT_SMALL - 1, statusLabel);

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
}
