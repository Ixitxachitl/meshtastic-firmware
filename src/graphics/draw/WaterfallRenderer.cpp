#include "configuration.h"
#include "graphics/TFTColorRegions.h"

#if defined(USE_SX1262) && GRAPHICS_TFT_COLORING_ENABLED

#include "PowerFSM.h"
#include "WaterfallRenderer.h"
#include "concurrency/LockGuard.h"
#include "graphics/ScreenFonts.h"
#include "graphics/SharedUIDisplay.h"
#include "graphics/TFTPalette.h"
#include "graphics/draw/UIRenderer.h"
#include "mesh/RadioLibInterface.h"

#include <SPI.h>

// SPI1 is the HSPI bus used by the ST7789 display, defined in Screen.cpp (inside namespace graphics)
namespace graphics
{
extern SPIClass SPI1;
}

// ST7789 address window offsets: physical chip resolution is 320×240,
// but only 240×135 pixels are used, offset by (40, 52).
static constexpr uint16_t ST7789_X_OFFSET = 40; // (320 - TFT_WIDTH) / 2
static constexpr uint16_t ST7789_Y_OFFSET = 52; // (240 - TFT_HEIGHT) / 2

// ST7789 commands — ST77XX_CASET/RASET/RAMWR come in as macros from ST7789Spi.h
// via graphics/Screen.h → UIRenderer.h, so don't redefine them here.
#ifndef ST77XX_CASET
#define ST77XX_CASET 0x2A
#endif
#ifndef ST77XX_RASET
#define ST77XX_RASET 0x2B
#endif
#ifndef ST77XX_RAMWR
#define ST77XX_RAMWR 0x2C
#endif

