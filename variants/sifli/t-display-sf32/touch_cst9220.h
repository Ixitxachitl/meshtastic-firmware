#pragma once

#include <stdbool.h>
#include <stdint.h>

// CST9220 capacitive touch controller, on the display FPC behind the 1V8
// level shifter at I2C 0x5A.
//
// LovyanGFX has no driver for this part, so touch does not come through the
// panel; the variant reads it directly and hands readTouch() to
// TouchScreenImpl1.

// Pulses the controller's reset line and waits for it to come up.
void touchInit();

// Reads the first touch point. Returns false when nothing is being touched.
bool readTouch(int16_t *x, int16_t *y);
