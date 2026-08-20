#pragma once

#include <OLEDDisplay.h>
#include <stdint.h>
#include <string>

namespace graphics
{

// =======================
// Shared UI Helpers
// =======================

#define textZeroLine 0
// Consistent Line Spacing - this is standard for all display and the fall-back spacing
#define textFirstLine (FONT_HEIGHT_SMALL - 1)
#define textSecondLine (textFirstLine + (FONT_HEIGHT_SMALL - 5))
#define textThirdLine (textSecondLine + (FONT_HEIGHT_SMALL - 5))
#define textFourthLine (textThirdLine + (FONT_HEIGHT_SMALL - 5))
#define textFifthLine (textFourthLine + (FONT_HEIGHT_SMALL - 5))
#define textSixthLine (textFifthLine + (FONT_HEIGHT_SMALL - 5))

// Consistent Line Spacing for devices like T114 and TEcho/ThinkNode M1 of devices
#define textFirstLine_medium (FONT_HEIGHT_SMALL + 1 + BASEUI_HEADER_MARGIN)
#define textSecondLine_medium (textFirstLine_medium + FONT_HEIGHT_SMALL)
#define textThirdLine_medium (textSecondLine_medium + FONT_HEIGHT_SMALL)
#define textFourthLine_medium (textThirdLine_medium + FONT_HEIGHT_SMALL)
#define textFifthLine_medium (textFourthLine_medium + FONT_HEIGHT_SMALL)
#define textSixthLine_medium (textFifthLine_medium + FONT_HEIGHT_SMALL)

// Consistent Line Spacing for devices like VisionMaster T190
#define textFirstLine_large (FONT_HEIGHT_SMALL + 1)
#define textSecondLine_large (textFirstLine_large + (FONT_HEIGHT_SMALL + 5))
#define textThirdLine_large (textSecondLine_large + (FONT_HEIGHT_SMALL + 5))
#define textFourthLine_large (textThirdLine_large + (FONT_HEIGHT_SMALL + 5))
#define textFifthLine_large (textFourthLine_large + (FONT_HEIGHT_SMALL + 5))
#define textSixthLine_large (textFifthLine_large + (FONT_HEIGHT_SMALL + 5))

#ifndef BASEUI_HEADER_MARGIN
#define BASEUI_HEADER_MARGIN 0
#endif
#ifndef BASEUI_HEADER_LR_MARGIN
#define BASEUI_HEADER_LR_MARGIN 0
#endif
#ifndef BASEUI_BODY_LR_MARGIN
#define BASEUI_BODY_LR_MARGIN 0
#endif
#ifndef BASEUI_BELOW_HEADER_MARGIN
#define BASEUI_BELOW_HEADER_MARGIN 0
#endif
#ifndef ROUNDED_SCREEN
#define ROUNDED_SCREEN false
#endif
// Breathing room around the body text on the list-style screens (favorites, waypoint,
// telemetry): BASEUI_BODY_TOP_MARGIN adds to BASEUI_BELOW_HEADER_MARGIN above the first
// row, and BASEUI_BODY_LR_MARGIN insets it from the left edge. Kept separate from
// BASEUI_BELOW_HEADER_MARGIN because that one also sizes the bottom nav reserve and the
// node list rows.
#ifndef BASEUI_BODY_TOP_MARGIN
#define BASEUI_BODY_TOP_MARGIN 0
#endif
// Fixed number of frame icons the navigation bar shows per page. 0 fits as many as
// the usable width allows.
#ifndef BASEUI_NAV_ICONS_PER_PAGE
#define BASEUI_NAV_ICONS_PER_PAGE 0
#endif
// Navigation bar icon size as a percentage of what BASEUI_ICON_SCALE would otherwise
// give it, for variants that want the bar lighter than the rest of the artwork.
#ifndef BASEUI_NAV_ICON_SIZE_PCT
#define BASEUI_NAV_ICON_SIZE_PCT 100
#endif
// Trims (negative) or grows (positive) how many entry rows the node list screens fit.
#ifndef BASEUI_NODE_LIST_ROW_ADJUST
#define BASEUI_NODE_LIST_ROW_ADJUST 0
#endif
// When set, the favorite-node compass is sized from the full text budget instead of the
// rows a given node happens to fill, so it stays the same size across favorites.
#ifndef BASEUI_FIXED_COMPASS_SIZE
#define BASEUI_FIXED_COMPASS_SIZE 0
#endif
// Pulls the splash screen's corner text (region, version, short name) toward the centre
// by this percentage of the half-width, for screens whose rounded corners clip them.
#ifndef BASEUI_SPLASH_CORNER_INSET_PCT
#define BASEUI_SPLASH_CORNER_INSET_PCT 0
#endif
// Multiplier applied to every embedded bitmap (status icons, emotes, node/GPS
// glyphs, nav bar, logos) and to the layout offsets around them. Variants with a
// display far larger than the artwork was drawn for bump this up.
#ifndef BASEUI_ICON_SCALE
#define BASEUI_ICON_SCALE 1
#endif

// Quick screen access
#define SCREEN_WIDTH display->getWidth()
#define SCREEN_HEIGHT display->getHeight()

// Shared state (declare inside namespace)
extern bool hasUnreadMessage;
enum class ScreenResolution : uint8_t { UltraLow = 0, Low = 1, High = 2 };
extern ScreenResolution currentResolution;
ScreenResolution determineScreenResolution(int16_t screenheight, int16_t screenwidth);

void decomposeTime(uint32_t rtc_sec, int &hour, int &minute, int &second);

// Rounded highlight (used for inverted headers)
void drawRoundedHighlight(OLEDDisplay *display, int16_t x, int16_t y, int16_t w, int16_t h, int16_t r);

// Nearest-neighbour XBM blit. Falls through to the library's drawXbm() at scale 1,
// so callers can use it unconditionally. w/h are the bitmap's own dimensions; the
// drawn area is w*scale by h*scale.
void drawScaledXbm(OLEDDisplay *display, int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t *xbm,
                   int scale = BASEUI_ICON_SCALE);

// Nearest-neighbour XBM blit into an arbitrary destination box, for target sizes that
// aren't an integer multiple of the source. Identical output to drawScaledXbm() when
// destW/destH happen to be exact multiples of w/h.
void drawStretchedXbm(OLEDDisplay *display, int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t *xbm, int16_t destW,
                      int16_t destH);

// Shared battery/time/mail header
void drawCommonHeader(OLEDDisplay *display, int16_t x, int16_t y, const char *titleStr = "", bool force_no_invert = false,
                      bool show_date = false, bool transparent_background = false, bool use_title_color_override = false,
                      uint16_t title_color_override = 0);

// Shared battery/time/mail header
void drawCommonFooter(OLEDDisplay *display, int16_t x, int16_t y);

// Inline so non-compact boards fold this to a constant false at every call site, cost-free.
static inline bool isCompactPanel(OLEDDisplay *display)
{
#if defined(OLED_COMPACT_UI)
    // Covers both known compact panels (72x40 and 64x48).
    return display->getWidth() <= 80 && display->getHeight() <= 48;
#else
    (void)display;
    return false;
#endif
}

const int *getTextPositions(OLEDDisplay *display);

bool isAllowedPunctuation(char c);

std::string sanitizeString(const std::string &input);

static inline bool isAPIConnected(uint8_t state)
{
    static constexpr bool connectedStates[] = {
        /* STATE_NONE    */ false,
        /* STATE_BLE     */ true,
        /* STATE_WIFI    */ true,
        /* STATE_SERIAL  */ true,
        /* STATE_PACKET  */ true,
        /* STATE_HTTP    */ true,
        /* STATE_ETH     */ true,
    };
    return state < sizeof(connectedStates) ? connectedStates[state] : false;
}

} // namespace graphics
