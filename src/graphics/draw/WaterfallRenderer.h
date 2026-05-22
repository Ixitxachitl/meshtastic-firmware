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

// Number of spectral-scan result bins from SX126x (RADIOLIB_SX126X_SPECTRAL_SCAN_RES_SIZE)
static constexpr uint8_t WATERFALL_BINS = 33;
// Half-width of the frequency sweep in kHz (must match SWEEP_HALF_MHZ in doFrequencySweep()).
static constexpr uint32_t WATERFALL_SWEEP_HALF_KHZ = 5000; // ±5 MHz = 10 MHz total
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

  protected:
    int32_t runOnce() override;

  private:
    // Ring buffer of normalised per-bin counts (0–255).  head_ is the *next write* index.
    uint8_t histBuf_[WATERFALL_ROWS][WATERFALL_BINS]{};
    std::atomic<uint8_t> head_{0};
    concurrency::Lock lock_;

    void doScan();

    // Map a normalised count (0–255) to an RGB565 heatmap colour
    static uint16_t countToRgb565(uint8_t normalised);

    // Set true by postDraw() each time the frame is painted; consumed (exchanged) by runOnce().
    static std::atomic<bool> active_;
};

} // namespace graphics

#endif // USE_SX1262 && GRAPHICS_TFT_COLORING_ENABLED
