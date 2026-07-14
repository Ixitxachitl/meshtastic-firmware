#pragma once
#include <OLEDDisplay.h>
#include <stdint.h>

// Nearest-neighbour scaled XBM draw. scale <= 1 falls through to drawXbm().
static inline void drawXbmScaled(OLEDDisplay *display, int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t *bits,
                                 int16_t scale)
{
    if (scale <= 1) {
        display->drawXbm(x, y, w, h, bits);
        return;
    }
    const int16_t rowBytes = static_cast<int16_t>((w + 7) / 8);
    for (int16_t row = 0; row < h; row++) {
        for (int16_t col = 0; col < w; col++) {
            if (bits[row * rowBytes + col / 8] & (1u << (col & 7)))
                display->fillRect(x + col * scale, y + row * scale, scale, scale);
        }
    }
}
