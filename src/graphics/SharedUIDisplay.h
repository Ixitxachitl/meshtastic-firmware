#pragma once

#include <OLEDDisplay.h>
#include <string>

extern OLEDDisplay *display;

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
#define textFirstLine_medium (FONT_HEIGHT_SMALL + 1)
#define textSecondLine_medium (textFirstLine_medium + FONT_HEIGHT_SMALL)
#define textThirdLine_medium (textSecondLine_medium + FONT_HEIGHT_SMALL)
#define textFourthLine_medium (textThirdLine_medium + FONT_HEIGHT_SMALL)
#define textFifthLine_medium (textFourthLine_medium + FONT_HEIGHT_SMALL)
#define textSixthLine_medium (textFifthLine_medium + FONT_HEIGHT_SMALL)

// Consistent Line Spacing for M5Stack UnitC6L with FONT_TINY (Tom Thumb 3x6 font)
// FONT_HEIGHT_TINY is 7 (6px font + 1px gap calculated by _fontHeight macro)
#if defined(M5STACK_UNITC6L) || defined(USE_TINY_FONT)
#define textFirstLine_unitc6l (FONT_HEIGHT_TINY + 4) // 11 pixels (header + spacing)
#define textSecondLine_unitc6l (textFirstLine_unitc6l + FONT_HEIGHT_TINY)
#define textThirdLine_unitc6l (textSecondLine_unitc6l + FONT_HEIGHT_TINY)
#define textFourthLine_unitc6l (textThirdLine_unitc6l + FONT_HEIGHT_TINY)
#define textFifthLine_unitc6l (textFourthLine_unitc6l + FONT_HEIGHT_TINY)
#define textSixthLine_unitc6l (textFifthLine_unitc6l + FONT_HEIGHT_TINY)
#endif

// Consistent Line Spacing for devices like VisionMaster T190
#define textFirstLine_large (FONT_HEIGHT_SMALL + 1)
#define textSecondLine_large (textFirstLine_large + (FONT_HEIGHT_SMALL + 5))
#define textThirdLine_large (textSecondLine_large + (FONT_HEIGHT_SMALL + 5))
#define textFourthLine_large (textThirdLine_large + (FONT_HEIGHT_SMALL + 5))
#define textFifthLine_large (textFourthLine_large + (FONT_HEIGHT_SMALL + 5))
#define textSixthLine_large (textFifthLine_large + (FONT_HEIGHT_SMALL + 5))

// Quick screen access
#define SCREEN_WIDTH display->getWidth()
#define SCREEN_HEIGHT display->getHeight()

// Shared state (declare inside namespace)
extern bool hasUnreadMessage;
enum class ScreenResolution : uint8_t { UltraLow = 0, Low = 1, High = 2 };
extern ScreenResolution currentResolution;
// Convenience alias for legacy code - true if High resolution
inline bool isHighResolution()
{
    return currentResolution == ScreenResolution::High;
}
ScreenResolution determineScreenResolution(int16_t screenheight, int16_t screenwidth);

void decomposeTime(uint32_t rtc_sec, int &hour, int &minute, int &second);

// Rounded highlight (used for inverted headers)
void drawRoundedHighlight(OLEDDisplay *display, int16_t x, int16_t y, int16_t w, int16_t h, int16_t r);

// Shared battery/time/mail header
void drawCommonHeader(OLEDDisplay *display, int16_t x, int16_t y, const char *titleStr = "", bool force_no_invert = false,
                      bool show_date = false);

// Shared battery/time/mail header
void drawCommonFooter(OLEDDisplay *display, int16_t x, int16_t y);

const int *getTextPositions(OLEDDisplay *display);

bool isAllowedPunctuation(char c);

std::string sanitizeString(const std::string &input);

void setMessagesScreenActive(bool active);
bool isMessagesScreenActive();

void setMessagesFrameIndex(int idx);
int getMessagesFrameIndex();

void setEnvTelemetryScreenActive(bool active);
bool isEnvTelemetryScreenActive();

void setEnvTelemetryFrameIndex(int idx);
int getEnvTelemetryFrameIndex();

// Centralized overlay state (menus, pickers, banners)
void setOverlayActive(bool active);
bool isOverlayActive();
} // namespace graphics
