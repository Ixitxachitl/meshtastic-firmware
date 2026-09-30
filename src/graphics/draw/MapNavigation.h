#pragma once

#include "configuration.h"

#if BASEUI_MAP_NAVIGATION

#include "mesh/generated/meshtastic/device_ui.pb.h"
#if BASEUI_MAP_ROUTING
#include "graphics/niche/Map/MapRouteStore.h"
#endif
#include <stdint.h>
#include <string>

// Map > Navigate: the target, the street route to it, and progress along that route. Display task only.
namespace graphics::MapNavigation
{

// Starting navigation saves the target to uiconfig, marked active, so a reboot mid-trip can offer it again.
// False (and nothing changes) when the node or waypoint has no known position.
bool navigateToNode(uint32_t nodeNum);
bool navigateToWaypoint(uint32_t waypointId);
void navigateToLocation(double lat, double lon, const char *name);
void stop();

bool isActive();
// The target's position now: live for a node or waypoint, falling back to where it was when picked.
bool targetPosition(double &lat, double &lon);
const char *targetName();

#if BASEUI_MAP_ROUTING
// Saved Routes: every route fetched is kept on the card. Listing reads the card on the fetch task; once it is in,
// pollSavedRoutes (called as the map draws) queues the menu that shows it.
bool listSavedRoutes();
void pollSavedRoutes();
// Navigates a saved route, read back from the card - no WiFi needed while it is followed.
void navigateSaved(int slot, const NicheGraphics::MapTiles::RouteStore::Header &saved);
#endif

// How the route is travelled; changing it while navigating asks for a new route.
meshtastic_NavTravelMode travelMode();
void setTravelMode(meshtastic_NavTravelMode mode);
// How the map turns while navigating with Follow Me on.
meshtastic_MapViewMode viewMode();
void setViewMode(meshtastic_MapViewMode mode);

// Called as the map draws: sends route requests, adopts finished routes, follows progress, reroutes when off it.
void update();

// What to tell the user next. Distances are along the route when there is one, straight-line otherwise.
struct Guidance {
    bool haveRoute;          // a street route is being followed
    bool routing;            // a route request is in flight
    bool routeFailed;        // the last request failed; retried later
    bool arrived;            // within a few metres of the end
    const char *instruction; // the next turn, or null
    uint16_t maneuverType;   // Valhalla's type for that turn
    float metersToManeuver;
    float metersRemaining;
    float secondsRemaining; // 0 when unknown
};
Guidance guidance();

// The route as Web Mercator fractions of the world width (2^32 wraps once around), for drawing. `progress` is the
// point the user has passed: drawing from there on shows only what is left.
uint32_t routePointCount();
const uint32_t *routeMercatorX();
const uint32_t *routeMercatorY();
uint32_t routeProgress();

// Direction of travel for a heading-up map, in degrees clockwise from north: the GPS course when moving fast enough
// to trust it, the compass otherwise where there is one. False until either has given a reading.
bool heading(float &degrees);

#if BASEUI_MAP_ADDRESS_SEARCH
// Sends a search and remembers that the menu is waiting on it. False if one couldn't be sent.
bool startAddressSearch(const char *query);
// Called as the map draws: once the search has finished, queues the results menu.
void pollAddressSearch();

// Live suggestions under the Address prompt: begin when it opens, feed it each edit, poll as the map draws (it asks once
// typing pauses and posts what comes back), and end when it closes. A picked suggestion is navigated to.
void beginAddressSuggestions();
void addressTyped(const std::string &text);
void pollAddressSuggestions();
void pickAddressSuggestion(int index);
void endAddressSuggestions();
#endif

} // namespace graphics::MapNavigation

#endif
