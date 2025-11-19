#pragma once
#include <cstddef>
#include <cstdint>

struct ToneDuration {
    int frequency_khz;
    int duration_ms;
};

// Convert a ToneDuration[] melody into RTTTL (written into `out`)
size_t tonesToRtttl(char *out, size_t outCap, const ToneDuration *td, int n, const char *name);

void playBeep();
void playLongBeep();
void playStartMelody();
void playShutdownMelody();
void playGPSEnableBeep();
void playGPSDisableBeep();
void playComboTune();
void playBoop();
void playChirp();
void playLongPressLeadUp();
bool playNextLeadUpNote();  // Play the next note in the lead-up sequence
void resetLeadUpSequence(); // Reset the lead-up sequence to start from beginning

#ifdef HAS_I2S
void buzzOnAudioThreadReady();

extern "C" void armAudioWarmup(uint8_t ticks);

// Signal AudioThread to prefill more aggressively for the next `ms`.
void buzzBoostFor(uint32_t ms);

// AudioThread queries this to know if it should boost prefill.
bool buzzBoostActive();
#endif