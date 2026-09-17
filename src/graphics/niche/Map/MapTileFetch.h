#pragma once

#include "configuration.h"

#if BASEUI_MAP_ONLINE_TILES

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

} // namespace NicheGraphics::MapTiles::Fetch

#endif
