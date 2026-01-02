#include "configuration.h"

#if HAS_TELEMETRY && !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR

#include "../mesh/generated/meshtastic/telemetry.pb.h"
#include "Default.h"
#include "EnvironmentTelemetry.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "PowerFSM.h"
#include "RTC.h"
#include "Router.h"
#include "UnitConversions.h"
#include "buzz.h"
#include "graphics/SharedUIDisplay.h"
#include "graphics/draw/MessageRenderer.h"
#include "graphics/draw/UIRenderer.h"
#include "graphics/emotes.h"
#include "graphics/fonts/OLEDDisplayFontsOhm.h"
#include "graphics/images.h"
#include "main.h"
#include "modules/ExternalNotificationModule.h"
#include "power.h"
#include "sleep.h"
#include "target_specific.h"
#include <OLEDDisplay.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <math.h>
#include <unordered_map>

using graphics::Emote;
using graphics::emotes;
using graphics::numEmotes;

OLEDDisplay *display = nullptr;

// Your chosen layout constants
static constexpr int kRulerPadLR = 4;       // L/R padding (px)
static constexpr int kNeedleH = 5;          // triangle height below baseline
static constexpr int kNeedleBaseHalf = 5;   // half base width
static constexpr int kRulerBaselineOfs = 8; // baseline = y + this
static constexpr int kLabelGap = -1;        // pixels below triangle base

// IAQ "ruler": |-----▲-----| with danger ticks and filled up-triangle below baseline
static void drawIAQRuler(OLEDDisplay *dpy, int x, int y, int w, int iaqValue, const char *label)
{
    // inner rect after padding
    int ix = x + kRulerPadLR;
    int iw = w - (kRulerPadLR * 2);
    if (iw < 12) {
        ix = x;
        iw = w;
    }

    // clamp + layout
    int iaq = iaqValue;
    if (iaq < 0)
        iaq = 0;
    if (iaq > 500)
        iaq = 500;
    const int baselineY = y + kRulerBaselineOfs;
    const int capHalf = 3;

    // end caps ‘|’
    dpy->drawLine(ix, baselineY - capHalf, ix, baselineY + capHalf);
    dpy->drawLine(ix + iw - 1, baselineY - capHalf, ix + iw - 1, baselineY + capHalf);

    // dashed baseline
    for (int i = ix + 1; i < ix + iw - 1; i += 3) {
        dpy->drawLine(i, baselineY, std::min(i + 1, ix + iw - 2), baselineY);
    }

    // ticks at danger levels: 0,25,50,100,150,200,300,500
    auto valToX = [&](int v) -> int { return ix + (int)std::lround((float)v / 500.0f * (iw - 1)); };
    const int tickShort = 2; // minor tick half-height
    const int tickLong = 3;  // major tick half-height

    auto drawTick = [&](int v, bool major) {
        int tx = valToX(v);
        int h = major ? tickLong : tickShort;
        dpy->drawLine(tx, baselineY - h, tx, baselineY + h);
    };

    // majors: 0,100,150,200,300,500 (category boundaries)
    drawTick(0, true);
    drawTick(100, true);
    drawTick(150, true);
    drawTick(200, true);
    drawTick(300, true);
    drawTick(500, true);

    // minors: 25, 50 (within "Excellent/Good/Moderate")
    drawTick(25, false);
    drawTick(50, false);

    // filled upward triangle BELOW the baseline (tip pokes through)
    const int nx = valToX(iaq);
    for (int dy = 0; dy <= kNeedleH; ++dy) {
        int half = (int)std::lround((float)kNeedleBaseHalf * ((float)dy / (float)kNeedleH));
        int yrow = baselineY + dy - 1; // rows below baseline
        dpy->drawLine(nx - half, yrow, nx + half, yrow);
    }

    // centered label snug under the triangle base - use FONT_SMALL explicitly
    dpy->setFont(FONT_SMALL);
    const int lw = dpy->getStringWidth(label);
    const int lx = x + (w - lw) / 2;
    dpy->drawString(lx, baselineY + kNeedleH - 1 + kLabelGap, label);
}

// keep last N samples per source
// Use larger buffer on PSRAM devices for richer historical data
#if defined(ARCH_ESP32) && defined(BOARD_HAS_PSRAM)
static constexpr size_t kHistLen = 60; // Moderate increase for PSRAM devices
#else
static constexpr size_t kHistLen = 30;     // Conservative for limited heap (reduced from 120→60→30)
#endif
static constexpr int kSparkH = 12; // pixels tall for standard displays

// Large display sparkline settings (SenseCAP Indicator 480x480, T-Deck 320x240)
static constexpr int kLargeSparkH = 80;      // Double height graphs for large displays
static constexpr int kLargeSparkMargin = 52; // Left margin for axis labels

// Fixed-capacity ring buffer (oldest→newest iteration)
template <size_t N> struct RingF {
    uint16_t len = 0;  // 0..N
    uint16_t head = 0; // index of oldest element
    float v[N];
    inline void push(float x)
    {
        if (std::isnan(x))
            return;
        if (len < N) {
            v[(head + len) % N] = x;
            ++len;
        } else {
            v[head] = x;
            head = (head + 1) % N;
        }
    }
    inline float at(size_t i) const { return v[(head + i) % N]; } // 0..len-1
};

template <size_t N> struct NodeHist {
    RingF<N> temp, hum, press, gas, voltage, current, lux, whiteLux, weight, distance, radiation;
    RingF<N> windSpeed, windDirection, dewPoint, soilTemp, soilMoisture;
    uint32_t lastUpdate = 0; // Track when this node was last updated (millis)

    // Store latest telemetry values directly instead of packet copies
    meshtastic_EnvironmentMetrics lastMetrics = meshtastic_EnvironmentMetrics_init_zero;
    uint32_t rxTime = 0; // When we received this data (for "ago" display)
};

// One record per node; no per-sample heap churn
static std::unordered_map<uint32_t, NodeHist<kHistLen>> s_hist;

// Cap on maximum nodes and evict stale ones
// Use larger limits on devices with PSRAM to allow more telemetry history
#if defined(ARCH_ESP32) && defined(BOARD_HAS_PSRAM)
static constexpr size_t kMaxHistNodes = 16; // Moderate increase for PSRAM-equipped devices
#else
static constexpr size_t kMaxHistNodes = 5; // Conservative limit for devices with limited heap
#endif
static constexpr uint32_t kNodeStaleTimeout = 3600000; // 1 hour - evict nodes not updated in this time

// Reserve map capacity on first use to avoid dynamic reallocation
static bool s_histReserved = false;

// Track most recent telemetry source for "auto" mode display
static uint32_t s_lastSource = 0;

// Clean up stale nodes from s_hist that haven't been updated recently
static void cleanupStaleNodes()
{
    uint32_t now = millis();
    for (auto it = s_hist.begin(); it != s_hist.end();) {
        if (now - it->second.lastUpdate > kNodeStaleTimeout) {
            LOG_DEBUG("EnvironmentTelemetry: Evicting stale node 0x%08x (last update %lums ago)", it->first,
                      now - it->second.lastUpdate);
            it = s_hist.erase(it);
        } else {
            ++it;
        }
    }
}

// Helper: apply both offsets centrally
// Overload for fixed-capacity ring

/**
 * Helper function to draw a string containing "kΩ" with proper Ohm symbol rendering.
 * Detects "kΩ" in the string and uses XBM bitmap rendering for the Ω symbol.
 * Selects the appropriate Ohm bitmap size based on the font height.
 * Similar approach to drawStringWithEmotes.
 */
static void drawStringWithOhm(OLEDDisplay *display, int16_t x, int16_t y, const char *text, int fontHeight = FONT_HEIGHT_SMALL)
{
    // Look for "k" followed by UTF-8 Omega (0xCE 0xA9)
    const char *ohmPos = nullptr;
    for (const char *p = text; *p; p++) {
        if (*p == 'k' && (unsigned char)p[1] == 0xCE && (unsigned char)p[2] == 0xA9) {
            ohmPos = p;
            break;
        }
    }

    if (!ohmPos) {
        // No Ohm symbol, draw normally
        display->drawString(x, y, text);
        return;
    }

    // Draw text up to and including "k"
    size_t prefixLen = ohmPos - text + 1; // Include the 'k'
    char prefix[128];
    strncpy(prefix, text, prefixLen);
    prefix[prefixLen] = '\0';
    display->drawString(x, y, prefix);
    x += display->getStringWidth(prefix);

    // Select appropriate Ohm bitmap based on font height
    const uint8_t *ohmBitmap;
    int ohmWidth, ohmHeight;

    if (fontHeight <= 7) {
        // Tiny font (TomThumb, height ~6)
        ohmBitmap = OhmBitmap_Tiny;
        ohmWidth = OhmWidth_Tiny;
        ohmHeight = OhmHeight_Tiny;
    } else if (fontHeight <= 14) {
        // Small font (ArialMT_Plain_10, height ~13)
        ohmBitmap = OhmBitmap_10;
        ohmWidth = OhmWidth_10;
        ohmHeight = OhmHeight_10;
    } else if (fontHeight <= 20) {
        // Medium font (ArialMT_Plain_16, height ~19)
        ohmBitmap = OhmBitmap_16;
        ohmWidth = OhmWidth_16;
        ohmHeight = OhmHeight_16;
    } else {
        // Large font (ArialMT_Plain_24, height ~28)
        ohmBitmap = OhmBitmap_24;
        ohmWidth = OhmWidth_24;
        ohmHeight = OhmHeight_24;
    }

    // Draw Ohm symbol bitmap, vertically centered with text
    x += 1; // 1px gap after 'k'
    int ohmY = y + (fontHeight - ohmHeight) / 2;
    display->drawXbm(x, ohmY, ohmWidth, ohmHeight, ohmBitmap);
    x += ohmWidth;

    // Draw remaining text after "kΩ" (skip "k" + UTF-8 Ω bytes: 0xCE 0xA9)
    const char *remaining = ohmPos + 3; // "k" (1) + "Ω" (0xCE 0xA9 = 2 bytes)
    if (*remaining != '\0') {
        display->drawString(x, y, remaining);
    }
}

// Sparkline placement offsets (tweak to taste)
static constexpr int kSparkXOffset = -2; // shift left
static constexpr int kSparkYOffset = 1;  // shift down

