#ifndef OLEDDISPLAYFONTSTOMTHUMB_h
#define OLEDDISPLAYFONTSTOMTHUMB_h

#ifdef ARDUINO
#include <Arduino.h>
#elif __MBED__
#define PROGMEM
#endif

/**
 * Tom Thumb for tiny displays: 3x6 glyphs on a 4px advance, so every character
 * carries its own trailing column of spacing and neighbours never touch.
 * Public domain font by Robey Pointer
 */
extern const uint8_t TomThumb4x6[] PROGMEM;

#endif
