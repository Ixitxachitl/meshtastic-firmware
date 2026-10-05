#pragma once

#include "configuration.h"

#if BASEUI_MAP_PNG_TILES

#include "./MapViewPose.h"
#include <stdint.h>

// Colour basemap in device-ui's tile layout: 256px PNGs at /maps/<style>/<z>/<x>/<y>.png, or /map/<z>/<x>/<y>.png
// when there are no style folders. Read from the SD card and decoded with PNGdec, or JPEGDEC for a JPEG under the
// same name when BASEUI_MAP_JPEG_TILES is on. Display task only.
namespace NicheGraphics::MapTiles::Png
{

constexpr int kMaxStyles = 16;
constexpr int kTileSize = 256;

// Rescans the card for style folders and selects `preferred` if present, else the first. Returns the count.
int refreshStyles(const char *preferred);
#if BASEUI_MAP_ONLINE_TILES && !defined(SENSECAP_INDICATOR)
// Writes /maps/osm/.url (OpenStreetMap) so a card with no map yet can fetch tiles. False with no card or on a write error.
bool seedDefaultStyle();
#endif
int styleCount();
// Folder name under /maps, or "" for the bare /map tree.
const char *styleName(int index);
// -1 when the card has no PNG tiles.
int activeStyle();
void setActiveStyle(int index);
// Changes whenever the same view would render differently.
uint32_t generation();

// A tile the reader reported missing has since been downloaded: forget the miss and invalidate the cached
// view, so the next draw picks it up instead of the fallback it scaled from a lower zoom.
void noteTileArrived(int z, int32_t x, int32_t y);

// Fills dst (w*h native-endian RGB565) with the view whose centre column/row is world pixel (centerX, centerY)
// at `zoom`. A missing tile is scaled up from a lower zoom when one exists, else filled with bg.
void renderView(uint16_t *dst, int16_t w, int16_t h, int32_t centerX, int32_t centerY, int zoom, uint16_t bg);

// As renderView, but through a turned (and maybe tilted) camera. Rows past the horizon get `sky`. Where the ground is
// shrunk, a lower zoom's tiles are sampled instead, which is both cheaper and steadier than skipping pixels.
void renderViewPosed(uint16_t *dst, int16_t w, int16_t h, const ViewPose &pose, int zoom, uint16_t bg, uint16_t sky);

} // namespace NicheGraphics::MapTiles::Png

#endif
