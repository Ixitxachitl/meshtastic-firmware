#ifndef OLEDDISPLAYFONTSOHM_h
#define OLEDDISPLAYFONTSOHM_h

#ifdef ARDUINO
#include <Arduino.h>
#elif __MBED__
#define PROGMEM
#endif

/**
 * Ohm symbol (Ω) bitmaps for direct XBM rendering.
 * Use drawXbm() to render these at the appropriate size.
 *
 * Character: Ω (Greek capital Omega)
 * Unicode: U+03A9
 * UTF-8: 0xCE 0xA9
 */

// XBM bitmaps for direct rendering
extern const uint8_t OhmBitmap_10[] PROGMEM; // 5x7 for ArialMT_Plain_10
extern const int OhmWidth_10;
extern const int OhmHeight_10;

extern const uint8_t OhmBitmap_16[] PROGMEM; // 8x11 for ArialMT_Plain_16
extern const int OhmWidth_16;
extern const int OhmHeight_16;

extern const uint8_t OhmBitmap_24[] PROGMEM; // 9x12 for ArialMT_Plain_24
extern const int OhmWidth_24;
extern const int OhmHeight_24;

extern const uint8_t OhmBitmap_Tiny[] PROGMEM; // 3x5 for TomThumb
extern const int OhmWidth_Tiny;
extern const int OhmHeight_Tiny;

// Legacy font arrays (kept for backward compatibility)
extern const uint8_t OhmGlyph_10[] PROGMEM;
extern const uint8_t OhmGlyph_16[] PROGMEM;
extern const uint8_t OhmGlyph_24[] PROGMEM;
extern const uint8_t OhmGlyph_Tiny[] PROGMEM;

#endif
