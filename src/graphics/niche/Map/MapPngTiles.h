#pragma once

#include "configuration.h"

#if BASEUI_MAP_PNG_TILES

#include <stdint.h>

// Colour basemap in device-ui's tile layout: 256px PNGs at /maps/<style>/<z>/<x>/<y>.png, or /map/<z>/<x>/<y>.png
// when there are no style folders. Read from the SD card and decoded with PNGdec. Display task only.
namespace NicheGraphics::MapTiles::Png
{

constexpr int kMaxStyles = 16;
constexpr int kTileSize = 256;

// Rescans the card for style folders and selects `preferred` if present, else the first. Returns the count.
int refreshStyles(const char *preferred);
int styleCount();
// Folder name under /maps, or "" for the bare /map tree.
const char *styleName(int index);
// -1 when the card has no PNG tiles.
int activeStyle();
void setActiveStyle(int index);
// Changes whenever the same view would render differently.
uint32_t generation();

// Fills dst (w*h native-endian RGB565) with the view whose centre column/row is world pixel (centerX, centerY)
// at `zoom`. A missing tile is scaled up from a lower zoom when one exists, else filled with bg.
void renderView(uint16_t *dst, int16_t w, int16_t h, int32_t centerX, int32_t centerY, int zoom, uint16_t bg);

} // namespace NicheGraphics::MapTiles::Png

#endif