// Large display sparkline with min/max labels and better visibility
// Unit conversion modes for graph min/max display:
// - convertTemp: convert Celsius to Fahrenheit
// - convertPress: convert hPa to inHg
// Data in ring buffers is always stored in metric units (C, hPa, kg, mm, m/s, etc)
// This function converts for display only based on the unit string
template <size_t N>
static void drawLargeSparkBoxed(OLEDDisplay *dpy, int x, int y, int w, int h, const RingF<N> &hist, const String &unit = "",
                                bool convertTemp = false, bool convertPress = false)
{
    if (hist.len < 2)
        return;

    // Tuning knobs (same as mini version)
    constexpr float kHeadroomFrac = 0.10f;
    constexpr float kWinsorLowerFrac = 0.02f;
    constexpr float kWinsorUpperFrac = 0.02f;
    constexpr float kSmoothAlpha = 0.0f;

    // Geometry - reserve space for labels on left
    const int labelMargin = 52; // space for "99.9" type labels with extra padding
    const int ix = x + labelMargin;
    const int iy = y + 1;
    const int iw = w - labelMargin - 4; // Extra margin to prevent line spillover
    const int ih = h - 2;
    const uint16_t L = hist.len;
    const float step = (L > 1) ? (float(iw) / float(L - 1)) : 0.0f;

    // First pass: raw extrema
    float lo = hist.at(0), hi = hist.at(0);
    for (uint16_t i = 1; i < L; ++i) {
        const float v = hist.at(i);
        if (v < lo)
            lo = v;
        if (v > hi)
            hi = v;
    }
    float span0 = hi - lo;
    if (span0 < 1e-6f) {
        lo -= 0.5f;
        hi += 0.5f;
        span0 = hi - lo;
    }

    // Winsorization
    const float loClamp = lo + span0 * kWinsorLowerFrac;
    const float hiClamp = hi - span0 * kWinsorUpperFrac;
    auto clampTo = [&](float v) {
        if (kWinsorLowerFrac > 0.f && v < loClamp)
            v = loClamp;
        if (kWinsorUpperFrac > 0.f && v > hiClamp)
            v = hiClamp;
        return v;
    };

    // Second pass: extrema after clamp
    float sPrev = clampTo(hist.at(0));
    float lo2 = sPrev, hi2 = sPrev;
    for (uint16_t i = 1; i < L; ++i) {
        float v = clampTo(hist.at(i));
        float s = (kSmoothAlpha > 0.f && kSmoothAlpha < 1.f) ? (kSmoothAlpha * v + (1.0f - kSmoothAlpha) * sPrev) : v;
        if (s < lo2)
            lo2 = s;
        if (s > hi2)
            hi2 = s;
        sPrev = s;
    }

    // Headroom + final span
    float loS = lo2, hiS = hi2;
    float span = hiS - loS;
    if (span < 1e-6f) {
        loS -= 0.5f;
        hiS += 0.5f;
        span = hiS - loS;
    }
    const float pad = span * kHeadroomFrac;
    loS -= pad;
    hiS += pad;
    span = hiS - loS;

    auto valToY = [&](float v) -> int {
        const float t = (v - loS) / span;
        int yy = iy + ih - int(std::lround(t * ih));
        if (yy < iy)
            yy = iy;
        if (yy > iy + ih)
            yy = iy + ih;
        return yy;
    };

    // Draw box
    dpy->drawRect(ix, iy, iw, ih);

    // Draw min/max labels with unit (small font for notation)
    dpy->setFont(ArialMT_Plain_10);
    dpy->setTextAlignment(TEXT_ALIGN_LEFT); // drawStringWithOhm assumes LEFT alignment
    float maxVal = hi2;
    float minVal = lo2;
    // Convert units if needed (data is stored in metric: Celsius, hPa, kg, mm, m/s)
    if (convertTemp) {
        maxVal = maxVal * 9.0f / 5.0f + 32.0f; // C to F
        minVal = minVal * 9.0f / 5.0f + 32.0f;
    } else if (convertPress) {
        maxVal = maxVal * 0.02953f; // hPa to inHg
        minVal = minVal * 0.02953f;
    } else if (unit == "lbs") {
        // Weight: kg to lbs
        maxVal = maxVal * 2.20462f;
        minVal = minVal * 2.20462f;
    } else if (unit == "in") {
        // Distance: mm to inches
        maxVal = maxVal * 0.03937f;
        minVal = minVal * 0.03937f;
    } else if (unit == "mph") {
        // Wind speed: m/s to mph
        maxVal = maxVal * 2.23694f;
        minVal = minVal * 2.23694f;
    }

    // Format labels and use drawStringWithOhm for Ohm symbol support
    char maxBuf[32], minBuf[32];
    snprintf(maxBuf, sizeof(maxBuf), "%.1f%s", maxVal, unit.c_str());
    snprintf(minBuf, sizeof(minBuf), "%.1f%s", minVal, unit.c_str());

    // For right-aligned text, calculate width manually since getStringWidth
    // won't handle UTF-8 Ω correctly
    int maxWidth, minWidth;

    if (unit == "kΩ") {
        // Calculate width: "123.4k" + 1px gap + Ohm bitmap width
        char numWithK[32];
        snprintf(numWithK, sizeof(numWithK), "%.1fk", maxVal);
        maxWidth = dpy->getStringWidth(numWithK) + 1 + OhmWidth_10;
        snprintf(numWithK, sizeof(numWithK), "%.1fk", minVal);
        minWidth = dpy->getStringWidth(numWithK) + 1 + OhmWidth_10;
    } else {
        maxWidth = dpy->getStringWidth(maxBuf);
        minWidth = dpy->getStringWidth(minBuf);
    }

    // Draw right-aligned to spark box edge (ix - 3)
    // Pass font height 13 (ArialMT_Plain_10) to get the correct small Ohm bitmap
    drawStringWithOhm(dpy, ix - 3 - maxWidth, iy, maxBuf, 13);
    drawStringWithOhm(dpy, ix - 3 - minWidth, iy + ih - 10, minBuf, 13);

    dpy->setFont(FONT_SMALL); // restore to label font

    // Draw polyline (thicker for better visibility on large screens)
    sPrev = clampTo(hist.at(0));
    int xPrev = ix + 1;
    int yPrev = valToY(sPrev);

    for (uint16_t i = 1; i < L; ++i) {
        int xCur = ix + 1 + int(std::lround(step * i));
        // Clamp to stay within box
        if (xCur >= ix + iw - 1) {
            xCur = ix + iw - 2;
        }
        float v = clampTo(hist.at(i));
        float s = (kSmoothAlpha > 0.f && kSmoothAlpha < 1.f) ? (kSmoothAlpha * v + (1.0f - kSmoothAlpha) * sPrev) : v;
        const int yCur = valToY(s);

        // Draw thicker line for large displays
        dpy->drawLine(xPrev, yPrev, xCur, yCur);
        if (h >= kLargeSparkH / 2) { // Only thicken if graph is reasonably tall
            dpy->drawLine(xPrev, yPrev + 1, xCur, yCur + 1);
        }
        xPrev = xCur;
        yPrev = yCur;
        sPrev = s;
    }
}

template <size_t N> static void drawMiniSparkBoxed(OLEDDisplay *dpy, int x, int y, int w, int h, const RingF<N> &hist)
{
    const int x0 = x + kSparkXOffset;
    const int y0 = y + kSparkYOffset;

    dpy->drawRect(x0, y0, w, h);
    if (hist.len < 2)
        return;

    // Tuning knobs
    constexpr float kHeadroomFrac = 0.10f;    // 10% visual headroom
    constexpr float kWinsorLowerFrac = 0.02f; // clamp bottom 2% (set 0 to disable)
    constexpr float kWinsorUpperFrac = 0.02f; // clamp top   2% (set 0 to disable)
    constexpr float kSmoothAlpha = 0.0f;      // 0=no smoothing; try 0.2–0.4 if noisy

    // Geometry
    const int ix = x0 + 1, iy = y0 + 1;
    const int iw = w - 2, ih = h - 2;
    const uint16_t L = hist.len;
    const float step = (L > 1) ? (float(iw) / float(L - 1)) : 0.0f;

    // First pass: raw extrema
    float lo = hist.at(0), hi = hist.at(0);
    for (uint16_t i = 1; i < L; ++i) {
        const float v = hist.at(i);
        if (v < lo)
            lo = v;
        if (v > hi)
            hi = v;
    }
    float span0 = hi - lo;
    if (span0 < 1e-6f) {
        lo -= 0.5f;
        hi += 0.5f;
        span0 = hi - lo;
    }

    // Winsorization bounds
    const float loClamp = lo + span0 * kWinsorLowerFrac;
    const float hiClamp = hi - span0 * kWinsorUpperFrac;

    auto clampTo = [&](float v) {
        if (kWinsorLowerFrac > 0.f && v < loClamp)
            v = loClamp;
        if (kWinsorUpperFrac > 0.f && v > hiClamp)
            v = hiClamp;
        return v;
    };

    // Second pass: extrema after clamp (+ optional smoothing)
    float sPrev = clampTo(hist.at(0));
    float lo2 = sPrev, hi2 = sPrev;
    for (uint16_t i = 1; i < L; ++i) {
        float v = clampTo(hist.at(i));
        float s = (kSmoothAlpha > 0.f && kSmoothAlpha < 1.f) ? (kSmoothAlpha * v + (1.0f - kSmoothAlpha) * sPrev) : v;
        if (s < lo2)
            lo2 = s;
        if (s > hi2)
            hi2 = s;
        sPrev = s;
    }

    // Headroom + final span
    float loS = lo2, hiS = hi2;
    float span = hiS - loS;
    if (span < 1e-6f) {
        loS -= 0.5f;
        hiS += 0.5f;
        span = hiS - loS;
    }
    const float pad = span * kHeadroomFrac;
    loS -= pad;
    hiS += pad;
    span = hiS - loS;

    auto valToY = [&](float v) -> int {
        const float t = (v - loS) / span;
        int yy = iy + ih - int(std::lround(t * ih));
        if (yy < iy)
            yy = iy;
        if (yy > iy + ih)
            yy = iy + ih;
        return yy;
    };

    // Draw polyline (start from real first sample)
    sPrev = clampTo(hist.at(0));
    int xPrev = ix;
    int yPrev = valToY(sPrev);

    for (uint16_t i = 1; i < L; ++i) {
        const int xCur = ix + int(std::lround(step * i));
        float v = clampTo(hist.at(i));
        float s = (kSmoothAlpha > 0.f && kSmoothAlpha < 1.f) ? (kSmoothAlpha * v + (1.0f - kSmoothAlpha) * sPrev) : v;
        const int yCur = valToY(s);
        dpy->drawLine(xPrev, yPrev, xCur, yCur);
        xPrev = xCur;
        yPrev = yCur;
        sPrev = s;
    }
}

// Prefer long_name when available (and width allows), else short_name, else hex id.
// Mirrors the selection logic used by MessageRenderer.
static inline const char *getSenderName(uint32_t nodeNum)
{
    static char buf[64];

    const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(nodeNum);
    if (node && node->has_user) {
        const char *ln = node->user.long_name;
        const char *sn = node->user.short_name;

#if defined(M5STACK_UNITC6L)
        // On this target, MessageRenderer prefers short_name.
        if (sn && sn[0])
            return sn;
        if (ln && ln[0])
            return ln;
#else
        // On wider screens, prefer long_name; otherwise short_name.
        if (SCREEN_WIDTH >= 200 && ln && ln[0])
            return ln;
        if (sn && sn[0])
            return sn;
        if (ln && ln[0])
            return ln; // last resort if short_name empty
#endif
    }

    // Fallback: hex node id like "ABCDEF12"
    snprintf(buf, sizeof(buf), "%08x", (unsigned int)nodeNum);
    return buf;
}

