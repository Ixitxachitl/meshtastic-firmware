#ifndef HUGEDISPLAYFONTS_h
#define HUGEDISPLAYFONTS_h

#ifdef T_WATCH_ULTRA

#ifdef ARDUINO
#include <Arduino.h>
#elif __MBED__
#define PROGMEM
#endif

/**
 * DejaVu Sans Plain 32
 *
 * The largest glyph table 'ThingPulse/esp8266-oled-ssd1306' ships is 24pt, which
 * reads tiny on the 410x502 AMOLED. This fills the tier above it so the T-Watch
 * Ultra keeps a size hierarchy between FONT_SMALL and FONT_MEDIUM/FONT_LARGE.
 */
extern const uint8_t DejaVuSans_plain_32[] PROGMEM;

#endif // T_WATCH_ULTRA

#endif
