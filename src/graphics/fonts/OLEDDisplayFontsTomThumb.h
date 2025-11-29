#ifndef OLEDDISPLAYFONTSTOMTHUMB_h
#define OLEDDISPLAYFONTSTOMTHUMB_h

#ifdef ARDUINO
#include <Arduino.h>
#elif __MBED__
#define PROGMEM
#endif

/**
 * Tom Thumb 4x6 fixed-width font for tiny displays
 * Public domain font by Robey Pointer
 */
extern const uint8_t TomThumb4x6[] PROGMEM;

#endif