// Magnus-Tetens over water (good for typical indoor temps)
static float dewPointC(float tempC, float rhPercent)
{
    if (!(rhPercent > 0.0f && rhPercent <= 100.0f))
        return NAN;
    const float a = 17.62f;
    const float b = 243.12f; // °C
    const float gamma = (a * tempC) / (b + tempC) + logf(rhPercent / 100.0f);
    return (b * gamma) / (a - gamma);
}

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR_EXTERNAL

// Sensors
#include "Sensor/CGRadSensSensor.h"
#include "Sensor/RCWL9620Sensor.h"
#include "Sensor/nullSensor.h"

EnvironmentTelemetryModule *environmentTelemetryModule = nullptr;

namespace graphics
{
extern void drawCommonHeader(OLEDDisplay *display, int16_t x, int16_t y, const char *titleStr, bool force_no_invert,
                             bool show_date);
}
#if __has_include(<Adafruit_AHTX0.h>)
#include "Sensor/AHT10.h"
#endif

#if __has_include(<Adafruit_BME280.h>)
#include "Sensor/BME280Sensor.h"
#endif

#if __has_include(<Adafruit_BMP085.h>)
#include "Sensor/BMP085Sensor.h"
#endif

#if __has_include(<Adafruit_BMP280.h>)
#include "Sensor/BMP280Sensor.h"
#endif

#if __has_include(<Adafruit_LTR390.h>)
#include "Sensor/LTR390UVSensor.h"
#endif

#if __has_include(<bsec2.h>)
#include "Sensor/BME680Sensor.h"
#endif

#if __has_include(<Adafruit_DPS310.h>)
#include "Sensor/DPS310Sensor.h"
#endif

#if __has_include(<Adafruit_MCP9808.h>)
#include "Sensor/MCP9808Sensor.h"
#endif

#if __has_include(<Adafruit_SHT31.h>)
#include "Sensor/SHT31Sensor.h"
#endif

#if __has_include(<Adafruit_LPS2X.h>)
#include "Sensor/LPS22HBSensor.h"
#endif

#if __has_include(<Adafruit_SHTC3.h>)
#include "Sensor/SHTC3Sensor.h"
#endif

#if __has_include("RAK12035_SoilMoisture.h") && defined(RAK_4631) && RAK_4631 == 1
#include "Sensor/RAK12035Sensor.h"
#endif

#if __has_include(<Adafruit_VEML7700.h>)
#include "Sensor/VEML7700Sensor.h"
#endif

#if __has_include(<Adafruit_TSL2591.h>)
#include "Sensor/TSL2591Sensor.h"
#endif

#if __has_include(<ClosedCube_OPT3001.h>)
#include "Sensor/OPT3001Sensor.h"
#endif

#if __has_include(<Adafruit_SHT4x.h>)
#include "Sensor/SHT4XSensor.h"
#endif

#if __has_include(<SparkFun_MLX90632_Arduino_Library.h>)
#include "Sensor/MLX90632Sensor.h"
#endif

#if __has_include(<DFRobot_LarkWeatherStation.h>)
#include "Sensor/DFRobotLarkSensor.h"
#endif

#if __has_include(<DFRobot_RainfallSensor.h>)
#include "Sensor/DFRobotGravitySensor.h"
#endif

#if __has_include(<SparkFun_Qwiic_Scale_NAU7802_Arduino_Library.h>)
#include "Sensor/NAU7802Sensor.h"
#endif

#if __has_include(<Adafruit_BMP3XX.h>)
#include "Sensor/BMP3XXSensor.h"
#endif

#if __has_include(<Adafruit_PCT2075.h>)
#include "Sensor/PCT2075Sensor.h"
#endif

#endif
#ifdef T1000X_SENSOR_EN
#include "Sensor/T1000xSensor.h"
#endif

#ifdef SENSECAP_INDICATOR
#include "Sensor/IndicatorSensor.h"
#endif

#if __has_include(<Adafruit_TSL2561_U.h>)
#include "Sensor/TSL2561Sensor.h"
#endif

#if __has_include(<BH1750_WE.h>)
#include "Sensor/BH1750Sensor.h"
#endif

#define FAILED_STATE_SENSOR_READ_MULTIPLIER 10
#define DISPLAY_RECEIVEID_MEASUREMENTS_ON_SCREEN true

#include "graphics/ScreenFonts.h"
#include <Throttle.h>

#include <forward_list>

static std::forward_list<TelemetrySensor *> sensors;

template <typename T> void addSensor(ScanI2C *i2cScanner, ScanI2C::DeviceType type)
{
    ScanI2C::FoundDevice dev = i2cScanner->find(type);
    if (dev.type != ScanI2C::DeviceType::NONE || type == ScanI2C::DeviceType::NONE) {
        TelemetrySensor *sensor = new T();
#if WIRE_INTERFACES_COUNT > 1
        TwoWire *bus = ScanI2CTwoWire::fetchI2CBus(dev.address);
        if (dev.address.port != ScanI2C::I2CPort::WIRE1 && sensor->onlyWire1()) {
            // This sensor only works on Wire (Wire1 is not supported)
            delete sensor;
            return;
        }
#else
        TwoWire *bus = &Wire;
#endif
        if (sensor->initDevice(bus, &dev)) {
            sensors.push_front(sensor);
            return;
        }
        // destroy sensor
        delete sensor;
    }
}

void EnvironmentTelemetryModule::i2cScanFinished(ScanI2C *i2cScanner)
{
    if (!moduleConfig.telemetry.environment_measurement_enabled && !ENVIRONMENTAL_TELEMETRY_MODULE_ENABLE) {
        return;
    }
    LOG_INFO("Environment Telemetry adding I2C devices...");

    // order by priority of metrics/values (low top, high bottom)

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR
#ifdef T1000X_SENSOR_EN
    // Not a real I2C device
    addSensor<T1000xSensor>(i2cScanner, ScanI2C::DeviceType::NONE);
#else
#ifdef SENSECAP_INDICATOR
    // Not a real I2C device, uses UART
    addSensor<IndicatorSensor>(i2cScanner, ScanI2C::DeviceType::NONE);
#endif
    addSensor<RCWL9620Sensor>(i2cScanner, ScanI2C::DeviceType::RCWL9620);
    addSensor<CGRadSensSensor>(i2cScanner, ScanI2C::DeviceType::CGRADSENS);
#endif
#endif

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR && !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR_EXTERNAL
#if __has_include(<DFRobot_LarkWeatherStation.h>)
    addSensor<DFRobotLarkSensor>(i2cScanner, ScanI2C::DeviceType::DFROBOT_LARK);
#endif
#if __has_include(<DFRobot_RainfallSensor.h>)
    addSensor<DFRobotGravitySensor>(i2cScanner, ScanI2C::DeviceType::DFROBOT_RAIN);
#endif
#if __has_include(<Adafruit_AHTX0.h>)
    addSensor<AHT10Sensor>(i2cScanner, ScanI2C::DeviceType::AHT10);
#endif
#if __has_include(<Adafruit_BMP085.h>)
    addSensor<BMP085Sensor>(i2cScanner, ScanI2C::DeviceType::BMP_085);
#endif
#if __has_include(<Adafruit_BME280.h>)
    addSensor<BME280Sensor>(i2cScanner, ScanI2C::DeviceType::BME_280);
#endif
#if __has_include(<Adafruit_LTR390.h>)
    addSensor<LTR390UVSensor>(i2cScanner, ScanI2C::DeviceType::LTR390UV);
#endif
#if __has_include(<bsec2.h>)
    addSensor<BME680Sensor>(i2cScanner, ScanI2C::DeviceType::BME_680);
#endif
#if __has_include(<Adafruit_BMP280.h>)
    addSensor<BMP280Sensor>(i2cScanner, ScanI2C::DeviceType::BMP_280);
#endif
#if __has_include(<Adafruit_DPS310.h>)
    addSensor<DPS310Sensor>(i2cScanner, ScanI2C::DeviceType::DPS310);
#endif
#if __has_include(<Adafruit_MCP9808.h>)
    addSensor<MCP9808Sensor>(i2cScanner, ScanI2C::DeviceType::MCP9808);
#endif
#if __has_include(<Adafruit_SHT31.h>)
    addSensor<SHT31Sensor>(i2cScanner, ScanI2C::DeviceType::SHT31);
#endif
#if __has_include(<Adafruit_LPS2X.h>)
    addSensor<LPS22HBSensor>(i2cScanner, ScanI2C::DeviceType::LPS22HB);
#endif
#if __has_include(<Adafruit_SHTC3.h>)
    addSensor<SHTC3Sensor>(i2cScanner, ScanI2C::DeviceType::SHTC3);
#endif
#if __has_include("RAK12035_SoilMoisture.h") && defined(RAK_4631) && RAK_4631 == 1
    addSensor<RAK12035Sensor>(i2cScanner, ScanI2C::DeviceType::RAK12035);
#endif
#if __has_include(<Adafruit_VEML7700.h>)
    addSensor<VEML7700Sensor>(i2cScanner, ScanI2C::DeviceType::VEML7700);
#endif
#if __has_include(<Adafruit_TSL2591.h>)
    addSensor<TSL2591Sensor>(i2cScanner, ScanI2C::DeviceType::TSL2591);
#endif
#if __has_include(<ClosedCube_OPT3001.h>)
    addSensor<OPT3001Sensor>(i2cScanner, ScanI2C::DeviceType::OPT3001);
#endif
#if __has_include(<Adafruit_SHT4x.h>)
    addSensor<SHT4XSensor>(i2cScanner, ScanI2C::DeviceType::SHT4X);
#endif
#if __has_include(<SparkFun_MLX90632_Arduino_Library.h>)
    addSensor<MLX90632Sensor>(i2cScanner, ScanI2C::DeviceType::MLX90632);
#endif

#if __has_include(<Adafruit_BMP3XX.h>)
    addSensor<BMP3XXSensor>(i2cScanner, ScanI2C::DeviceType::BMP_3XX);
#endif
#if __has_include(<Adafruit_PCT2075.h>)
    addSensor<PCT2075Sensor>(i2cScanner, ScanI2C::DeviceType::PCT2075);
#endif
#if __has_include(<Adafruit_TSL2561_U.h>)
    addSensor<TSL2561Sensor>(i2cScanner, ScanI2C::DeviceType::TSL2561);
#endif
#if __has_include(<SparkFun_Qwiic_Scale_NAU7802_Arduino_Library.h>)
    addSensor<NAU7802Sensor>(i2cScanner, ScanI2C::DeviceType::NAU7802);
#endif
#if __has_include(<BH1750_WE.h>)
    addSensor<BH1750Sensor>(i2cScanner, ScanI2C::DeviceType::BH1750);
#endif

#endif
}

