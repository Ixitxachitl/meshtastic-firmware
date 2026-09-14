#include "TouchHaptics.h"
#include "main.h"

void touchHapticPulse(TouchHaptic reason)
{
    (void)reason;
// The boards that have always buzzed under a finger; other DRV2605 boards carry it for notifications only.
#if (defined(T_WATCH_S3) || defined(T_WATCH_ULTRA)) && defined(HAS_DRV2605)
    // Library 1, effect 75: one sharp click. The driver runs the waveform itself, so this doesn't block.
    drv.setWaveform(0, 75);
    drv.setWaveform(1, 0); // end waveform
    drv.go();
#endif
}
