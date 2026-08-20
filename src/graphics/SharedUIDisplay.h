#pragma once

#include <OLEDDisplay.h>
#include <OLEDDisplayUi.h>
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

// Shared battery/time/mail header
void drawCommonHeader(OLEDDisplay *display, int16_t x, int16_t y, const char *titleStr = "", bool force_no_invert = false,
                      bool show_date = false, bool transparent_background = false, bool use_title_color_override = false,
                      uint16_t title_color_override = 0);

// Shared battery/time/mail header
void drawCommonFooter(OLEDDisplay *display, int16_t x, int16_t y);

// Frame renderers must clear through this rather than calling display->clear() directly.
//
// While OLEDDisplayUi is IN_TRANSITION it draws *two* frames into the same buffer before
// committing: the outgoing one first at a sliding offset, then the incoming one on top. A frame
// that clears unconditionally therefore wipes the outgoing frame that was just drawn, so the
// transition renders as a blank screen instead of two frames sliding past each other. Only the
// first draw of a cycle owns the clear - the outgoing frame mid-transition, or the current frame
// when there is no transition at all (relationship NONE).
//
// Inline so this stays free: with transitions off the branch folds away to the original clear.
static inline void clearForFrame(OLEDDisplay *display, const OLEDDisplayUiState *state)
{
    if (!state || state->transitionFrameRelationship != TransitionRelationship_INCOMING)
        display->clear();
}

// The frame index this callback is drawing for.
//
// state->currentFrame names the *outgoing* frame for the entire duration of a transition, so any
// frame that identifies itself from it - to pick which module, which favourite node, which page -
// picks the wrong one while it is sliding in, and typically bails out and draws nothing until the
// transition completes. transitionFrameTarget is only maintained by nextFrame(), never by
// previousFrame(), so going backwards the incoming index has to be derived from the direction.
// frameCount resolves the backwards wrap off frame 0, whose incoming frame is the *last* one.
// Callers that identify themselves by index must pass it. The 0 default keeps the old, wrong answer
// rather than a plausible-looking one, so a missing argument shows up as the same blank frame
// instead of silently drawing the wrong content.
static inline uint8_t frameIndexFor(const OLEDDisplayUiState *state, size_t frameCount = 0)
{
    if (!state)
        return 0;
    if (state->frameState == IN_TRANSITION && state->transitionFrameRelationship == TransitionRelationship_INCOMING) {
        if (state->frameTransitionDirection < 0) {
            if (state->currentFrame > 0)
                return state->currentFrame - 1;
            // Wrapping backwards from the first frame lands on the last. Forward wrap needs no
            // special case: nextFrame() stores the wrapped index in transitionFrameTarget, which is
            // why last-to-first always worked and first-to-last did not.
            return frameCount > 0 ? (uint8_t)(frameCount - 1) : state->currentFrame;
        }
        return state->transitionFrameTarget;
    }
    return state->currentFrame;
}

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