int32_t EnvironmentTelemetryModule::runOnce()
{
    if (sleepOnNextExecution == true) {
        sleepOnNextExecution = false;
        uint32_t nightyNightMs = Default::getConfiguredOrDefaultMs(moduleConfig.telemetry.environment_update_interval,
                                                                   default_telemetry_broadcast_interval_secs);
        LOG_DEBUG("Sleep for %ims, then awake to send metrics again", nightyNightMs);
        doDeepSleep(nightyNightMs, true, false);
    }

    uint32_t result = UINT32_MAX;
    /*
        Uncomment the preferences below if you want to use the module
        without having to configure it from the PythonAPI or WebUI.
    */

    // moduleConfig.telemetry.environment_measurement_enabled = 1;
    // moduleConfig.telemetry.environment_screen_enabled = 1;
    // moduleConfig.telemetry.environment_update_interval = 15;

    if (!(moduleConfig.telemetry.environment_measurement_enabled || moduleConfig.telemetry.environment_screen_enabled ||
          ENVIRONMENTAL_TELEMETRY_MODULE_ENABLE)) {
        // If this module is not enabled, and the user doesn't want the display screen don't waste any OSThread time on it
        return disable();
    }

    if (firstTime) {
        // This is the first time the OSThread library has called this function, so do some setup
        firstTime = 0;

        if (moduleConfig.telemetry.environment_measurement_enabled || ENVIRONMENTAL_TELEMETRY_MODULE_ENABLE) {
            LOG_INFO("Environment Telemetry: init");

            // check if we have at least one sensor
            if (!sensors.empty()) {
                result = DEFAULT_SENSOR_MINIMUM_WAIT_TIME_BETWEEN_READS;
            }

#ifdef T1000X_SENSOR_EN
#elif !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR_EXTERNAL
            if (ina219Sensor.hasSensor())
                result = ina219Sensor.runOnce();
            if (ina260Sensor.hasSensor())
                result = ina260Sensor.runOnce();
            if (ina3221Sensor.hasSensor())
                result = ina3221Sensor.runOnce();
            if (max17048Sensor.hasSensor())
                result = max17048Sensor.runOnce();
                // this only works on the wismesh hub with the solar option. This is not an I2C sensor, so we don't need the
                // sensormap here.
#ifdef HAS_RAKPROT
            result = rak9154Sensor.runOnce();
#endif
#endif
        }

        // Set when mesh broadcasts should start (after stagger delay)
        int32_t broadcastDelay = setStartDelay();
        meshBroadcastStartTime = millis() + broadcastDelay;
        LOG_DEBUG("Mesh broadcast will start in %dms", broadcastDelay);

        // Initialize screen data immediately if screen is enabled
        if (moduleConfig.telemetry.environment_screen_enabled) {
            // Set lastScreenUpdate to trigger shortly after first sensor reading, but avoid
            // duplicating the initial mesh broadcast which will also update the screen data
            lastScreenUpdate = millis();
            LOG_INFO("Screen enabled - will update after first broadcast in %dms", broadcastDelay);
        }

        // Return 2 seconds to allow timely checking of broadcast window
        // (broadcastDelay could be 30+ seconds, which would delay the actual broadcast)
        return result == UINT32_MAX ? disable() : 2000;
    } else {
        // if we somehow got to a second run of this module with measurement disabled, then just wait forever
        if (!moduleConfig.telemetry.environment_measurement_enabled && !ENVIRONMENTAL_TELEMETRY_MODULE_ENABLE) {
            return disable();
        }

        for (TelemetrySensor *sensor : sensors) {
            uint32_t delay = sensor->runOnce();
            if (delay < result) {
                result = delay;
            }
        }

        // Check if it's time to broadcast to mesh (respecting initial stagger delay)
        bool timeForMeshBroadcast =
            (lastSentToMesh == 0 && millis() >= meshBroadcastStartTime) ||
            (lastSentToMesh > 0 &&
             !Throttle::isWithinTimespanMs(lastSentToMesh, Default::getConfiguredOrDefaultMsScaled(
                                                               moduleConfig.telemetry.environment_update_interval,
                                                               default_telemetry_broadcast_interval_secs, numOnlineNodes)));

        if (timeForMeshBroadcast &&
            airTime->isTxAllowedChannelUtil(config.device.role != meshtastic_Config_DeviceConfig_Role_SENSOR) &&
            airTime->isTxAllowedAirUtil()) {
            sendTelemetry();
            lastSentToMesh = millis();
        } else if (((lastSentToPhone == 0) || !Throttle::isWithinTimespanMs(lastSentToPhone, sendToPhoneIntervalMs)) &&
                   (service->isToPhoneQueueEmpty())) {
            // Just send to phone when it's not our time to send to mesh yet
            // Only send while queue is empty (phone assumed connected)
            sendTelemetry(NODENUM_BROADCAST, true);
            lastSentToPhone = millis();
        }
    }
    return min(min(sendToPhoneIntervalMs, screenUpdateIntervalMs), result);
}

bool EnvironmentTelemetryModule::wantUIFrame()
{
    return moduleConfig.telemetry.environment_screen_enabled;
}

#if HAS_SCREEN

// Cache for formatted display strings to avoid recreating on every frame
// Use fixed buffers instead of String to reduce heap fragmentation
static struct TelemetryDisplayCache {
    static constexpr size_t MAX_ENTRIES = 15;
    static constexpr size_t BUF_SIZE = 48;

    char entries[MAX_ENTRIES][BUF_SIZE];
    size_t entryCount = 0;
    char leftStr[BUF_SIZE];
    char tempStr[BUF_SIZE];
    char humStr[BUF_SIZE];
    char pressStr[BUF_SIZE];
    char iaqStr[BUF_SIZE];
    uint32_t lastSender = 0;
    const meshtastic_MeshPacket *lastPacket = nullptr;
    bool dirty = true;

    void markDirty() { dirty = true; }
    void clear()
    {
        entryCount = 0;
        leftStr[0] = '\0';
        tempStr[0] = '\0';
        humStr[0] = '\0';
        pressStr[0] = '\0';
        iaqStr[0] = '\0';
        lastSender = 0;
        lastPacket = nullptr;
        dirty = true;
    }

    void addEntry(const char *str)
    {
        if (entryCount < MAX_ENTRIES) {
            strncpy(entries[entryCount], str, BUF_SIZE - 1);
            entries[entryCount][BUF_SIZE - 1] = '\0';
            entryCount++;
        }
    }
} s_displayCache;

// Scroll state tracking (float for smooth pixel-level scrolling)
static float s_scrollY = 0.0f;
static bool s_manualScrolling = false;

std::vector<uint32_t> EnvironmentTelemetryModule::getSourcesWithTelemetry() const
{
    std::vector<uint32_t> out;
    out.reserve(s_hist.size());
    for (const auto &kv : s_hist)
        out.push_back(kv.first);
    std::sort(out.begin(), out.end()); // nice to have
    return out;
}

void EnvironmentTelemetryModule::clearEnvCache()
{
    s_hist.clear();
    s_lastSource = 0;
    s_displayCache.markDirty();
}

void EnvironmentTelemetryModule::invalidateDisplayCache()
{
    s_displayCache.markDirty();
}

void EnvironmentTelemetryModule::scrollUp()
{
    s_manualScrolling = true;
    s_scrollY -= 12.0f; // Scroll by one row (similar to MessageRenderer)
    if (s_scrollY < 0.0f)
        s_scrollY = 0.0f;
}

void EnvironmentTelemetryModule::scrollDown()
{
    s_manualScrolling = true;
    s_scrollY += 12.0f; // Scroll by one row
}

void EnvironmentTelemetryModule::handleScrollDrag(int deltaY)
{
    s_manualScrolling = true;
    // Direct 1:1 pixel scrolling - negative deltaY means drag down (scroll content up)
    s_scrollY -= (float)deltaY;
    // Note: clamping happens in drawFrame to respect actual content height
    if (s_scrollY < 0.0f)
        s_scrollY = 0.0f;
}

void EnvironmentTelemetryModule::resetScroll()
{
    s_scrollY = 0.0f;
    s_manualScrolling = false;
}

void EnvironmentTelemetryModule::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    ::display = display;
    // === Setup display ===
    display->clear();
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    int line = 1;

    // === Set Title
    const char *titleStr = (graphics::currentResolution == graphics::ScreenResolution::High) ? "Environment" : "Env.";

    // === Header ===
    graphics::drawCommonHeader(display, x, y, titleStr);

    // === Row spacing setup ===
#if defined(M5STACK_UNITC6L) || defined(USE_TINY_FONT)
    const int rowHeight = 7;
#else
    const int rowHeight = FONT_HEIGHT_SMALL - 4;
