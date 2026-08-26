#pragma once

#include "configuration.h"

#ifdef ARCH_NRF52

#include <stdint.h>

// Drives the NonBlockingRTTTL sequencer from a FreeRTOS software timer instead of the cooperative
// main loop. tone() is hardware timed, so a note ends on time regardless; what stutters is the
// start of the next note waiting behind a long display refresh. A 2 ms timer keeps that gap fixed.
namespace NRF52RtttlTicker
{
void begin(uint8_t pin, const char *song);

// Only advances the song if the timer could not be created; otherwise a no-op.
void pump();

void stop();
} // namespace NRF52RtttlTicker

#endif
