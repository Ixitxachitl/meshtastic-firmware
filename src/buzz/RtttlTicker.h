#pragma once

#include "configuration.h"

// Advances the NonBlockingRTTTL sequencer from a FreeRTOS timer, so the next note does not wait behind
// the cooperative main loop. tone() is hardware timed, so only note starts need servicing.
// Also off when RTTTL is compiled out, since NonBlockingRtttl is then absent.
#if (defined(ARCH_NRF52) || defined(ARCH_ESP32)) && !MESHTASTIC_EXCLUDE_RTTTL
#define HAS_RTTTL_TICKER 1
#else
#define HAS_RTTTL_TICKER 0
#endif

#if HAS_RTTTL_TICKER

#include <stdint.h>

namespace RtttlTicker
{
void begin(uint8_t pin, const char *song);

// Only advances the song if the timer could not be created or started; otherwise a no-op.
void pump();

void stop();
} // namespace RtttlTicker

#endif