#endif
    int currentY = graphics::getTextPositions(display)[line++];

    // === Determine which node's telemetry to show ===
    uint32_t sourceNode = 0;
    const NodeHist<kHistLen> *histToShow = nullptr;

    if (selectedSource != 0) {
        // Show specific source
        auto it = s_hist.find(selectedSource);
        if (it != s_hist.end()) {
            sourceNode = selectedSource;
            histToShow = &it->second;
        }
    } else {
        // Auto mode: show most recent source
        if (s_lastSource != 0) {
            auto it = s_hist.find(s_lastSource);
            if (it != s_hist.end()) {
                sourceNode = s_lastSource;
                histToShow = &it->second;
            }
        }
    }

    // === Handle no telemetry data case ===
    if (!histToShow) {
        bool hasSensors = !sensors.empty() || ina219Sensor.hasSensor() || ina260Sensor.hasSensor() || ina3221Sensor.hasSensor() ||
                          max17048Sensor.hasSensor();
        bool hasRemoteData = !s_hist.empty();

        if (!hasSensors && !hasRemoteData) {
            display->drawString(x, currentY, "No sensors detected");
        } else {
            display->drawString(x, currentY, "Waiting for telemetry...");
        }
        return;
    }

    // Get the stored metrics directly (no decode needed!)
    const auto &m = histToShow->lastMetrics;
    const auto &nh = *histToShow;

    // Check if any telemetry field has valid data
    bool hasAny = m.has_temperature || m.has_relative_humidity || m.barometric_pressure != 0 || m.iaq != 0 ||
                  m.has_gas_resistance || m.gas_resistance != 0 || m.voltage != 0 || m.current != 0 || m.lux != 0 ||
                  m.white_lux != 0 || m.weight != 0 || m.distance != 0 || m.radiation != 0;

    if (!hasAny) {
        display->drawString(x, currentY, "Empty Data");
        return;
    }

    // Check if we need to rebuild the cached strings
    bool senderChanged = (s_displayCache.lastSender != sourceNode);

    if (s_displayCache.dirty || senderChanged) {
        s_displayCache.lastSender = sourceNode;
        s_displayCache.dirty = false;

        // Temperature
        if (m.has_temperature) {
            if (config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL) {
                snprintf(s_displayCache.tempStr, sizeof(s_displayCache.tempStr), "Tmp: %.1f°F",
                         UnitConversions::CelsiusToFahrenheit(m.temperature));
            } else {
                snprintf(s_displayCache.tempStr, sizeof(s_displayCache.tempStr), "Tmp: %.1f°C", m.temperature);
            }
        } else {
            s_displayCache.tempStr[0] = '\0';
        }

        // Humidity
        if (m.has_relative_humidity) {
            snprintf(s_displayCache.humStr, sizeof(s_displayCache.humStr), "Hum: %.0f%%", m.relative_humidity);
        } else {
            s_displayCache.humStr[0] = '\0';
        }

        // Pressure
        if (m.barometric_pressure != 0) {
            if (config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL) {
                snprintf(s_displayCache.pressStr, sizeof(s_displayCache.pressStr), "Prss: %.2f inHg",
                         UnitConversions::HectoPascalToInchesOfMercury(m.barometric_pressure));
            } else {
                snprintf(s_displayCache.pressStr, sizeof(s_displayCache.pressStr), "Prss: %.0f hPa", m.barometric_pressure);
            }
        } else {
            s_displayCache.pressStr[0] = '\0';
        }

        // Build entries array for other metrics
        s_displayCache.entryCount = 0;

#if !defined(M5STACK_UNITC6L)
        // Dew point
        if (m.has_temperature && m.has_relative_humidity && m.relative_humidity > 0.0f) {
            const float dpC = dewPointC(m.temperature, m.relative_humidity);
            if (!isnan(dpC)) {
                char buf[48];
                if (config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL) {
                    const float dpF = dpC * 9.0f / 5.0f + 32.0f;
                    snprintf(buf, sizeof(buf), "Dew: %.1f°F", dpF);
                } else {
                    snprintf(buf, sizeof(buf), "Dew: %.1f°C", dpC);
                }
                s_displayCache.addEntry(buf);
            }
        }

        // Gas
        constexpr float kMinGasKOhm = 0.5f;
        constexpr float kMaxGasKOhm = 1000.0f;
        if ((m.has_gas_resistance || m.gas_resistance != 0) &&
            (m.gas_resistance >= kMinGasKOhm && m.gas_resistance <= kMaxGasKOhm)) {
            char buf[48];
            snprintf(buf, sizeof(buf), "Gas: %.2f kΩ", m.gas_resistance);
            s_displayCache.addEntry(buf);
        }
#endif

        // Other metrics
        if (m.voltage != 0 || m.current != 0) {
            char buf[48];
            snprintf(buf, sizeof(buf), "%.1fV / %.0fmA", m.voltage, m.current);
            s_displayCache.addEntry(buf);
        }
        if (m.lux != 0) {
            char buf[48];
            snprintf(buf, sizeof(buf), "Light: %.0flx", m.lux);
            s_displayCache.addEntry(buf);
        }
        if (m.white_lux != 0) {
            char buf[48];
            snprintf(buf, sizeof(buf), "White: %.0flx", m.white_lux);
            s_displayCache.addEntry(buf);
        }
        if (m.weight != 0) {
            char buf[48];
            if (config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL) {
                snprintf(buf, sizeof(buf), "Weight: %.1f lbs", m.weight * 2.20462f);
            } else {
                snprintf(buf, sizeof(buf), "Weight: %.0f kg", m.weight);
            }
            s_displayCache.addEntry(buf);
        }
        if (m.distance != 0) {
            char buf[48];
            if (config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL) {
                snprintf(buf, sizeof(buf), "Level: %.2f in", m.distance * 0.03937f);
            } else {
                snprintf(buf, sizeof(buf), "Level: %.0f mm", m.distance);
            }
            s_displayCache.addEntry(buf);
        }
        if (m.radiation != 0) {
            char buf[48];
            snprintf(buf, sizeof(buf), "Rad: %.2f µR/h", m.radiation);
            s_displayCache.addEntry(buf);
        }

        // IAQ string
        if (m.iaq != 0) {
            const char *rating = (m.iaq <= 25)    ? " (Excellent)"
                                 : (m.iaq <= 50)  ? " (Good)"
                                 : (m.iaq <= 100) ? " (Moderate)"
                                 : (m.iaq <= 150) ? " (Poor)"
                                 : (m.iaq <= 200) ? " (Unhealthy)"
                                 : (m.iaq <= 300) ? " (Very Unhealthy)"
                                                  : " (Hazardous)";
            snprintf(s_displayCache.iaqStr, sizeof(s_displayCache.iaqStr), "IAQ: %d%s", m.iaq, rating);
        } else {
            s_displayCache.iaqStr[0] = '\0';
        }

        // Cache sender name (only changes when sender changes)
        strncpy(s_displayCache.leftStr, getSenderName(sourceNode), sizeof(s_displayCache.leftStr) - 1);
        s_displayCache.leftStr[sizeof(s_displayCache.leftStr) - 1] = '\0';
    }

    // === Build timestamp string (cached, only updates when time changes) ===
    static uint32_t lastAgoSecs = UINT32_MAX;
    static char cachedDisplayStr[64];
    // Calculate time since telemetry was received using stored rxTime
    uint32_t agoSecs = (histToShow->rxTime > 0) ? (millis() - histToShow->rxTime) / 1000 : 0;

    // Only rebuild timestamp string when seconds change
    if (agoSecs != lastAgoSecs || s_displayCache.dirty) {
        lastAgoSecs = agoSecs;
        char agoBuf[16];
        if (agoSecs > 864000) {
            snprintf(agoBuf, sizeof(agoBuf), "?");
        } else if (agoSecs > 3600) {
            snprintf(agoBuf, sizeof(agoBuf), "%luh", (unsigned long)(agoSecs / 3600));
        } else if (agoSecs > 60) {
            snprintf(agoBuf, sizeof(agoBuf), "%lum", (unsigned long)(agoSecs / 60));
        } else {
            snprintf(agoBuf, sizeof(agoBuf), "%lus", (unsigned long)agoSecs);
        }
        snprintf(cachedDisplayStr, sizeof(cachedDisplayStr), "%s (%s)", s_displayCache.leftStr, agoBuf);
    }
    const char *displayStr = cachedDisplayStr;

    // Clear stale IAQ data if reading is too old (> 1 hour)
    // Note: We don't check m.iaq == 0 here because that could mean:
    // 1. IAQ sensor still calibrating (should show "IAQ: 0 (Excellent)" as feedback)
    // 2. No IAQ sensor (iaqStr will already be empty from cache rebuild)
    if (agoSecs > 3600) {
        s_displayCache.iaqStr[0] = '\0';
    }

    // === Now render ===
    // Detect large display devices (SenseCAP Indicator 480x480, T-Deck 320x240)
    bool isLargeDisplay = false;
#if defined(SENSECAP_INDICATOR) || defined(T_DECK)
    isLargeDisplay = true;
