#include "buzz.h"
#include "NodeDB.h"
#include "configuration.h"

#if !defined(ARCH_ESP32) && !defined(ARCH_RP2040) && !defined(ARCH_PORTDUINO)
#include "Tone.h"
#endif

#if !defined(ARCH_PORTDUINO)
extern "C" void delay(uint32_t dwMs);
#endif

#include <cstdio>
#include <cstring>
#include <algorithm>

// ---- I2S/RTTTL glue ---------------------------------------------------------
#ifdef HAS_I2S
#include "main.h"

// If you already expose a flag for using I2S as buzzer, reuse it here.
// Fall back to false if you don't have such a flag yet.
extern meshtastic_LocalModuleConfig moduleConfig;
static inline bool i2sBuzzerEnabled() {
    return moduleConfig.external_notification.use_i2s_as_buzzer;
}

// Begin an RTTTL string on the I2S audio thread and pump until done.
static inline void playRttlI2S(const char* rttl)
{
    if (!audioThread || !rttl || !*rttl) return;
    audioThread->beginRttl(rttl, strlen(rttl));
}

void pumpAudioTick() {
    if (audioThread) { (void)audioThread->isPlaying(); }
}
#endif // HAS_I2S

// Some common frequencies.
#define NOTE_C3 131
#define NOTE_CS3 139
#define NOTE_D3 147
#define NOTE_DS3 156
#define NOTE_E3 165
#define NOTE_F3 175
#define NOTE_FS3 185
#define NOTE_G3 196
#define NOTE_GS3 208
#define NOTE_A3 220
#define NOTE_AS3 233
#define NOTE_B3 247
#define NOTE_CS4 277

// ---- Tone[] -> RTTTL converter ----------------------------------------------
// We choose BPM=240 so that note lengths match your millisecond constants:
// whole=1000ms, 1/2=500ms, 1/4=250ms, 1/8=125ms, dotted 1/2=750ms (3/4)
static constexpr int kRtttlBpm = 240;

// Map your discrete frequencies to RTTTL tokens (with explicit octave).
// If a new freq appears, we'll pick the closest known one.
struct NoteToken { int freq; const char* tok; };
static const NoteToken kNoteMap[] = {
    { NOTE_C3,  "c3"  }, { NOTE_CS3, "c#3" }, { NOTE_D3,  "d3"  }, { NOTE_DS3, "d#3" },
    { NOTE_E3,  "e3"  }, { NOTE_F3,  "f3"  }, { NOTE_FS3, "f#3" }, { NOTE_G3,  "g3"  },
    { NOTE_GS3, "g#3" }, { NOTE_A3,  "a3"  }, { NOTE_AS3, "a#3" }, { NOTE_B3,  "b3"  },
    { NOTE_CS4, "c#4" },
};

// Nearest-note lookup by absolute frequency difference
static inline const char* freqToRtttl(int f)
{
    const char* best = "p"; // rest as last-resort
    int bestErr = 1e9;
    for (auto &m : kNoteMap) {
        int err = (m.freq > f) ? (m.freq - f) : (f - m.freq);
        if (err < bestErr) { bestErr = err; best = m.tok; }
    }
    return best;
}

// Duration token (1,2,4,8,16) + optional dotted flag, chosen by closest ms.
// We only need to cover the lengths used in buzz.cpp.
struct DurCand { int denom; bool dotted; int ms; };
static const DurCand kDur[] = {
    { 1,  false, 1000 }, // whole
    { 2,  true,   750 }, // dotted half (3/4)
    { 2,  false,  500 }, // half
    { 4,  false,  250 }, // quarter
    { 8,  false,  125 }, // eighth
    // { 16,false,   63 }, // 1/16th if you add faster blips later
};

static inline void chooseDur(int ms, int &denom, bool &dotted)
{
    int bestIdx = 0, bestErr = 1e9;
    for (int i = 0; i < (int)(sizeof(kDur)/sizeof(kDur[0])); ++i) {
        int err = (kDur[i].ms > ms) ? (kDur[i].ms - ms) : (ms - kDur[i].ms);
        if (err < bestErr) { bestErr = err; bestIdx = i; }
    }
    denom = kDur[bestIdx].denom;
    dotted = kDur[bestIdx].dotted;
}

// Build an RTTTL string into a caller-supplied buffer.
// Format: <name>:d=8,o=5,b=240:<notes...>
// We emit explicit durations per note to be precise; default d/o are still set.
size_t tonesToRtttl(char* out, size_t outCap,
                                  const ToneDuration* td, int n,
                                  const char* name = "sys")
{
    if (!out || outCap == 0) return 0;
    out[0] = '\0';

    // Header
    int wrote = snprintf(out, outCap, "%s:d=8,o=5,b=%d:", name, kRtttlBpm);
    if (wrote <= 0 || (size_t)wrote >= outCap) return (size_t)std::max(0, wrote);
    size_t pos = (size_t)wrote;

    // Body
    for (int i = 0; i < n; ++i) {
        int denom; bool dotted;
        chooseDur(td[i].duration_ms, denom, dotted);
        const char* note = freqToRtttl(td[i].frequency_khz);

        // token like "8c#3." or "4b3"
        char token[16];
        if (dotted)
            snprintf(token, sizeof(token), "%d%s.", denom, note);
        else
            snprintf(token, sizeof(token), "%d%s", denom, note);

        // append, with comma if not last
        int need = (int)strlen(token) + ((i+1<n) ? 1 : 0);
        if (pos + (size_t)need >= outCap) break; // avoid overflow; truncated is fine
        memcpy(out + pos, token, strlen(token)); pos += strlen(token);
        if (i+1 < n) out[pos++] = ',';
        out[pos] = '\0';
    }
    return pos;
}

