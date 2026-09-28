#include "graphics/draw/MapNavigation.h"

#if BASEUI_MAP_NAVIGATION

#include "NodeDB.h"
#include "gps/GeoCoord.h"
#include "graphics/Screen.h"
#include "graphics/draw/MenuHandler.h"
#include "main.h"
#include "mesh/Throttle.h"
#if !MESHTASTIC_EXCLUDE_WAYPOINT
#include "WaypointStore.h"
#endif
#if BASEUI_MAP_ADDRESS_SEARCH || BASEUI_MAP_ROUTING
#include "graphics/niche/Map/MapTileFetch.h"
#endif
#ifdef MESHTASTIC_ENCRYPTED_STORAGE
#include "security/EncryptedStorage.h"
#endif
#if BASEUI_MAP_ROUTING
#include <esp_heap_caps.h>
#endif

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace graphics::MapNavigation
{
namespace
{

meshtastic_NavTarget target = meshtastic_NavTarget_init_zero;
bool active = false;
bool loaded = false;
#if BASEUI_MAP_ADDRESS_SEARCH
bool awaitingSearch = false;
#endif

// Direction of travel, kept so a heading-up map holds still while stopped.
float lastHeading = 0;
bool haveHeading = false;

#if BASEUI_MAP_ROUTING
namespace Fetch = NicheGraphics::MapTiles::Fetch;
namespace Route = NicheGraphics::MapTiles::Route;

constexpr float kOffRouteMeters = 50.0f;  // further than this from the route is off it
constexpr uint32_t kOffRouteMs = 10000;   // for this long before asking for a new one
constexpr uint32_t kRerouteGapMs = 30000; // and never more often than this
constexpr uint32_t kRetryMs = 20000;      // after a request that couldn't be sent, or failed
constexpr float kArrivedMeters = 25.0f;
constexpr float kTargetMovedMeters = 150.0f; // a node target this far from the route's end gets a new route

// The route being followed, copied out of the fetcher so a reroute can land while this one is still drawn.
uint32_t pointCount = 0, pointCap = 0;
int32_t *latE7 = nullptr, *lonE7 = nullptr;
uint32_t *mercX = nullptr, *mercY = nullptr;
float *cumulative = nullptr; // metres along the route to each point
Route::Maneuver *maneuvers = nullptr;
uint16_t maneuverCount = 0, maneuverCap = 0;
float routeSeconds = 0;

uint32_t progressIndex = 0; // the segment we are on runs from this point to the next
float progressAlong = 0;    // metres along the route to where we are
bool arrived = false;
bool requestInFlight = false, lastRequestFailed = false;
bool discardInFlight = false; // the target changed under a request: its reply is for the old one
uint32_t lastRequestMs = 0, offRouteSinceMs = 0;
int sessionSlot = -1;      // the saved route this trip writes to, so rerouting replaces it instead of adding another
bool awaitingList = false; // the Saved Routes menu is waiting on the card
#endif

void ensureLoaded()
{
    if (loaded)
        return;
    loaded = true;
    if (uiconfig.has_map_data && uiconfig.map_data.has_nav_target) {
        target = uiconfig.map_data.nav_target; // for its travel mode; a trip is picked up again from Saved Routes
    }
}

// Written straight away rather than on the map's autosave cadence: a target is picked by hand, rarely.
void save()
{
#ifdef MESHTASTIC_ENCRYPTED_STORAGE
    if (EncryptedStorage::isLockdownActive())
        return; // a location, held back like the saved map home
#endif
    uiconfig.has_map_data = true;
    uiconfig.map_data.has_nav_target = true;
    uiconfig.map_data.nav_target = target;
    nodeDB->saveProto("/prefs/uiconfig.proto", meshtastic_DeviceUIConfig_size, &meshtastic_DeviceUIConfig_msg, &uiconfig);
}

#if BASEUI_MAP_ROUTING
void dropRoute()
{
    discardInFlight = requestInFlight;
    pointCount = 0;
    maneuverCount = 0;
    progressIndex = 0;
    progressAlong = 0;
    arrived = false;
    offRouteSinceMs = 0;
}

bool reserve(uint32_t points, uint16_t turns)
{
    if (points > pointCap) {
        free(latE7);
        free(lonE7);
        free(mercX);
        free(mercY);
        free(cumulative);
        latE7 = static_cast<int32_t *>(heap_caps_malloc(points * sizeof(int32_t), MALLOC_CAP_SPIRAM));
        lonE7 = static_cast<int32_t *>(heap_caps_malloc(points * sizeof(int32_t), MALLOC_CAP_SPIRAM));
        mercX = static_cast<uint32_t *>(heap_caps_malloc(points * sizeof(uint32_t), MALLOC_CAP_SPIRAM));
        mercY = static_cast<uint32_t *>(heap_caps_malloc(points * sizeof(uint32_t), MALLOC_CAP_SPIRAM));
        cumulative = static_cast<float *>(heap_caps_malloc(points * sizeof(float), MALLOC_CAP_SPIRAM));
        pointCap = (latE7 && lonE7 && mercX && mercY && cumulative) ? points : 0;
        if (!pointCap)
            return false;
    }
    if (turns > maneuverCap) {
        free(maneuvers);
        maneuvers = static_cast<Route::Maneuver *>(heap_caps_malloc(turns * sizeof(Route::Maneuver), MALLOC_CAP_SPIRAM));
        maneuverCap = maneuvers ? turns : 0;
        if (!maneuverCap)
            return false;
    }
    return true;
}

// Web Mercator as a fraction of the world, scaled to 2^32 - the tiles' own projection, at every zoom at once.
void toMercator(int32_t lat, int32_t lon, uint32_t &x, uint32_t &y)
{
    double latDeg = lat * 1e-7;
    if (latDeg > 85.05112878)
        latDeg = 85.05112878;
    if (latDeg < -85.05112878)
        latDeg = -85.05112878;
    const double s = sin(latDeg * M_PI / 180.0);
    const double fx = (lon * 1e-7 + 180.0) / 360.0;
    const double fy = 0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * M_PI);
    x = (uint32_t)fmin(fmax(fx * 4294967296.0, 0.0), 4294967295.0);
    y = (uint32_t)fmin(fmax(fy * 4294967296.0, 0.0), 4294967295.0);
}

void adopt(const Route::Result &r)
{
    if (r.pointCount < 2 || !reserve(r.pointCount, r.maneuverCount ? r.maneuverCount : 1)) {
        lastRequestFailed = true;
        return;
    }
    dropRoute();
    pointCount = r.pointCount;
    memcpy(latE7, r.latE7, pointCount * sizeof(int32_t));
    memcpy(lonE7, r.lonE7, pointCount * sizeof(int32_t));
    maneuverCount = r.maneuverCount;
    memcpy(maneuvers, r.maneuvers, maneuverCount * sizeof(Route::Maneuver));
    routeSeconds = r.timeSec;
    cumulative[0] = 0;
    for (uint32_t i = 0; i < pointCount; i++) {
        toMercator(latE7[i], lonE7[i], mercX[i], mercY[i]);
        if (i > 0)
            cumulative[i] = cumulative[i - 1] +
                            GeoCoord::latLongToMeter(latE7[i - 1] * 1e-7, lonE7[i - 1] * 1e-7, latE7[i] * 1e-7, lonE7[i] * 1e-7);
    }
    lastRequestFailed = false;
}

bool haveFix()
{
    return localPosition.latitude_i != 0 || localPosition.longitude_i != 0;
}

void requestRoute()
{
    double toLat, toLon;
    if (!haveFix() || !targetPosition(toLat, toLon))
        return;
    lastRequestMs = millis();
    if (lastRequestMs == 0)
        lastRequestMs = 1;
    Fetch::clearRoute();
    NicheGraphics::MapTiles::RouteStore::Header saveAs{};
    saveAs.targetLatE7 = target.latitude_i;
    saveAs.targetLonE7 = target.longitude_i;
    saveAs.nodeNum = target.node_num;
    saveAs.waypointId = target.waypoint_id;
    saveAs.travelMode = (uint8_t)target.travel_mode;
    strncpy(saveAs.name, target.name, sizeof(saveAs.name) - 1);
    requestInFlight = Fetch::startRoute(localPosition.latitude_i * 1e-7, localPosition.longitude_i * 1e-7, toLat, toLon,
                                        (Route::Mode)target.travel_mode, saveAs, sessionSlot);
    if (!requestInFlight)
        lastRequestFailed = true; // WiFi down, most likely; try again in a while
}

// Closest point on the route to us, searched near where we were first and then everywhere. Returns metres off it.
float track(float &alongOut, uint32_t &indexOut)
{
    const double latRad = localPosition.latitude_i * 1e-7 * M_PI / 180.0;
    const double mPerLat = 110540.0 * 1e-7, mPerLon = 111320.0 * cos(latRad) * 1e-7;
    auto px = [&](uint32_t i) { return (float)((lonE7[i] - localPosition.longitude_i) * mPerLon); };
    auto py = [&](uint32_t i) { return (float)((latE7[i] - localPosition.latitude_i) * mPerLat); };

    float best = 1e30f;
    auto scan = [&](uint32_t from, uint32_t to) {
        for (uint32_t i = from; i + 1 < to; i++) {
            const float ax = px(i), ay = py(i), bx = px(i + 1), by = py(i + 1);
            const float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
            float t = len2 > 0 ? -(ax * dx + ay * dy) / len2 : 0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            const float cx = ax + t * dx, cy = ay + t * dy, d2 = cx * cx + cy * cy;
            if (d2 < best) {
                best = d2;
                indexOut = i;
                alongOut = cumulative[i] + t * (cumulative[i + 1] - cumulative[i]);
            }
        }
    };
    const uint32_t from = progressIndex > 10 ? progressIndex - 10 : 0;
    const uint32_t to = progressIndex + 400 < pointCount ? progressIndex + 400 : pointCount;
    scan(from, to);
    // Lost - a U-turn, or a skipped stretch: search the whole route, but not every frame. Off it, that walk would
    // otherwise run at the full map framerate for as long as it takes to get back on.
    static uint32_t lastFullScanMs = 0;
    if (sqrtf(best) > kOffRouteMeters && (lastFullScanMs == 0 || !Throttle::isWithinTimespanMs(lastFullScanMs, 2000))) {
        lastFullScanMs = millis() ? millis() : 1;
        scan(0, pointCount);
    }
    return sqrtf(best);
}
#endif

void copyName(char (&dst)[sizeof(meshtastic_NavTarget::name)], const char *src)
{
    strncpy(dst, src ? src : "", sizeof(dst) - 1);
    dst[sizeof(dst) - 1] = '\0';
}

void start(const meshtastic_NavTarget &next)
{
    ensureLoaded();
    const meshtastic_NavTravelMode mode = target.travel_mode;
    target = next;
    target.travel_mode = mode; // the mode carries over to the next target
    active = true;
#if BASEUI_MAP_ROUTING
    dropRoute();
    lastRequestMs = 0;
    lastRequestFailed = false;
    sessionSlot = -1; // a new trip: its route gets a slot of its own
#endif
    save();
}

} // namespace

