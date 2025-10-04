#pragma once
#include <cstddef> 

struct ToneDuration {
    int frequency_khz;
    int duration_ms;
};

// Convert a ToneDuration[] melody into RTTTL (written into `out`)
size_t tonesToRtttl(char* out, size_t outCap,
                    const ToneDuration* td, int n,
                    const char* name);

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
size_t tonesToRtttl(char* out, size_t outCap, const ToneDuration* td, int n, const char* name);