const int DURATION_1_8 = 125;  // 1/8 note
const int DURATION_1_4 = 250;  // 1/4 note
const int DURATION_1_2 = 500;  // 1/2 note
const int DURATION_3_4 = 750;  // 1/4 note
const int DURATION_1_1 = 1000; // 1/1 note

void playTones(const ToneDuration *tone_durations, int size)
{
    if (config.device.buzzer_mode == meshtastic_Config_DeviceConfig_BuzzerMode_DISABLED ||
        config.device.buzzer_mode == meshtastic_Config_DeviceConfig_BuzzerMode_NOTIFICATIONS_ONLY) {
        // Buzzer is disabled or not set to system tones
        return;
    }
    
#ifdef HAS_I2S
    if (i2sBuzzerEnabled()) {
        // Convert current ToneDuration[] to RTTTL and play over I2S
        char rttl[192]; // plenty for the short melodies in buzz.cpp
        tonesToRtttl(rttl, sizeof(rttl), tone_durations, size, "sys");
        playRttlI2S(rttl);
        return;
    }
#endif    
    
#ifdef PIN_BUZZER
    if (!config.device.buzzer_gpio)
        config.device.buzzer_gpio = PIN_BUZZER;
#endif
    if (config.device.buzzer_gpio) {
        for (int i = 0; i < size; i++) {
            const auto &tone_duration = tone_durations[i];
            tone(config.device.buzzer_gpio, tone_duration.frequency_khz, tone_duration.duration_ms);
            // to distinguish the notes, set a minimum time between them.
            delay(1.3 * tone_duration.duration_ms);
        }
    }
}

void playBeep()
{
    ToneDuration melody[] = {{NOTE_B3, DURATION_1_8}};
    playTones(melody, sizeof(melody) / sizeof(ToneDuration));
}

void playLongBeep()
{
    ToneDuration melody[] = {{NOTE_B3, DURATION_1_1}};
    playTones(melody, sizeof(melody) / sizeof(ToneDuration));
}

void playGPSEnableBeep()
{
    ToneDuration melody[] = {{NOTE_C3, DURATION_1_8}, {NOTE_FS3, DURATION_1_4}, {NOTE_CS4, DURATION_1_4}};
    playTones(melody, sizeof(melody) / sizeof(ToneDuration));
}

void playGPSDisableBeep()
{
    ToneDuration melody[] = {{NOTE_CS4, DURATION_1_8}, {NOTE_FS3, DURATION_1_4}, {NOTE_C3, DURATION_1_4}};
    playTones(melody, sizeof(melody) / sizeof(ToneDuration));
}

void playStartMelody()
{
    ToneDuration melody[] = {{NOTE_FS3, DURATION_1_8}, {NOTE_AS3, DURATION_1_8}, {NOTE_CS4, DURATION_1_4}};
    playTones(melody, sizeof(melody) / sizeof(ToneDuration));
}

void playShutdownMelody()
{
    ToneDuration melody[] = {{NOTE_CS4, DURATION_1_8}, {NOTE_AS3, DURATION_1_8}, {NOTE_FS3, DURATION_1_4}};
    playTones(melody, sizeof(melody) / sizeof(ToneDuration));
}

void playChirp()
{
    // A short, friendly "chirp" sound for key presses
    ToneDuration melody[] = {{NOTE_AS3, 20}}; // Very short AS3 note
    playTones(melody, sizeof(melody) / sizeof(ToneDuration));
}

void playBoop()
{
    // A short, friendly "boop" sound for button presses
    ToneDuration melody[] = {{NOTE_A3, 50}}; // Very short A3 note
    playTones(melody, sizeof(melody) / sizeof(ToneDuration));
}

void playLongPressLeadUp()
{
    // An ascending lead-up sequence for long press - builds anticipation
    ToneDuration melody[] = {
        {NOTE_C3, 100}, // Start low
        {NOTE_E3, 100}, // Step up
        {NOTE_G3, 100}, // Keep climbing
        {NOTE_B3, 150}  // Peak with longer note for emphasis
    };
    playTones(melody, sizeof(melody) / sizeof(ToneDuration));
}

// Static state for progressive lead-up notes
static int leadUpNoteIndex = 0;
static const ToneDuration leadUpNotes[] = {
    {NOTE_C3, 100}, // Start low
    {NOTE_E3, 100}, // Step up
    {NOTE_G3, 100}, // Keep climbing
    {NOTE_B3, 150}  // Peak with longer note for emphasis
};
static const int leadUpNotesCount = sizeof(leadUpNotes) / sizeof(ToneDuration);

bool playNextLeadUpNote()
{
    if (leadUpNoteIndex >= leadUpNotesCount) {
        return false; // All notes have been played
    }

    // Use playTones to handle buzzer logic consistently
    const auto &note = leadUpNotes[leadUpNoteIndex];
    playTones(&note, 1); // Play single note using existing playTones function

    leadUpNoteIndex++;

    if (leadUpNoteIndex >= leadUpNotesCount) {
        return false; // this was the final note
    }
    return true; // Note was played (playTones handles buzzer availability internally)
}

void resetLeadUpSequence()
{
    leadUpNoteIndex = 0;
}

void playComboTune()
{
    // Quick high-pitched notes with trills
    ToneDuration melody[] = {
        {NOTE_G3, 80},  // Quick chirp
        {NOTE_B3, 60},  // Higher chirp
        {NOTE_CS4, 80}, // Even higher
        {NOTE_G3, 60},  // Quick trill down
        {NOTE_CS4, 60}, // Quick trill up
        {NOTE_B3, 120}  // Ending chirp
    };
    playTones(melody, sizeof(melody) / sizeof(ToneDuration));
}
