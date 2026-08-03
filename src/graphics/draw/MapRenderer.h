#pragma once

// configuration.h is what defines BASEUI_HAS_MAP (see the opt-in block there) - include it before
// the guard below, or this whole header silently preprocesses away to nothing and every call site
// fails at link time instead of compile time. Same trap MapTileSourceSD.h documents for HAS_SDCARD.
#include "configuration.h"

#if BASEUI_HAS_MAP

#include "graphics/Screen.h"
#include <OLEDDisplay.h>
#include <OLEDDisplayUi.h>

namespace graphics
{

/**
 * @brief Map screen: baked-in basemap tiles (see NicheGraphics::MapTiles) plus known node
 * positions, with joystick pan/zoom. Shared by color TFT and E-Ink BaseUI variants - both draw
 * through the same OLEDDisplay primitive calls.
 *
 * A regular select press opens the Map's own menu (menuHandler::mapBaseMenu). On devices with real
 * directional input (HAS_DIRECTIONAL_INPUT - a keyboard, rotary encoder, touchscreen, or
 * trackball), Pan Mode and Zoom Level are both entered directly from that menu (not a picker) and
 * held until Back is pressed. Two-button-only devices (e.g. T-Beam 1W) have no direction to hold,
 * so they instead get mapPanMenu()/mapZoomLevelMenu() - discrete-option pickers that step the view
 * one direction/level per selection, navigable the same button-press way as every other menu.
 * Follow Me is a plain on/off toggle picker on every device.
 *
 * Opt-in at build time via -DBASEUI_HAS_MAP=1; off by default, and the flag additionally requires a
 * color-TFT or E-Ink display plus SD/portduino basemap storage - see configuration.h.
 */
namespace MapRenderer
{

void drawMapFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y);

// Pan Mode: entered directly from the menu, held until Back is pressed (see the guard in
// Screen::handleInputEvent) - not an enabled/disabled toggle. While active, the joystick's
// up/down/left/right pans the view instead of paging between frames. Off by default, so simply
// landing on the Map frame never traps the joystick. Mutually exclusive with Zoom Mode.
bool isPanModeEnabled();
void setPanModeEnabled(bool enabled);

// Joystick pan - nudges the view one step in a screen direction. Implicitly disables Follow Me,
// since otherwise the next redraw would immediately snap back to our own position.
void panUp();
void panDown();
void panLeft();
void panRight();

// Follow Me: while enabled (the default), the view re-centers on our own position (or the node
// centroid, if we have none) every frame. Disabling it freezes the view wherever it currently is.
bool isFollowMeEnabled();
void setFollowMeEnabled(bool enabled);

// Zoom Mode: entered directly from the menu, held until Back is pressed - not a discrete picker.
// While active, up/down adjust zoom by one level at a time and a zoom ruler is drawn on screen.
// Mutually exclusive with Pan Mode.
bool isZoomModeEnabled();
void setZoomModeEnabled(bool enabled);

int zoom();
void setZoom(int zoom);
void zoomIn();
void zoomOut();
constexpr int kMinZoom = 0;
constexpr int kMaxZoom = 18;

} // namespace MapRenderer

} // namespace graphics

#endif // BASEUI_HAS_MAP
