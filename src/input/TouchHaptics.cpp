#include "TouchHaptics.h"
#include "main.h"

void touchHapticPulse(TouchHaptic reason)
{
    (void)reason;
// The same two boards the touch layer used to pulse on, not every board with the chip: the others
// carry a DRV2605 for notification buzzes and have never buzzed under a finger.
#if (defined(T_WATCH_S3) || defined(T_WATCH_ULTRA)) && defined(HAS_DRV2605)
    // Library 1, effect 75: a single sharp click. Fire-and-forget - the driver runs the waveform
    // itself, so this doesn't block the caller for the length of the pulse.
    drv.setWaveform(0, 75);
    drv.setWaveform(1, 0); // end waveform
    drv.go();
#endif
}
