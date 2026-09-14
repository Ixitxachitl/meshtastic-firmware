#pragma once

#include "configuration.h"

// Why the motor is asked to pulse. It follows what a touch turned out to be, not the finger landing: the touch
// layer pulses for gestures it can name, and a tap is acknowledged by whatever it landed on, so a dead tap is silent.
enum class TouchHaptic : uint8_t {
    Gesture,   // a swipe, or the start of a drag
    LongPress, // press-and-hold reached its threshold
    Activate,  // a tap that landed on something
};

// No-op on boards with no motor under the glass.
void touchHapticPulse(TouchHaptic reason);