bool navigateToNode(uint32_t nodeNum)
{
    meshtastic_PositionLite pos;
    meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(nodeNum);
    if (!node || !nodeDB->hasValidPosition(node) || !nodeDB->copyNodePosition(nodeNum, pos))
        return false;
    meshtastic_NavTarget next = meshtastic_NavTarget_init_zero;
    next.latitude_i = pos.latitude_i;
    next.longitude_i = pos.longitude_i;
    next.node_num = nodeNum;
    copyName(next.name, node->long_name[0] ? node->long_name : node->short_name);
    start(next);
    return true;
}

bool navigateToWaypoint(uint32_t waypointId)
{
#if MESHTASTIC_EXCLUDE_WAYPOINT
    (void)waypointId;
    return false;
#else
    const StoredWaypoint *entry = waypointStore.findWaypoint(waypointId);
    if (!entry || !entry->waypoint.has_latitude_i || !entry->waypoint.has_longitude_i)
        return false;
    meshtastic_NavTarget next = meshtastic_NavTarget_init_zero;
    next.latitude_i = entry->waypoint.latitude_i;
    next.longitude_i = entry->waypoint.longitude_i;
    next.waypoint_id = waypointId;
    copyName(next.name, entry->waypoint.name[0] ? entry->waypoint.name : "Waypoint");
    start(next);
    return true;
#endif
}

