#pragma once

#include "configuration.h"

// Why the motor is being asked to pulse.
//
// It used to fire on touch-down, which is the one moment nothing is known yet: a tap that did
// nothing at all felt exactly like pressing a key. So the pulse now follows what the touch turned
// out to be. The touch layer buzzes for the gestures it can name itself, and a tap is acknowledged
// by whatever consumes it - a key, an on-screen button, a banner option. A tap that lands on
// nothing reaches no one, and correctly feels like nothing.
//
// The click is the same for all three today; the distinction is what earns one, not how it feels.
// Kept as an enum anyway so a board that wants to tell them apart has somewhere to say so.
enum class TouchHaptic : uint8_t {
    Gesture,   // a swipe, or the start of a drag: the view is about to follow the finger
    LongPress, // press-and-hold reached its threshold
    Activate,  // a tap that landed on something
};

// No-op on boards with no motor under the glass.
void touchHapticPulse(TouchHaptic reason);