#endif

    // Use advanced display with sparklines only on high-resolution screens
    if (graphics::isHighResolution() && isLargeDisplay) {
        // === LARGE DISPLAY SCROLLABLE LAYOUT with graphs for all metrics ===

        // Calculate available scroll area (starts right after header)
        int scrollTop = currentY;
        int scrollBottom = SCREEN_HEIGHT;
        int visibleHeight = scrollBottom - scrollTop;

        // Full-width graphs spanning to screen edge
        const int graphW = SCREEN_WIDTH - x - 2;
        // T-Deck has smaller screen in landscape, use smaller graphs
#if defined(T_DECK)
        const int graphH = kLargeSparkH / 3; // 1/3 height for T-Deck
#else
        const int graphH = kLargeSparkH; // Full height for SenseCAP Indicator
#endif

        // Use FONT_SMALL which is medium-sized (19px) on large TFT displays
        display->setFont(FONT_SMALL);
        const int labelRowHeight = FONT_HEIGHT_SMALL;

        // Build list of all metric rows with graphs (use fixed array to avoid heap allocation)
        struct MetricRow {
            char label[64];
            const RingF<kHistLen> *hist;
            int height;        // custom height for this row
            bool convertTemp;  // convert Celsius to Fahrenheit
            bool convertPress; // convert hPa to inHg
            String unit;
        };
        static constexpr size_t MAX_METRIC_ROWS = 25;
        MetricRow metricRows[MAX_METRIC_ROWS];
        size_t metricRowCount = 0;
        bool isImperial = (config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL);

        // Add sender/timestamp as first scrollable row (text-only, no graph)
        if (metricRowCount < MAX_METRIC_ROWS) {
            strncpy(metricRows[metricRowCount].label, displayStr, sizeof(metricRows[metricRowCount].label) - 1);
            metricRows[metricRowCount].hist = nullptr;
            metricRows[metricRowCount].height = labelRowHeight + 2;
            metricRows[metricRowCount].convertTemp = false;
            metricRows[metricRowCount].convertPress = false;
            metricRows[metricRowCount].unit = "";
            metricRowCount++;
        }

        // Temperature with graph
        if (m.has_temperature && metricRowCount < MAX_METRIC_ROWS) {
            if (isImperial) {
                snprintf(metricRows[metricRowCount].label, sizeof(metricRows[metricRowCount].label), "Temperature: %.1f°F",
                         UnitConversions::CelsiusToFahrenheit(m.temperature));
            } else {
                snprintf(metricRows[metricRowCount].label, sizeof(metricRows[metricRowCount].label), "Temperature: %.1f°C",
                         m.temperature);
            }
            metricRows[metricRowCount].hist = (nh.temp.len >= 2) ? &nh.temp : nullptr;
            metricRows[metricRowCount].height =
                labelRowHeight + (nh.temp.len >= 2 ? (graphH + 4) : 0); // label + graph (if available)
            metricRows[metricRowCount].convertTemp = isImperial;
            metricRows[metricRowCount].convertPress = false;
            metricRows[metricRowCount].unit = isImperial ? "°F" : "°C";
            metricRowCount++;
        }

        // Humidity with graph
        if (m.has_relative_humidity && metricRowCount < MAX_METRIC_ROWS) {
            snprintf(metricRows[metricRowCount].label, sizeof(metricRows[metricRowCount].label), "Humidity: %.0f%%",
                     m.relative_humidity);
            metricRows[metricRowCount].hist = (nh.hum.len >= 2) ? &nh.hum : nullptr;
            metricRows[metricRowCount].height = labelRowHeight + (nh.hum.len >= 2 ? (graphH + 4) : 0);
            metricRows[metricRowCount].convertTemp = false;
            metricRows[metricRowCount].convertPress = false;
            metricRows[metricRowCount].unit = "%";
            metricRowCount++;
        }

        // Pressure with graph
        if (m.barometric_pressure != 0 && metricRowCount < MAX_METRIC_ROWS) {
            if (isImperial) {
                snprintf(metricRows[metricRowCount].label, sizeof(metricRows[metricRowCount].label), "Pressure: %.2f inHg",
                         UnitConversions::HectoPascalToInchesOfMercury(m.barometric_pressure));
            } else {
                snprintf(metricRows[metricRowCount].label, sizeof(metricRows[metricRowCount].label), "Pressure: %.0f hPa",
                         m.barometric_pressure);
            }
            metricRows[metricRowCount].hist = (nh.press.len >= 2) ? &nh.press : nullptr;
            metricRows[metricRowCount].height = labelRowHeight + (nh.press.len >= 2 ? (graphH + 4) : 0);
            metricRows[metricRowCount].convertTemp = false;
            metricRows[metricRowCount].convertPress = isImperial;
            metricRows[metricRowCount].unit = isImperial ? "inHg" : "hPa";
            metricRowCount++;
        }

        // Add all other metrics with graphs where available
        for (size_t i = 0; i < s_displayCache.entryCount && metricRowCount < MAX_METRIC_ROWS; i++) {
            const char *entry = s_displayCache.entries[i];
            const RingF<kHistLen> *histPtr = nullptr;
            bool needsTempConv = false;
            String unitStr = "";

            // Match entries to history data by checking metric type and set units
            if (strncmp(entry, "Dew:", 4) == 0) {
                histPtr = (nh.dewPoint.len >= 2) ? &nh.dewPoint : nullptr;
                needsTempConv = isImperial;
                unitStr = isImperial ? "°F" : "°C";
            } else if (strncmp(entry, "Gas:", 4) == 0) {
                histPtr = (nh.gas.len >= 2) ? &nh.gas : nullptr;
                unitStr = "kΩ";
            } else if (strstr(entry, "V /") != nullptr) {
                histPtr = (nh.voltage.len >= 2) ? &nh.voltage : nullptr;
                unitStr = "V";
            } else if (strncmp(entry, "Light:", 6) == 0) {
                histPtr = (nh.lux.len >= 2) ? &nh.lux : nullptr;
                unitStr = "lx";
            } else if (strncmp(entry, "White:", 6) == 0) {
                histPtr = (nh.whiteLux.len >= 2) ? &nh.whiteLux : nullptr;
                unitStr = "lx";
            } else if (strncmp(entry, "Weight:", 7) == 0) {
                histPtr = (nh.weight.len >= 2) ? &nh.weight : nullptr;
                unitStr = isImperial ? "lbs" : "kg";
            } else if (strncmp(entry, "Level:", 6) == 0) {
                histPtr = (nh.distance.len >= 2) ? &nh.distance : nullptr;
                unitStr = isImperial ? "in" : "mm";
            } else if (strncmp(entry, "Rad:", 4) == 0) {
                histPtr = (nh.radiation.len >= 2) ? &nh.radiation : nullptr;
                unitStr = "µR/h";
            } else if (strncmp(entry, "Wind:", 5) == 0) {
                histPtr = (nh.windSpeed.len >= 2) ? &nh.windSpeed : nullptr;
                unitStr = isImperial ? "mph" : "m/s";
            } else if (strncmp(entry, "Dir:", 4) == 0) {
                histPtr = (nh.windDirection.len >= 2) ? &nh.windDirection : nullptr;
                unitStr = "°";
            } else if (strncmp(entry, "Soil T:", 7) == 0) {
                histPtr = (nh.soilTemp.len >= 2) ? &nh.soilTemp : nullptr;
                needsTempConv = isImperial;
                unitStr = isImperial ? "°F" : "°C";
            } else if (strncmp(entry, "Soil M:", 7) == 0) {
                histPtr = (nh.soilMoisture.len >= 2) ? &nh.soilMoisture : nullptr;
                unitStr = "%";
            }

            strncpy(metricRows[metricRowCount].label, entry, sizeof(metricRows[metricRowCount].label) - 1);
            metricRows[metricRowCount].hist = histPtr;
            metricRows[metricRowCount].height = labelRowHeight + (histPtr ? (graphH + 4) : 0);
            metricRows[metricRowCount].convertTemp = needsTempConv;
            metricRows[metricRowCount].convertPress = false;
            metricRows[metricRowCount].unit = unitStr;
            metricRowCount++;
        }

        // Add IAQ as last scrollable row with ruler (no sparkline)
        bool hasIAQRow = false;
        const int iaqRulerHeight = kRulerBaselineOfs + kNeedleH + FONT_HEIGHT_SMALL + 4;
        if (m.iaq != 0 && metricRowCount < MAX_METRIC_ROWS) {
            // Use cached string if available, otherwise create temporary label
            if (s_displayCache.iaqStr[0] != '\0') {
                strncpy(metricRows[metricRowCount].label, s_displayCache.iaqStr, sizeof(metricRows[metricRowCount].label) - 1);
            } else {
                snprintf(metricRows[metricRowCount].label, sizeof(metricRows[metricRowCount].label), "IAQ: %d", m.iaq);
            }
            metricRows[metricRowCount].hist = nullptr; // No sparkline for IAQ, will use ruler
            metricRows[metricRowCount].height = iaqRulerHeight;
            metricRows[metricRowCount].convertTemp = false;
            metricRows[metricRowCount].convertPress = false;
            metricRows[metricRowCount].unit = "";
            hasIAQRow = true;
            metricRowCount++;
        }

        // Calculate total content height
        int totalContentHeight = 0;
        for (size_t i = 0; i < metricRowCount; i++) {
            totalContentHeight += metricRows[i].height;
        }

        // Clamp scroll position
        float maxScroll = (float)(totalContentHeight - visibleHeight);
        if (maxScroll < 0.0f)
            maxScroll = 0.0f;
        if (s_scrollY < 0.0f)
            s_scrollY = 0.0f;
        if (s_scrollY > maxScroll)
            s_scrollY = maxScroll;

        // Render visible metric rows with offset (convert float to int for rendering)
        int yOffset = scrollTop - (int)s_scrollY;
        int cumulativeY = 0;

        for (size_t i = 0; i < metricRowCount; i++) {
            bool isIAQRow = hasIAQRow && (i == metricRowCount - 1);
            int rowY = yOffset + cumulativeY;
            int actualRowHeight = metricRows[i].height;

            // Draw if any part of the row is visible
            if (rowY < scrollBottom && rowY + actualRowHeight > scrollTop) {
                if (isIAQRow) {
                    // Draw IAQ ruler instead of text+graph
                    const int rulerW = SCREEN_WIDTH - 2 * x;
                    drawIAQRuler(display, x, rowY, rulerW, m.iaq, metricRows[i].label);
                } else {
                    // Draw label (use Ohm-aware function for gas resistance)
                    drawStringWithOhm(display, x, rowY, metricRows[i].label);

                    // Draw graph if history data exists
                    if (metricRows[i].hist && metricRows[i].hist->len >= 2) {
                        int graphY = rowY + labelRowHeight;
                        drawLargeSparkBoxed(display, x, graphY, graphW, graphH, *metricRows[i].hist, metricRows[i].unit,
                                            metricRows[i].convertTemp, metricRows[i].convertPress);
                    }
                }
            }

            cumulativeY += actualRowHeight;
        }

        // Draw scrollbar if content exceeds visible area
        const int kScrollbarWidth = 3;
        if (totalContentHeight > visibleHeight) {
            int scrollbarX = SCREEN_WIDTH - kScrollbarWidth;
            int scrollbarHeight = visibleHeight;
            int thumbHeight = std::max(6, (scrollbarHeight * visibleHeight) / totalContentHeight);
            int thumbY = scrollTop + (int)((scrollbarHeight - thumbHeight) * s_scrollY / maxScroll);

            for (int i = 0; i < thumbHeight; i++) {
                display->setPixel(scrollbarX, thumbY + i);
                display->setPixel(scrollbarX + 1, thumbY + i);
            }
        }
    } else if (graphics::isHighResolution()) {
        // === SCROLLABLE HIGH-RES LAYOUT with sparklines for all metrics ===

        // Calculate available scroll area (starts right after header)
        int scrollTop = currentY;
        int scrollBottom = SCREEN_HEIGHT;
        int visibleHeight = scrollBottom - scrollTop;

        // Use nh from histToShow (already set up earlier in drawFrame)
        // No need to look up again - sourceNode and nh are already available

        // Calculate sparkline width dynamically to use available space
        // Find maximum label width to calculate remaining space
        int maxLabelWidth = 0;
        if (s_displayCache.tempStr[0] != '\0') {
            int w = display->getStringWidth(s_displayCache.tempStr);
            if (w > maxLabelWidth)
                maxLabelWidth = w;
        }
        if (s_displayCache.humStr[0] != '\0') {
            int w = display->getStringWidth(s_displayCache.humStr);
            if (w > maxLabelWidth)
                maxLabelWidth = w;
        }
        if (s_displayCache.pressStr[0] != '\0') {
            int w = display->getStringWidth(s_displayCache.pressStr);
            if (w > maxLabelWidth)
                maxLabelWidth = w;
        }
        for (size_t i = 0; i < s_displayCache.entryCount; i++) {
            int w = display->getStringWidth(s_displayCache.entries[i]);
            if (w > maxLabelWidth)
                maxLabelWidth = w;
        }

        // Sparkline width: remaining space minus padding and scrollbar
        const int kPadding = 6; // space between label and graph
        const int kScrollbarWidth = 3;
        const int kSparkW = std::max(50, SCREEN_WIDTH - maxLabelWidth - kPadding - x - kScrollbarWidth - 2);
        const int graphX = SCREEN_WIDTH - kSparkW - kScrollbarWidth - 2;

        // Build list of all metric rows with sparklines (use fixed array to avoid heap allocation)
        struct MetricRow {
            const char *label;
            const RingF<kHistLen> *hist;
        };
        static constexpr size_t MAX_METRIC_ROWS = 20;
        MetricRow metricRows[MAX_METRIC_ROWS];
        size_t metricRowCount = 0;

        // Add sender/timestamp as first scrollable row
        if (metricRowCount < MAX_METRIC_ROWS)
            metricRows[metricRowCount++] = {displayStr, nullptr};

        // Add all metrics that have data
        if (s_displayCache.tempStr[0] != '\0' && metricRowCount < MAX_METRIC_ROWS)
            metricRows[metricRowCount++] = {s_displayCache.tempStr, &nh.temp};
        if (s_displayCache.humStr[0] != '\0' && metricRowCount < MAX_METRIC_ROWS)
            metricRows[metricRowCount++] = {s_displayCache.humStr, &nh.hum};
        if (s_displayCache.pressStr[0] != '\0' && metricRowCount < MAX_METRIC_ROWS)
            metricRows[metricRowCount++] = {s_displayCache.pressStr, &nh.press};

        // Add entries from cache with corresponding history data
        for (size_t i = 0; i < s_displayCache.entryCount && metricRowCount < MAX_METRIC_ROWS; i++) {
            const char *entry = s_displayCache.entries[i];
            const RingF<kHistLen> *histPtr = nullptr;

            // Match entries to history data by checking metric type
            if (strncmp(entry, "Dew:", 4) == 0)
                histPtr = (nh.dewPoint.len >= 2) ? &nh.dewPoint : nullptr;
            else if (strncmp(entry, "Gas:", 4) == 0)
                histPtr = (nh.gas.len >= 2) ? &nh.gas : nullptr;
            else if (strstr(entry, "V /") != nullptr)
                histPtr = (nh.voltage.len >= 2) ? &nh.voltage : nullptr;
            else if (strncmp(entry, "Light:", 6) == 0)
                histPtr = (nh.lux.len >= 2) ? &nh.lux : nullptr;
            else if (strncmp(entry, "White:", 6) == 0)
                histPtr = (nh.whiteLux.len >= 2) ? &nh.whiteLux : nullptr;
            else if (strncmp(entry, "Weight:", 7) == 0)
                histPtr = (nh.weight.len >= 2) ? &nh.weight : nullptr;
            else if (strncmp(entry, "Level:", 6) == 0)
                histPtr = (nh.distance.len >= 2) ? &nh.distance : nullptr;
            else if (strncmp(entry, "Rad:", 4) == 0)
                histPtr = (nh.radiation.len >= 2) ? &nh.radiation : nullptr;
            else if (strncmp(entry, "Wind:", 5) == 0)
                histPtr = (nh.windSpeed.len >= 2) ? &nh.windSpeed : nullptr;
            else if (strncmp(entry, "Dir:", 4) == 0)
                histPtr = (nh.windDirection.len >= 2) ? &nh.windDirection : nullptr;
            else if (strncmp(entry, "Soil T:", 7) == 0)
                histPtr = (nh.soilTemp.len >= 2) ? &nh.soilTemp : nullptr;
            else if (strncmp(entry, "Soil M:", 7) == 0)
                histPtr = (nh.soilMoisture.len >= 2) ? &nh.soilMoisture : nullptr;

            metricRows[metricRowCount++] = {entry, histPtr};
        }

        // Add IAQ as last scrollable row (no sparkline) - only if we actually have IAQ data
        // Note: Don't rely on cached string alone since it may be cleared for stale data
        bool hasIAQRow = false;
        static char iaqFallbackLabel[48];
        if (m.iaq != 0 && metricRowCount < MAX_METRIC_ROWS) {
            // Use cached string if available, otherwise create temporary label
            const char *iaqLabel;
            if (s_displayCache.iaqStr[0] != '\0') {
                iaqLabel = s_displayCache.iaqStr;
            } else {
                snprintf(iaqFallbackLabel, sizeof(iaqFallbackLabel), "IAQ: %d", m.iaq);
                iaqLabel = iaqFallbackLabel;
            }
            metricRows[metricRowCount++] = {iaqLabel, nullptr};
            hasIAQRow = true;
        }

        // Calculate total content height accounting for variable row heights
        int totalContentHeight = 0;
        for (size_t i = 0; i < metricRowCount; i++) {
            bool isIAQRow = hasIAQRow && (i == metricRowCount - 1);
            int rowHeightForCalc = isIAQRow ? (kRulerBaselineOfs + kNeedleH + FONT_HEIGHT_SMALL + 4) : rowHeight;
            totalContentHeight += rowHeightForCalc;
        }

        // Clamp scroll position
        float maxScroll = (float)(totalContentHeight - visibleHeight);
        if (maxScroll < 0.0f)
            maxScroll = 0.0f;
        if (s_scrollY < 0.0f)
            s_scrollY = 0.0f;
        if (s_scrollY > maxScroll)
            s_scrollY = maxScroll;

        // Render visible metric rows with offset (convert float to int for rendering)
        // Calculate IAQ ruler actual height
        const int iaqRulerHeight = kRulerBaselineOfs + kNeedleH + FONT_HEIGHT_SMALL + 4;

        int yOffset = scrollTop - (int)s_scrollY;
        int cumulativeY = 0; // Track actual Y position accounting for variable row heights

        for (size_t i = 0; i < metricRowCount; i++) {
            // Check if this is the IAQ row (last one if IAQ was added)
            bool isIAQRow = hasIAQRow && (i == metricRowCount - 1);

            // Calculate row position and height
            int rowY = yOffset + cumulativeY;
            int actualRowHeight = isIAQRow ? iaqRulerHeight : rowHeight;

            // Draw if any part of the row is visible (allow partial rendering)
            if (rowY < scrollBottom && rowY + actualRowHeight > scrollTop) {
                if (isIAQRow) {
                    // Draw IAQ ruler instead of text+sparkline
                    const int rulerW = SCREEN_WIDTH - 2 * x;
                    drawIAQRuler(display, x, rowY, rulerW, m.iaq, metricRows[i].label);
                } else {
                    // Draw label (use Ohm-aware function for gas resistance)
                    drawStringWithOhm(display, x, rowY, metricRows[i].label);

                    // Draw sparkline if history data exists (no offset, 1px gap comes from rowHeight spacing)
                    if (metricRows[i].hist && metricRows[i].hist->len >= 2) {
                        drawMiniSparkBoxed(display, graphX, rowY, kSparkW, rowHeight - 1, *metricRows[i].hist);
                    }
                }
            }

            // Advance cumulative position by actual row height
            cumulativeY += actualRowHeight;
        }

        // Draw scrollbar if content exceeds visible area
        if (totalContentHeight > visibleHeight) {
            int scrollbarX = SCREEN_WIDTH - 2;
            int scrollbarHeight = visibleHeight;
            int thumbHeight = std::max(6, (scrollbarHeight * visibleHeight) / totalContentHeight);
            int thumbY = scrollTop + (int)((scrollbarHeight - thumbHeight) * s_scrollY / maxScroll);

            for (int i = 0; i < thumbHeight; i++) {
                display->setPixel(scrollbarX, thumbY + i);
            }
        }
    } else {
        // Simple display for low-resolution screens (like develop branch)
        // Build all metrics list using fixed array
        const char *allMetrics[20];
        size_t metricCount = 0;

        if (s_displayCache.tempStr[0] != '\0')
            allMetrics[metricCount++] = s_displayCache.tempStr;
        if (s_displayCache.humStr[0] != '\0')
            allMetrics[metricCount++] = s_displayCache.humStr;
        if (s_displayCache.pressStr[0] != '\0')
            allMetrics[metricCount++] = s_displayCache.pressStr;

        // Add all other entries
        for (size_t i = 0; i < s_displayCache.entryCount; i++) {
            allMetrics[metricCount++] = s_displayCache.entries[i];
        }

        // First row: sender/time on left, first metric right-aligned on right (if available)
        graphics::MessageRenderer::drawStringWithEmotes(display, x, currentY, displayStr, emotes, numEmotes);

#if defined(M5STACK_UNITC6L)
        // For M5STACK_UNITC6L only: put sender/time on first line, then all metrics on separate lines
        currentY += rowHeight;
        for (size_t i = 0; i < metricCount; i++) {
            drawStringWithOhm(display, x, currentY, allMetrics[i]);
            currentY += rowHeight;
        }
#else
        size_t startIdx = 0;
        if (metricCount > 0) {
            int rightX = SCREEN_WIDTH - display->getStringWidth(allMetrics[0]);
            drawStringWithOhm(display, rightX, currentY, allMetrics[0]);
            startIdx = 1;
        }
        currentY += rowHeight;

        // Remaining metrics in 2-column format
        const int splitX = SCREEN_WIDTH / 2;
        for (size_t i = startIdx; i < metricCount; i += 2) {
            // Left column
            drawStringWithOhm(display, x, currentY, allMetrics[i]);

            // Right column
            if (i + 1 < metricCount) {
                drawStringWithOhm(display, splitX, currentY, allMetrics[i + 1]);
            }

            currentY += rowHeight;
        }
#endif
    }

    // === IAQ alert logic (banner/beep for dangerous levels) ===
    if (s_displayCache.iaqStr[0] != '\0' && m.iaq != 0) {
        const char *bannerMsg = nullptr;
        if (m.iaq > 200 && m.iaq <= 300)
            bannerMsg = "Very Unhealthy IAQ";
        else if (m.iaq > 300)
            bannerMsg = "Hazardous IAQ";
        else if (m.iaq > 150)
            bannerMsg = "Unhealthy IAQ";

        static uint32_t lastAlertTime = 0;
        static bool inBanner = false;
        uint32_t now = millis();
        bool isCooldownOver = (now - lastAlertTime > 60000);
        bool isOwnTelemetry = sourceNode == nodeDB->getNodeNum();
        if (!inBanner && isOwnTelemetry && bannerMsg && isCooldownOver) {
            inBanner = true;
            lastAlertTime = now;
            screen->showSimpleBanner(bannerMsg, 3000);
            if (m.iaq > 200 && moduleConfig.external_notification.enabled && !externalNotificationModule->getMute())
                playLongBeep();
            inBanner = false;
        }
    }

    // Redraw header on top of scrollable content to prevent scrolled items from appearing over it
    if (graphics::isHighResolution()) {
        graphics::drawCommonHeader(display, x, y, titleStr);
    }

    graphics::drawCommonFooter(display, x, y);
}
#endif

