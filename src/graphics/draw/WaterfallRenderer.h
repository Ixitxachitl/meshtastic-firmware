#pragma once

#include "graphics/TFTColorRegions.h"
#if defined(USE_SX1262) && GRAPHICS_TFT_COLORING_ENABLED

#include "concurrency/Lock.h"
#include "concurrency/OSThread.h"
#include <OLEDDisplay.h>
#include <OLEDDisplayUi.h>
#include <atomic>

namespace graphics
{

// Number of spectral-scan result bins — must equal RadioLibInterface::SPECTRAL_SCAN_BINS.
static constexpr uint8_t WATERFALL_BINS = 60;
// Height of the scrolling history in pixels (also the number of stored rows).
// 135 total − 20 px header − 1 px gap − 13 px footer (FONT_SMALL_LOCAL) = 101 rows.
static constexpr uint8_t WATERFALL_ROWS = 101;
// Screen-space origin of the waterfall area (below the title bar)
static constexpr uint8_t WATERFALL_X = 0;
static constexpr uint8_t WATERFALL_Y = 21; // header is 20 px (FONT_HEIGHT_SMALL-1+2), plus 1 px gap
static constexpr uint8_t WATERFALL_W = 240;

/**
 * WaterfallRenderer — async spectrum scanner + TFT waterfall display.
 *
 * runOnce() collects a 33-bin RSSI histogram from the SX126x spectral scan
 * and pushes it into a ring buffer.  postDraw() is called by Screen::runOnce()
 * immediately after updateUiFrame() and paints the colored waterfall directly
 * onto the ST7789 via SPI, overlaying the monochrome title drawn by the
 * standard OLEDDisplay frame callback.
 */
class WaterfallRenderer : public concurrency::OSThread
{
  public:
    static WaterfallRenderer *instance;

    WaterfallRenderer();

    // Frame callback registered with the Screen frame rotation system
    static void drawWaterfallFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y);

    // Called by Screen::runOnce() AFTER updateUiFrame() to paint colored pixels
    static void postDraw();

    // Map a normalised count (0–255) to an RGB565 heatmap colour.
    // Public so the palette-LUT builder in the .cpp can call it once at startup.
    static uint16_t countToRgb565(uint8_t normalised);

  protected:
    int32_t runOnce() override;

  private:
    // Ring buffer of normalised per-bin counts (0–255).  head_ is the *next write* index.
    uint8_t histBuf_[WATERFALL_ROWS][WATERFALL_BINS]{};
    std::atomic<uint8_t> head_{0};
    concurrency::Lock lock_;

    // Last head_ value painted by postDraw(), used to skip SPI work when nothing changed.
    // Initialised to a sentinel that can't equal a real head value so the first paint always runs.
    uint8_t lastPaintedHead_{0xFF};

    void doScan();

    // Set true by postDraw() each time the frame is painted; consumed (exchanged) by runOnce().
    static std::atomic<bool> active_;
};

} // namespace graphics

#endif // USE_SX1262 && GRAPHICS_TFT_COLORING_ENABLED