void navigateToLocation(double lat, double lon, const char *name)
{
    meshtastic_NavTarget next = meshtastic_NavTarget_init_zero;
    next.latitude_i = (int32_t)lround(lat * 1e7);
    next.longitude_i = (int32_t)lround(lon * 1e7);
    if (name && name[0])
        copyName(next.name, name);
    else
        snprintf(next.name, sizeof(next.name), "%.5f, %.5f", lat, lon);
    start(next);
}

void stop()
{
    ensureLoaded();
    if (!active)
        return;
    active = false;
#if BASEUI_MAP_ROUTING
    dropRoute();
#endif
    save();
}

bool isActive()
{
    return active;
}

bool targetPosition(double &lat, double &lon)
{
    if (!active)
        return false;
    int32_t latI = target.latitude_i, lonI = target.longitude_i;
    meshtastic_PositionLite pos;
    if (target.node_num && nodeDB->copyNodePosition(target.node_num, pos) && (pos.latitude_i || pos.longitude_i)) {
        latI = pos.latitude_i;
        lonI = pos.longitude_i;
    }
#if !MESHTASTIC_EXCLUDE_WAYPOINT
    if (target.waypoint_id) {
        if (const StoredWaypoint *entry = waypointStore.findWaypoint(target.waypoint_id)) {
            latI = entry->waypoint.latitude_i;
            lonI = entry->waypoint.longitude_i;
        }
    }
#endif
    lat = latI * 1e-7;
    lon = lonI * 1e-7;
    return true;
}