bool EnvironmentTelemetryModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_Telemetry *t)
{
    if (t->which_variant == meshtastic_Telemetry_environment_metrics_tag) {
#if defined(DEBUG_PORT) && !defined(DEBUG_MUTE)
        const char *sender = getSenderShortName(mp);

        LOG_INFO("(Received from %s): barometric_pressure=%f, current=%f, gas_resistance=%f, relative_humidity=%f, "
                 "temperature=%f",
                 sender, t->variant.environment_metrics.barometric_pressure, t->variant.environment_metrics.current,
                 t->variant.environment_metrics.gas_resistance, t->variant.environment_metrics.relative_humidity,
                 t->variant.environment_metrics.temperature);
        LOG_INFO("(Received from %s): voltage=%f, IAQ=%d, distance=%f, lux=%f, white_lux=%f", sender,
                 t->variant.environment_metrics.voltage, t->variant.environment_metrics.iaq,
                 t->variant.environment_metrics.distance, t->variant.environment_metrics.lux,
                 t->variant.environment_metrics.white_lux);

        LOG_INFO("(Received from %s): wind speed=%fm/s, direction=%d degrees, weight=%fkg", sender,
                 t->variant.environment_metrics.wind_speed, t->variant.environment_metrics.wind_direction,
                 t->variant.environment_metrics.weight);

        LOG_INFO("(Received from %s): radiation=%fµR/h", sender, t->variant.environment_metrics.radiation);

#endif
        // Reserve map capacity on first use to prevent reallocation during traffic
        if (!s_histReserved) {
            s_hist.reserve(kMaxHistNodes);
            s_histReserved = true;
        }

        // Track per-source telemetry
        uint32_t from = mp.from;
        const auto &em = t->variant.environment_metrics;

        // Enforce maximum node limit to prevent memory exhaustion
        // CRITICAL: Clean up stale nodes first, THEN check capacity
        cleanupStaleNodes();

        // Check if this is a new node
        auto it = s_hist.find(from);
        bool isNewNode = (it == s_hist.end());

        // If new node and at capacity, evict stalest to make room
        if (isNewNode && s_hist.size() >= kMaxHistNodes) {
            uint32_t stalestNode = 0;
            uint32_t stalestTime = UINT32_MAX;
            for (const auto &kv : s_hist) {
                if (kv.second.lastUpdate < stalestTime) {
                    stalestTime = kv.second.lastUpdate;
                    stalestNode = kv.first;
                }
            }

            // Evict stalest node (must succeed to make room)
            if (stalestNode != 0 && stalestNode != from) {
                LOG_DEBUG("EnvironmentTelemetry: Evicting node 0x%08x to make room for new node 0x%08x", stalestNode, from);
                s_hist.erase(stalestNode);
                it = s_hist.end(); // Invalidate iterator after erase
            } else {
                // Cannot evict - likely all nodes are current node, skip this telemetry
                LOG_WARN("EnvironmentTelemetry: Cannot add node 0x%08x - map full and no node to evict", from);
                return false;
            }
        }

        // Get or create NodeHist entry - check heap before attempting allocation
        if (isNewNode) {
            // Check minimum heap before attempting allocation (need ~2KB for NodeHist + map overhead)
            if (memGet.getFreeHeap() < 8192) {
                LOG_WARN("EnvironmentTelemetry: Insufficient heap (%u bytes) to add node 0x%08x", memGet.getFreeHeap(), from);
                return false;
            }
            // Use emplace to construct in-place, check if it succeeded
            auto result = s_hist.emplace(from, NodeHist<kHistLen>());
            if (!result.second) {
                LOG_ERROR("EnvironmentTelemetry: Failed to allocate NodeHist for node 0x%08x", from);
                return false;
            }
            it = result.first;
        }

        // Store metrics and history - safe now because we made room
        NodeHist<kHistLen> &nh2 = it->second;
        nh2.lastUpdate = millis();
        nh2.rxTime = millis(); // For "ago" display
        nh2.lastMetrics = em;  // Store full metrics for display
        if (em.has_temperature)
            nh2.temp.push(em.temperature);
        if (em.has_relative_humidity)
            nh2.hum.push(em.relative_humidity);
        if (em.barometric_pressure)
            nh2.press.push(em.barometric_pressure);
        if (em.has_gas_resistance || em.gas_resistance != 0)
            nh2.gas.push(em.gas_resistance);
        if (em.voltage != 0)
            nh2.voltage.push(em.voltage);
        if (em.current != 0)
            nh2.current.push(em.current);
        if (em.lux != 0)
            nh2.lux.push(em.lux);
        if (em.white_lux != 0)
            nh2.whiteLux.push(em.white_lux);
        if (em.weight != 0)
            nh2.weight.push(em.weight);
        if (em.distance != 0)
            nh2.distance.push(em.distance);
        if (em.radiation != 0)
            nh2.radiation.push(em.radiation);
        if (em.wind_speed != 0)
            nh2.windSpeed.push(em.wind_speed);
        if (em.wind_direction != 0)
            nh2.windDirection.push(em.wind_direction);
        if (em.soil_temperature != 0)
            nh2.soilTemp.push(em.soil_temperature);
        if (em.soil_moisture != 0)
            nh2.soilMoisture.push(em.soil_moisture);
        // Calculate dew point if we have temp and humidity
        if (em.has_temperature && em.has_relative_humidity && em.relative_humidity > 0.0f) {
            float dpC = dewPointC(em.temperature, em.relative_humidity);
            if (!isnan(dpC))
                nh2.dewPoint.push(dpC);
        }

        // Track most recent source for auto-display and mark cache dirty
        s_lastSource = from;
        s_displayCache.markDirty();

        // Screen will refresh on next UI cycle - no need to force
    }

    return false; // Let others look at this message also if they want
}

