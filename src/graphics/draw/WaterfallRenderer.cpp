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
    // Scan continuously, even when the waterfall frame isn't visible or the screen is off.
    // This lets the renderer keep histogramming RF activity in the background so the user sees
    // recent history the moment they switch to the waterfall frame.
    const bool visible = active_.exchange(false, std::memory_order_acq_rel);
    doScan();
#if !MESHTASTIC_EXCLUDE_POWER_FSM
    if (visible)
        powerFSM.trigger(EVENT_RECEIVED_MSG); // keep screen awake while waterfall is being watched
#endif
    // LoRa preamble + header at LONG_FAST (SF11/BW250) takes >100 ms; a packet runs 1–2 s.
    // Returning 2000 ms gives ~98% contiguous RX time between sweeps so we don't shred packet RX,
    // while still updating the waterfall ~30 rows/min plus an immediate blip whenever a packet lands.
    return 2000;
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
    // at the channel frequency's bin with an approximate LoRa bandwidth envelope.
    if (RadioLibInterface::spectralScanPacketReceived) {
        RadioLibInterface::spectralScanPacketReceived = false;
        uint8_t sigN = rssiToN(RadioLibInterface::spectralScanLastPacketRSSI);

        // Map channel freq → bin index within the current sweep range.
        const uint32_t startKHz = RadioLibInterface::spectralScanStartFreqKHz;
        const uint32_t endKHz = RadioLibInterface::spectralScanEndFreqKHz;
        const uint32_t chKHz = (uint32_t)(RadioLibInterface::instance->getFreq() * 1000.0f + 0.5f);
        uint8_t centerBin = WATERFALL_BINS / 2;
        if (endKHz > startKHz && chKHz >= startKHz && chKHz <= endKHz) {
            centerBin = static_cast<uint8_t>(((uint64_t)(chKHz - startKHz) * (WATERFALL_BINS - 1)) / (endKHz - startKHz));
        }

        concurrency::LockGuard g(&lock_);
        uint8_t slot = head_.load(std::memory_order_relaxed);
        for (uint8_t i = 0; i < WATERFALL_BINS; i++) {
            // Distance from channel-freq bin — shape approximates a LoRa ~250 kHz signal envelope.
            uint8_t d = (i >= centerBin) ? (i - centerBin) : (centerBin - i);
            histBuf_[slot][i] = (d <= 1) ? sigN : (d <= 2) ? static_cast<uint8_t>(sigN >> 1) : 0;
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

// Pre-baked palette: 256 RGB565 colours stored as already-byte-swapped (big-endian) byte pairs
// so the inner paint loop is two raw byte writes per pixel with no shifts.
static uint8_t s_paletteBE[256][2];
static bool s_paletteReady = false;
static void buildPaletteIfNeeded()
{
    if (s_paletteReady)
        return;
    for (int n = 0; n < 256; n++) {
        uint16_t c = WaterfallRenderer::countToRgb565(static_cast<uint8_t>(n));
        s_paletteBE[n][0] = static_cast<uint8_t>(c >> 8);
        s_paletteBE[n][1] = static_cast<uint8_t>(c & 0xFF);
    }
    s_paletteReady = true;
}

// Per-bin pixel span: s_binStart[i]..s_binStart[i+1] is the pixel range source bin i covers.
// 33 bins over 240 pixels → ~7-8 px each; lets the paint loop avoid a per-pixel divide.
static uint8_t s_binStart[WATERFALL_BINS + 1];
static bool s_binStartReady = false;
static void buildBinSpansIfNeeded()
{
    if (s_binStartReady)
        return;
    // Match the original nearest-neighbour mapping `bin = px * BINS / W`:
    // bin i covers pixels [ceil(i*W/BINS), ceil((i+1)*W/BINS)).
    for (int i = 0; i <= WATERFALL_BINS; i++) {
        s_binStart[i] = static_cast<uint8_t>((i * WATERFALL_W + WATERFALL_BINS - 1) / WATERFALL_BINS);
    }
    s_binStartReady = true;
}

void WaterfallRenderer::drawWaterfallFrame(OLEDDisplay *display, OLEDDisplayUiState * /*state*/, int16_t x, int16_t y)
{
    // The OLEDDisplayUi framebuffer flush that follows this call wipes the waterfall body region
    // to black (we only draw the header/footer text into the framebuffer). Force postDraw to
    // repaint the body on top of that fresh-zeroed area, even if no new scan data has landed.
    if (instance)
        instance->lastPaintedHead_ = 0xFF;

    drawCommonHeader(display, x, y, "Waterfall");

    // Footer: left = sweep start freq, center = "MHz", right = sweep end freq.
    // Use FONT_SMALL_LOCAL (ArialMT_Plain_10, 13 px) so the footer is as compact as possible.
    uint32_t startKHz = RadioLibInterface::spectralScanStartFreqKHz;
    uint32_t endKHz = RadioLibInterface::spectralScanEndFreqKHz;
    uint32_t centerKHz = RadioLibInterface::spectralScanCenterFreqKHz;
    if (centerKHz > 0 && endKHz > startKHz) {
        char left[10], right[10];
        char center[16];
        snprintf(left, sizeof(left), "%.3f", startKHz / 1000.0f);
        snprintf(center, sizeof(center), "%.3f MHz", centerKHz / 1000.0f);
        snprintf(right, sizeof(right), "%.3f", endKHz / 1000.0f);
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

    // Mark frame visible so runOnce() continues scanning, regardless of whether we paint below.
    // Doing this first means a skipped repaint still keeps the scan loop alive.
    instance->active_.store(true, std::memory_order_relaxed);

    // Atomic load — no lock needed for a single uint8_t atomic read.
    uint8_t head = instance->head_.load(std::memory_order_acquire);

    // Skip the entire SPI transaction when no new scan data has landed since the last paint.
    // postDraw() is called every UI frame (~30 Hz) but scans complete at ~12 Hz, so most calls
    // would otherwise repaint identical pixels (~48 KB SPI / frame wasted).
    if (head == instance->lastPaintedHead_)
        return;
    instance->lastPaintedHead_ = head;

    buildPaletteIfNeeded();
    buildBinSpansIfNeeded();

    static const SPISettings settings(40000000, MSBFIRST, SPI_MODE0);

    // When the nav bar overlay is visible (~2 s after a frame switch) skip painting
    // the bottom rows that overlap the icon strip so the nav bar stays on top.
    uint8_t rowsToPaint = WATERFALL_ROWS;
    if (UIRenderer::isNavigationBarVisible()) {
        const uint8_t navTopY = 118; // 135 - 16 - 1
        if (navTopY > WATERFALL_Y && (navTopY - WATERFALL_Y) < rowsToPaint)
            rowsToPaint = navTopY - WATERFALL_Y;
    }

    const uint16_t x1 = WATERFALL_X + ST7789_X_OFFSET;
    const uint16_t y1 = WATERFALL_Y + ST7789_Y_OFFSET;
    const uint16_t x2 = x1 + WATERFALL_W - 1;
    const uint16_t y2 = y1 + rowsToPaint - 1;

    uint8_t rowBuf[WATERFALL_W * 2]; // 480 B on stack

    digitalWrite(ST7789_NSS, LOW);
    SPI1.beginTransaction(settings);

    digitalWrite(ST7789_RS, LOW);
    SPI1.write(ST77XX_CASET);
    digitalWrite(ST7789_RS, HIGH);
    uint8_t caset[4] = {(uint8_t)(x1 >> 8), (uint8_t)x1, (uint8_t)(x2 >> 8), (uint8_t)x2};
    SPI1.writeBytes(caset, 4);

    digitalWrite(ST7789_RS, LOW);
    SPI1.write(ST77XX_RASET);
    digitalWrite(ST7789_RS, HIGH);
    uint8_t raset[4] = {(uint8_t)(y1 >> 8), (uint8_t)y1, (uint8_t)(y2 >> 8), (uint8_t)y2};
    SPI1.writeBytes(raset, 4);

    digitalWrite(ST7789_RS, LOW);
    SPI1.write(ST77XX_RAMWR);
    digitalWrite(ST7789_RS, HIGH);

    // Paint rows newest-at-top. Lock-free read of the histogram is intentional: a torn
    // newest row at ~12 Hz is visually imperceptible (one row of one frame).
    for (uint8_t row = 0; row < rowsToPaint; row++) {
        uint8_t bufIdx = (head + WATERFALL_ROWS - 1 - row) % WATERFALL_ROWS;
        const uint8_t *hist = instance->histBuf_[bufIdx];

        // Fill rowBuf bin-span by bin-span — no per-pixel divide, just byte-pair fan-out.
        uint8_t *p = rowBuf;
        for (uint8_t bin = 0; bin < WATERFALL_BINS; bin++) {
            const uint8_t hi = s_paletteBE[hist[bin]][0];
            const uint8_t lo = s_paletteBE[hist[bin]][1];
            const uint8_t spanEnd = s_binStart[bin + 1];
            for (uint8_t px = s_binStart[bin]; px < spanEnd; px++) {
                *p++ = hi;
                *p++ = lo;
            }
        }
        SPI1.writeBytes(rowBuf, WATERFALL_W * 2);
    }

    SPI1.endTransaction();
    digitalWrite(ST7789_NSS, HIGH);
}

} // namespace graphics

#endif // USE_SX1262 && GRAPHICS_TFT_COLORING_ENABLED