namespace graphics
{

WaterfallRenderer *WaterfallRenderer::instance = nullptr;
std::atomic<bool> WaterfallRenderer::active_{false};

WaterfallRenderer::WaterfallRenderer() : OSThread("WaterfallRenderer", 1500)
{
    instance = this;
}

int32_t WaterfallRenderer::runOnce()
{
    if (!active_.exchange(false, std::memory_order_acq_rel)) {
        // Frame not visible — release radio hold and kick the radio back into duty-cycle RX immediately.
        if (RadioLibInterface::spectralScanHoldRadio) {
            RadioLibInterface::spectralScanHoldRadio = false;
            RadioLibInterface::spectralScanRequest = false;
            RadioLibInterface::triggerSpectralScan(); // wake radio → startReceive() → startReceiveDutyCycleAuto()
        }
        return 500;
    }
    doScan();
#if !MESHTASTIC_EXCLUDE_POWER_FSM
    powerFSM.trigger(EVENT_RECEIVED_MSG); // keep screen awake while waterfall is visible
#endif
    // Sweep itself takes ~50 ms in the radio task (33 bins × ~1.5 ms each).
    // Returning 80 ms here gives ~12 Hz waterfall scroll while leaving the radio time to finish.
    return 80;
}

void WaterfallRenderer::doScan()
{
    if (!RadioLibInterface::instance)
        return;

    // Helper: map packet RSSI [−110..−40 dBm] to colour index [0..255], same scale as doFrequencySweep().
    auto rssiToN = [](int16_t rssiDbm) -> uint8_t {
        int r = rssiDbm;
        if (r < -110)
            r = -110;
        if (r > -40)
            r = -40;
        return static_cast<uint8_t>(((r + 110) * 936U) >> 8);
    };

    // If a packet was received (by the normal duty-cycle RX path), inject a row showing the signal
    // centered at the channel frequency with an approximate LoRa bandwidth envelope.
    if (RadioLibInterface::spectralScanPacketReceived) {
        RadioLibInterface::spectralScanPacketReceived = false;
        uint8_t sigN = rssiToN(RadioLibInterface::spectralScanLastPacketRSSI);
        concurrency::LockGuard g(&lock_);
        uint8_t slot = head_.load(std::memory_order_relaxed);
        for (uint8_t i = 0; i < WATERFALL_BINS; i++) {
            // Distance from center bin — shape approximates a LoRa ~250 kHz signal within a 1 MHz sweep.
            uint8_t d = (i >= WATERFALL_BINS / 2) ? (i - WATERFALL_BINS / 2) : (WATERFALL_BINS / 2 - i);
            histBuf_[slot][i] = (d <= 4) ? sigN : (d <= 7) ? static_cast<uint8_t>(sigN >> 1) : 0;
        }
        head_.store((slot + 1) % WATERFALL_ROWS, std::memory_order_release);
    }

    // Collect results from the frequency sweep the radio thread completed since our last call.
    if (RadioLibInterface::spectralScanReady) {
        RadioLibInterface::spectralScanReady = false;

        concurrency::LockGuard g(&lock_);
        uint8_t slot = head_.load(std::memory_order_relaxed);
        for (uint8_t i = 0; i < WATERFALL_BINS; i++) {
            // spectralScanResultsBuf[i] is RSSI mapped to [0..65535].
            // Shift right 8 bits to get a 0–255 value suitable for the colour ramp.
            histBuf_[slot][i] = static_cast<uint8_t>(RadioLibInterface::spectralScanResultsBuf[i] >> 8);
        }
        head_.store((slot + 1) % WATERFALL_ROWS, std::memory_order_release);
    }

    // Do NOT hold the radio — let it return to duty-cycle RX between sweeps so packets are received normally.
    RadioLibInterface::spectralScanHoldRadio = false;
    RadioLibInterface::spectralScanRequest = true;
    RadioLibInterface::triggerSpectralScan();
}

// Heatmap: dark navy (noise floor ≤ −110 dBm) → blue → cyan → green → yellow → red (≥ −40 dBm)
// n=0 maps to −110 dBm, n=255 maps to −40 dBm (≈0.27 dB/step).
uint16_t WaterfallRenderer::countToRgb565(uint8_t n)
{
    uint8_t r, g, b;
    if (n == 0) {
        // No counts in this bin: dark navy background (visually present but clearly "quiet")
        r = 0;
        g = 0;
        b = 32;
    } else if (n < 32) {
        r = 0;
        g = 0;
        b = n * 4;
    } else if (n < 96) {
        // blue ramp
        r = 0;
        g = 0;
        b = 128 + (n - 32) * 2;
    } else if (n < 144) {
        // blue → cyan
        r = 0;
        g = (n - 96) * 5;
        b = 255;
    } else if (n < 192) {
        // cyan → green
        r = 0;
        g = 255;
        b = 255 - (n - 144) * 5;
    } else if (n < 224) {
        // green → yellow
        r = (n - 192) * 8;
        g = 255;
        b = 0;
    } else {
        // yellow → red
        r = 255;
        g = 255 - (n - 224) * 8;
        b = 0;
    }
    return TFTPalette::rgb565(r, g, b);
}

void WaterfallRenderer::drawWaterfallFrame(OLEDDisplay *display, OLEDDisplayUiState * /*state*/, int16_t x, int16_t y)
{
    drawCommonHeader(display, x, y, "Waterfall");

    // Footer: left = sweep start freq, center = "MHz", right = sweep end freq.
    // Use FONT_SMALL_LOCAL (ArialMT_Plain_10, 13 px) so the footer is as compact as possible.
    uint32_t centerKHz = RadioLibInterface::spectralScanCenterFreqKHz;
    if (centerKHz > 0) {
        char left[10], right[10];
        char center[16];
        snprintf(left, sizeof(left), "%.3f", (centerKHz - WATERFALL_SWEEP_HALF_KHZ) / 1000.0f);
        snprintf(center, sizeof(center), "%.3f MHz", centerKHz / 1000.0f);
        snprintf(right, sizeof(right), "%.3f", (centerKHz + WATERFALL_SWEEP_HALF_KHZ) / 1000.0f);
        const int16_t footerH = _fontHeight(FONT_SMALL_LOCAL); // 13 px
        const int16_t footerY = y + (display->getHeight() - footerH);
        display->setFont(FONT_SMALL_LOCAL);
        display->setColor(WHITE);
        display->setTextAlignment(TEXT_ALIGN_LEFT);
        display->drawString(x, footerY, left);
        display->setTextAlignment(TEXT_ALIGN_CENTER);
        display->drawString(x + display->getWidth() / 2, footerY, center);
        display->setTextAlignment(TEXT_ALIGN_RIGHT);
        display->drawString(x + display->getWidth(), footerY, right);
    }
}

void WaterfallRenderer::postDraw()
{
    if (!instance)
        return;

    // Snapshot the ring-buffer head under lock, then release before the long SPI transfer
    uint8_t rowBuf[WATERFALL_W * 2]; // 480 bytes on stack
    const SPISettings settings(40000000, MSBFIRST, SPI_MODE0);

    // When the nav bar overlay is visible (~2 s after a frame switch) skip painting
    // the bottom rows that overlap the icon strip so the nav bar stays on top.
    // Nav bar icons are 16 px tall and sit at logical Y = SCREEN_HEIGHT - iconSize - 1 = 118.
    uint8_t rowsToPaint = WATERFALL_ROWS;
    if (UIRenderer::isNavigationBarVisible()) {
        const uint8_t navTopY = 118; // logical Y where the nav bar begins on TFT (135 - 16 - 1)
        if (navTopY > WATERFALL_Y && (navTopY - WATERFALL_Y) < rowsToPaint)
            rowsToPaint = navTopY - WATERFALL_Y;
    }

    const uint16_t x1 = WATERFALL_X + ST7789_X_OFFSET;
    const uint16_t y1 = WATERFALL_Y + ST7789_Y_OFFSET;
    const uint16_t x2 = x1 + WATERFALL_W - 1;
    const uint16_t y2 = y1 + rowsToPaint - 1;

    // Build pixel data and send in one SPI window for efficiency
    digitalWrite(ST7789_NSS, LOW);
    SPI1.beginTransaction(settings);

    // Set column address window
    digitalWrite(ST7789_RS, LOW);
    SPI1.write(ST77XX_CASET);
    digitalWrite(ST7789_RS, HIGH);
    uint8_t caset[4] = {(uint8_t)(x1 >> 8), (uint8_t)x1, (uint8_t)(x2 >> 8), (uint8_t)x2};
    SPI1.writeBytes(caset, 4);

    // Set row address window
    digitalWrite(ST7789_RS, LOW);
    SPI1.write(ST77XX_RASET);
    digitalWrite(ST7789_RS, HIGH);
    uint8_t raset[4] = {(uint8_t)(y1 >> 8), (uint8_t)y1, (uint8_t)(y2 >> 8), (uint8_t)y2};
    SPI1.writeBytes(raset, 4);

    // Begin RAM write
    digitalWrite(ST7789_RS, LOW);
    SPI1.write(ST77XX_RAMWR);
    digitalWrite(ST7789_RS, HIGH);

    // Paint rows newest-at-top.  We hold the lock only to read head_, then
    // read the buffer without locking (a torn newest row is visually harmless).
    uint8_t head;
    {
        concurrency::LockGuard g(&instance->lock_);
        head = instance->head_.load(std::memory_order_acquire);
    }

    for (uint8_t row = 0; row < rowsToPaint; row++) {
        // newest scan first: (head - 1 - row + ROWS) % ROWS
        uint8_t bufIdx = (head + WATERFALL_ROWS - 1 - row) % WATERFALL_ROWS;
        const uint8_t *hist = instance->histBuf_[bufIdx];

        // Expand 33 bins → 240 pixels (nearest-neighbour)
        uint8_t *p = rowBuf;
        for (uint16_t px = 0; px < WATERFALL_W; px++) {
            uint8_t bin = (uint32_t)px * WATERFALL_BINS / WATERFALL_W;
            uint16_t color16 = countToRgb565(hist[bin]);
            *p++ = (uint8_t)(color16 >> 8);
            *p++ = (uint8_t)(color16 & 0xFF);
        }
        SPI1.writeBytes(rowBuf, WATERFALL_W * 2);
    }

    SPI1.endTransaction();
    digitalWrite(ST7789_NSS, HIGH);

    // Mark frame visible so runOnce() continues scanning.
    instance->active_.store(true, std::memory_order_relaxed);
}

} // namespace graphics

#endif // USE_SX1262 && GRAPHICS_TFT_COLORING_ENABLED