const char *targetName()
{
    return target.name;
}

#if BASEUI_MAP_ROUTING
bool listSavedRoutes()
{
    Fetch::clearList();
    awaitingList = Fetch::startListRoutes();
    return awaitingList;
}

void pollSavedRoutes()
{
    if (!awaitingList)
        return;
    const Fetch::ListState state = Fetch::listState();
    if (state != Fetch::ListState::Done && state != Fetch::ListState::Failed)
        return;
    awaitingList = false;
    menuHandler::menuQueue = menuHandler::NavSavedRoutesMenu;
    if (screen)
        screen->runNow();
}

void navigateSaved(int slot, const NicheGraphics::MapTiles::RouteStore::Header &saved)
{
    meshtastic_NavTarget next = meshtastic_NavTarget_init_zero;
    next.latitude_i = saved.targetLatE7;
    next.longitude_i = saved.targetLonE7;
    next.node_num = saved.nodeNum;
    next.waypoint_id = saved.waypointId;
    copyName(next.name, saved.name);
    start(next);
    target.travel_mode = (meshtastic_NavTravelMode)saved.travelMode;
    save();
    sessionSlot = slot; // rerouting on this trip replaces this saved route
    // Read back from the card rather than fetched, so it works with no WiFi; a failed read falls back to fetching.
    lastRequestMs = millis() ? millis() : 1;
    Fetch::clearRoute();
    requestInFlight = Fetch::startLoadRoute(slot);
}
#endif

meshtastic_NavTravelMode travelMode()
{
    ensureLoaded();
    return target.travel_mode;
}

void setTravelMode(meshtastic_NavTravelMode mode)
{
    ensureLoaded();
    if (target.travel_mode == mode)
        return;
    target.travel_mode = mode;
#if BASEUI_MAP_ROUTING
    if (active) { // the old route was for the old mode
        dropRoute();
        lastRequestMs = 0;
        lastRequestFailed = false;
    }
#endif
    save();
}

meshtastic_MapViewMode viewMode()
{
    return uiconfig.has_map_data ? uiconfig.map_data.view_mode : meshtastic_MapViewMode_NORTH_UP;
}

void setViewMode(meshtastic_MapViewMode mode)
{
    uiconfig.has_map_data = true;
    uiconfig.map_data.view_mode = mode;
    nodeDB->saveProto("/prefs/uiconfig.proto", meshtastic_DeviceUIConfig_size, &meshtastic_DeviceUIConfig_msg, &uiconfig);
}

void update()
{
#if BASEUI_MAP_ROUTING
    if (requestInFlight) {
        const Fetch::RouteState state = Fetch::routeState();
        if (state == Fetch::RouteState::Done) {
            const Route::Result *r = Fetch::routeResult();
            if (r && !discardInFlight) {
                adopt(*r);
                if (Fetch::routeSlot() >= 0)
                    sessionSlot = Fetch::routeSlot();
            }
            Fetch::clearRoute();
            requestInFlight = false;
        } else if (state == Fetch::RouteState::Failed || state == Fetch::RouteState::Idle) {
            Fetch::clearRoute();
            requestInFlight = false;
            lastRequestFailed = !discardInFlight;
        }
        if (!requestInFlight)
            discardInFlight = false;
    }

    if (!active || !haveFix())
        return;
    const bool waited = lastRequestMs == 0 || !Throttle::isWithinTimespanMs(lastRequestMs, kRetryMs);
    if (pointCount == 0) {
        if (!requestInFlight && waited)
            requestRoute();
        return;
    }

    float along = 0;
    uint32_t index = progressIndex;
    const float off = track(along, index);
    if (off <= kOffRouteMeters) {
        progressIndex = index;
        progressAlong = along;
        offRouteSinceMs = 0;
    } else if (offRouteSinceMs == 0) {
        offRouteSinceMs = millis() ? millis() : 1;
    }
    arrived = cumulative[pointCount - 1] - progressAlong < kArrivedMeters && off <= kOffRouteMeters;

    // Off it long enough, or a node target that has wandered away from where the route ends: ask again.
    bool reroute = offRouteSinceMs && !Throttle::isWithinTimespanMs(offRouteSinceMs, kOffRouteMs);
    double toLat, toLon;
    if (target.node_num && targetPosition(toLat, toLon) &&
        GeoCoord::latLongToMeter(toLat, toLon, latE7[pointCount - 1] * 1e-7, lonE7[pointCount - 1] * 1e-7) > kTargetMovedMeters)
        reroute = true;
    if (reroute && !requestInFlight && !arrived && !Throttle::isWithinTimespanMs(lastRequestMs, kRerouteGapMs))
        requestRoute();
#endif
}