bool EnvironmentTelemetryModule::getEnvironmentTelemetry(meshtastic_Telemetry *m)
{
    bool valid = true;
    bool hasEnvSensor = false; // Track if we have actual environmental sensors
    m->time = getTime();
    m->which_variant = meshtastic_Telemetry_environment_metrics_tag;
    m->variant.environment_metrics = meshtastic_EnvironmentMetrics_init_zero;

    for (TelemetrySensor *sensor : sensors) {
        valid = valid && sensor->getMetrics(m);
        hasEnvSensor = true; // These are actual environmental sensors
    }

#ifdef HAS_RAKPROT
    valid = valid && rak9154Sensor.getMetrics(m);
    hasEnvSensor = true; // RAK9154 is environmental
#endif

    // Early return if we don't have environmental sensors - don't try to read power sensors
    if (!hasEnvSensor) {
        return false;
    }

#ifndef T1000X_SENSOR_EN
    if (ina219Sensor.hasSensor()) {
        valid = valid && ina219Sensor.getMetrics(m);
    }
    if (ina260Sensor.hasSensor()) {
        valid = valid && ina260Sensor.getMetrics(m);
    }
    if (ina3221Sensor.hasSensor()) {
        valid = valid && ina3221Sensor.getMetrics(m);
    }
    if (max17048Sensor.hasSensor()) {
        valid = valid && max17048Sensor.getMetrics(m);
    }
#endif
    // Only return true if we have actual environmental sensors (not just power sensors)
    return valid && hasEnvSensor;
}

meshtastic_MeshPacket *EnvironmentTelemetryModule::allocReply()
{
    if (currentRequest) {
        auto req = *currentRequest;
        const auto &p = req.decoded;
        meshtastic_Telemetry scratch;
        meshtastic_Telemetry *decoded = NULL;
        memset(&scratch, 0, sizeof(scratch));
        if (pb_decode_from_bytes(p.payload.bytes, p.payload.size, &meshtastic_Telemetry_msg, &scratch)) {
            decoded = &scratch;
        } else {
            LOG_ERROR("Error decoding EnvironmentTelemetry module!");
            return NULL;
        }
        // Check for a request for environment metrics
        if (decoded->which_variant == meshtastic_Telemetry_environment_metrics_tag) {
            meshtastic_Telemetry m = meshtastic_Telemetry_init_zero;
            if (getEnvironmentTelemetry(&m)) {
                LOG_INFO("Environment telemetry reply to request");
                return allocDataProtobuf(m);
            } else {
                return NULL;
            }
        }
    }
    return NULL;
}

bool EnvironmentTelemetryModule::sendTelemetry(NodeNum dest, bool phoneOnly)
{
    meshtastic_Telemetry m = meshtastic_Telemetry_init_zero;
    m.which_variant = meshtastic_Telemetry_environment_metrics_tag;
    m.time = getTime();

    if (!getEnvironmentTelemetry(&m)) {
        return false;
    }

    LOG_INFO("Send: barometric_pressure=%f, current=%f, gas_resistance=%f, relative_humidity=%f, temperature=%f",
             m.variant.environment_metrics.barometric_pressure, m.variant.environment_metrics.current,
             m.variant.environment_metrics.gas_resistance, m.variant.environment_metrics.relative_humidity,
             m.variant.environment_metrics.temperature);
    LOG_INFO("Send: voltage=%f, IAQ=%d, distance=%f, lux=%f", m.variant.environment_metrics.voltage,
             m.variant.environment_metrics.iaq, m.variant.environment_metrics.distance, m.variant.environment_metrics.lux);

    LOG_INFO("Send: wind speed=%fm/s, direction=%d degrees, weight=%fkg", m.variant.environment_metrics.wind_speed,
             m.variant.environment_metrics.wind_direction, m.variant.environment_metrics.weight);

    LOG_INFO("Send: radiation=%fµR/h", m.variant.environment_metrics.radiation);

    LOG_INFO("Send: soil_temperature=%f, soil_moisture=%u", m.variant.environment_metrics.soil_temperature,
             m.variant.environment_metrics.soil_moisture);

    sensor_read_error_count = 0;

    meshtastic_MeshPacket *p = allocDataProtobuf(m);
    p->to = dest;
    p->decoded.want_response = false;
    if (config.device.role == meshtastic_Config_DeviceConfig_Role_SENSOR)
        p->priority = meshtastic_MeshPacket_Priority_RELIABLE;
    else
        p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;

    // Store own metrics in s_hist (no packet storage needed)
    uint32_t self = nodeDB->getNodeNum();
    const auto &em = m.variant.environment_metrics;
    auto &selfHist = s_hist[self];
    selfHist.lastUpdate = millis();
    selfHist.rxTime = millis(); // For "ago" display
    selfHist.lastMetrics = em;  // Store full metrics for display
    if (em.has_temperature)
        selfHist.temp.push(em.temperature);
    if (em.has_relative_humidity)
        selfHist.hum.push(em.relative_humidity);
    if (em.barometric_pressure)
        selfHist.press.push(em.barometric_pressure);
    if (em.has_gas_resistance || em.gas_resistance != 0)
        selfHist.gas.push(em.gas_resistance);
    if (em.voltage != 0)
        selfHist.voltage.push(em.voltage);
    if (em.current != 0)
        selfHist.current.push(em.current);
    if (em.lux != 0)
        selfHist.lux.push(em.lux);
    if (em.white_lux != 0)
        selfHist.whiteLux.push(em.white_lux);
    if (em.weight != 0)
        selfHist.weight.push(em.weight);
    if (em.distance != 0)
        selfHist.distance.push(em.distance);
    if (em.radiation != 0)
        selfHist.radiation.push(em.radiation);
    if (em.wind_speed != 0)
        selfHist.windSpeed.push(em.wind_speed);
    if (em.wind_direction != 0)
        selfHist.windDirection.push(em.wind_direction);
    if (em.soil_temperature != 0)
        selfHist.soilTemp.push(em.soil_temperature);
    if (em.soil_moisture != 0)
        selfHist.soilMoisture.push(em.soil_moisture);
    // Calculate dew point if we have temp and humidity
    if (em.has_temperature && em.has_relative_humidity && em.relative_humidity > 0.0f) {
        float dpC = dewPointC(em.temperature, em.relative_humidity);
        if (!isnan(dpC))
            selfHist.dewPoint.push(dpC);
    }

    // Track most recent source for auto-display and mark cache dirty
    s_lastSource = self;
    s_displayCache.markDirty();
    // Screen will refresh on next UI cycle

    if (phoneOnly) {
        LOG_INFO("Send packet to phone");
        service->sendToPhone(p);
    } else {
        LOG_INFO("Send packet to mesh");
        service->sendToMesh(p, RX_SRC_LOCAL, true);

        if (config.device.role == meshtastic_Config_DeviceConfig_Role_SENSOR && config.power.is_power_saving) {
            meshtastic_ClientNotification *notification = clientNotificationPool.allocZeroed();
            notification->level = meshtastic_LogRecord_Level_INFO;
            notification->time = getValidTime(RTCQualityFromNet);
            sprintf(notification->message, "Sending telemetry and sleeping for %us interval in a moment",
                    Default::getConfiguredOrDefaultMs(moduleConfig.telemetry.environment_update_interval,
                                                      default_telemetry_broadcast_interval_secs) /
                        1000U);
            service->sendClientNotification(notification);
            sleepOnNextExecution = true;
            LOG_DEBUG("Start next execution in 5s, then sleep");
            setIntervalFromNow(FIVE_SECONDS_MS);
        }
    }
    return true;
}

AdminMessageHandleResult EnvironmentTelemetryModule::handleAdminMessageForModule(const meshtastic_MeshPacket &mp,
                                                                                 meshtastic_AdminMessage *request,
                                                                                 meshtastic_AdminMessage *response)
{
    AdminMessageHandleResult result = AdminMessageHandleResult::NOT_HANDLED;
#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR_EXTERNAL

    for (TelemetrySensor *sensor : sensors) {
        result = sensor->handleAdminMessage(mp, request, response);
        if (result != AdminMessageHandleResult::NOT_HANDLED)
            return result;
    }

    if (ina219Sensor.hasSensor()) {
        result = ina219Sensor.handleAdminMessage(mp, request, response);
        if (result != AdminMessageHandleResult::NOT_HANDLED)
            return result;
    }
    if (ina260Sensor.hasSensor()) {
        result = ina260Sensor.handleAdminMessage(mp, request, response);
        if (result != AdminMessageHandleResult::NOT_HANDLED)
            return result;
    }
    if (ina3221Sensor.hasSensor()) {
        result = ina3221Sensor.handleAdminMessage(mp, request, response);
        if (result != AdminMessageHandleResult::NOT_HANDLED)
            return result;
    }
    if (max17048Sensor.hasSensor()) {
        result = max17048Sensor.handleAdminMessage(mp, request, response);
        if (result != AdminMessageHandleResult::NOT_HANDLED)
            return result;
    }
#endif
    return result;
}

#endif
