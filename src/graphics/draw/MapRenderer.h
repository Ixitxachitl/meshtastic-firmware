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
 * Where BASEUI_MAP_ONSCREEN_CONTROLS is set, all of that is instead four buttons drawn on the frame
 * itself and there is no Map menu at all - see handleControlTap(). Everywhere else:
 *
 * A regular select press opens the Map's own menu (menuHandler::mapBaseMenu). On devices with real
 * directional input (HAS_DIRECTIONAL_INPUT - a keyboard, rotary encoder, touchscreen, or
 * trackball), Pan Mode and Zoom Level are both entered directly from that menu (not a picker) and
 * held until Back is pressed. Two-button-only devices have no direction to hold,
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

// Pan Mode: while active, the joystick's up/down/left/right pans the view instead of paging between
// frames, and a finger drags the map directly. Off by default, so simply landing on the Map frame
// never traps the joystick. Mutually exclusive with Zoom Mode.
//
// Reached from the menu on most boards, and held until Back is pressed (see the guard in
// Screen::handleInputEvent) rather than being an enabled/disabled toggle. Under
// BASEUI_MAP_ONSCREEN_CONTROLS it is the PAN button and behaves as the plain latch it looks like.
bool isPanModeEnabled();
void setPanModeEnabled(bool enabled);

// Joystick pan - nudges the view one step in a screen direction. Implicitly disables Follow Me,
// since otherwise the next redraw would immediately snap back to our own position.
void panUp();
void panDown();
void panLeft();
void panRight();

// Touch pan - moves the map by an exact finger displacement in screen pixels, for hardware that
// reports a continuous drag (BASEUI_HAS_TOUCH_DRAG). Pass the delta between consecutive drag
// reports, not the offset from where the finger landed.
//
// The map follows the finger, which is the opposite sense to the joystick calls above: a press
// means "move the view that way", a finger means "move the map that way". Also disables Follow Me.
void panByFingerDelta(float dxPx, float dyPx);

// Follow Me: while enabled (the default), the view re-centers on our own position (or the node
// centroid, if we have none) every frame. Disabling it freezes the view wherever it currently is.
bool isFollowMeEnabled();
void setFollowMeEnabled(bool enabled);

// Zoom Mode: entered directly from the menu, held until Back is pressed - not a discrete picker.
// While active, up/down adjust zoom by one level at a time and a zoom ruler is drawn on screen.
// Mutually exclusive with Pan Mode. Unreachable under BASEUI_MAP_ONSCREEN_CONTROLS, where the two
// zoom buttons step the level directly and there is no menu to enter a mode from.
bool isZoomModeEnabled();
void setZoomModeEnabled(bool enabled);

#if BASEUI_MAP_ONSCREEN_CONTROLS
// On-screen controls: a column of key caps down the right edge - zoom in, zoom out, and the Pan
// and Follow Me toggles, styled like the on-screen keyboard's keys. Coordinates are the panel's,
// as the touch layer reports them. Returns true if the tap landed on a button and was acted on,
// false to leave the tap to whatever else wanted it - including before the frame's first draw,
// when there are no buttons on screen to hit.
bool handleControlTap(int16_t tapX, int16_t tapY);
#endif

int zoom();
void setZoom(int zoom);
void zoomIn();
void zoomOut();
constexpr int kMinZoom = 0;
constexpr int kMaxZoom = 18;

} // namespace MapRenderer

} // namespace graphics

#endif // BASEUI_HAS_MAP