Guidance guidance()
{
    Guidance g{};
#if BASEUI_MAP_ROUTING
    g.routing = requestInFlight;
    g.routeFailed = lastRequestFailed && pointCount == 0;
    if (pointCount > 0) {
        g.haveRoute = true;
        g.arrived = arrived;
        const float total = cumulative[pointCount - 1];
        g.metersRemaining = total - progressAlong;
        if (g.metersRemaining < 0)
            g.metersRemaining = 0;
        g.secondsRemaining = total > 0 ? routeSeconds * g.metersRemaining / total : 0;
        for (uint16_t i = 0; i < maneuverCount; i++) {
            if (maneuvers[i].beginIndex > progressIndex) {
                g.instruction = maneuvers[i].instruction;
                g.maneuverType = maneuvers[i].type;
                g.metersToManeuver = cumulative[maneuvers[i].beginIndex] - progressAlong;
                break;
            }
        }
        return g;
    }
#endif
    double toLat, toLon;
    if ((localPosition.latitude_i || localPosition.longitude_i) && targetPosition(toLat, toLon)) {
        g.metersRemaining =
            GeoCoord::latLongToMeter(localPosition.latitude_i * 1e-7, localPosition.longitude_i * 1e-7, toLat, toLon);
        g.arrived = g.metersRemaining < 20.0f;
    }
    return g;
}

uint32_t routePointCount()
{
#if BASEUI_MAP_ROUTING
    return active ? pointCount : 0;
#else
    return 0;
#endif
}

const uint32_t *routeMercatorX()
{
#if BASEUI_MAP_ROUTING
    return mercX;
#else
    return nullptr;
#endif
}

const uint32_t *routeMercatorY()
{
#if BASEUI_MAP_ROUTING
    return mercY;
#else
    return nullptr;
#endif
}

uint32_t routeProgress()
{
#if BASEUI_MAP_ROUTING
    return progressIndex;
#else
    return 0;
#endif
}

bool heading(float &degrees)
{
    // km/h, as the GPS driver stores it. A vehicle's own metal throws a compass off; its course is steady once moving.
    const uint32_t speed = localPosition.ground_speed;
    if (speed >= 8) {
        lastHeading = localPosition.ground_track * 1e-5f;
        haveHeading = true;
    } else if (screen && screen->hasHeading()) {
        lastHeading = screen->getHeading();
        haveHeading = true;
    } else if (speed >= 2) {
        lastHeading = localPosition.ground_track * 1e-5f;
        haveHeading = true;
    }
    degrees = lastHeading;
    return haveHeading;
}

#if BASEUI_MAP_ADDRESS_SEARCH
bool startAddressSearch(const char *query)
{
    namespace Fetch = NicheGraphics::MapTiles::Fetch;
    Fetch::clearSearch();
    awaitingSearch = Fetch::startSearch(query);
    return awaitingSearch;
}

void pollAddressSearch()
{
    namespace Fetch = NicheGraphics::MapTiles::Fetch;
    if (!awaitingSearch)
        return;
    const Fetch::SearchState state = Fetch::searchState();
    if (state != Fetch::SearchState::Done && state != Fetch::SearchState::Failed)
        return;
    awaitingSearch = false;
    menuHandler::menuQueue = menuHandler::NavSearchResultsMenu;
    if (screen)
        screen->runNow();
}
#endif

} // namespace graphics::MapNavigation

#endif
