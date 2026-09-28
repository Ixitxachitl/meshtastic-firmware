#pragma once

#include "configuration.h"

#if BASEUI_MAP_ONLINE_TILES

#include "./MapGeocodeParse.h"
#include "./MapRouteParse.h"
#include "./MapRouteStore.h"
#include <stdint.h>

// Downloads map tiles the card doesn't have, one at a time, into the same /maps/<style>/z/x/y.png layout the
// reader and device-ui both use. On demand only: no bulk download, and WiFi is never brought up for this.
namespace NicheGraphics::MapTiles::Fetch
{

// Queue a tile the reader couldn't find. Cheap and safe to call from the display task; silently does nothing
// unless the toggle is on, WiFi is already up, and the active style has a .url beside its tiles.
void requestTile(int z, int32_t x, int32_t y);

// Called each time the map draws. Fetching only runs while the map is actually on screen, so leaving the frame
// stops the radio work without needing a teardown path.
void noteMapDrawn();

#if BASEUI_MAP_ADDRESS_SEARCH
// Address search for the map's Navigate > Address, run on the same task as the tiles: one at a time, at most one a
// second, per Nominatim's policy. Needs WiFi up, but not the online-tiles toggle - it is only ever asked for by hand.
enum class SearchState : uint8_t { Idle, Queued, Running, Done, Failed };

// False if a search is already under way, the query is empty, or WiFi is down.
bool startSearch(const char *query);
SearchState searchState();
// The places found, while the state is Done; 0 otherwise. Valid until the next startSearch.
int searchResults(const Geocode::Result *&results);
// Back to Idle once the result has been shown.
void clearSearch();
#endif

#if BASEUI_MAP_ROUTING
// A street route for Map > Navigate, from Valhalla, on the same task. One job at a time; fetching needs WiFi up.
// Every route fetched is saved to the card (/maps/.routes), so it can be followed again without WiFi.
enum class RouteState : uint8_t { Idle, Queued, Running, Done, Failed };
constexpr int kRouteSlots = 10; // saved routes kept on the card; the oldest makes way

// Fetches a route and saves it as `saveAs` (counts and totals filled in): into `saveSlot`, or with -1 into a free
// slot or else the oldest. False if a job is already under way or WiFi is down.
bool startRoute(double fromLat, double fromLon, double toLat, double toLon, Route::Mode mode, const RouteStore::Header &saveAs,
                int saveSlot);
// Reads a saved route back. It arrives as Done, as a fetched one does.
bool startLoadRoute(int slot);
RouteState routeState();
// The route, while the state is Done; null otherwise. Valid until clearRoute.
const Route::Result *routeResult();
// The slot the last route was saved to or loaded from; -1 if it couldn't be.
int routeSlot();
// Back to Idle once the route has been copied out.
void clearRoute();

// Saved routes on the card, newest first.
struct SavedRoute {
    int8_t slot;
    RouteStore::Header header;
};
enum class ListState : uint8_t { Idle, Queued, Done, Failed };
bool startListRoutes();
ListState listState();
int savedRoutes(const SavedRoute *&routes); // while Done
void clearList();
// Removed on the task's next turn, before any listing queued after it.
void deleteRoute(int slot);

// Downloads the active style's tiles along a saved route, at the zooms a drive is followed at, skipping any already on
// the card. Refused for tile servers whose policy forbids bulk downloading (OpenStreetMap's own).
enum class DownloadState : uint8_t { Idle, Queued, Running, Finished, Failed };
struct DownloadProgress {
    DownloadState state;
    uint32_t done, total, failed;
    const char *reason; // why it failed
};
bool startRouteDownload(int slot);
DownloadProgress downloadProgress();
void cancelDownload();
#endif

} // namespace NicheGraphics::MapTiles::Fetch

#endif
