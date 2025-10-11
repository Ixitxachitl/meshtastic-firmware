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
#include "main.h"   // for audioThread
#include "NodeDB.h"        // for moduleConfig type

#if defined(HAS_I2S) && defined(ESP32)

// ---- Pending/queue state must live here (one definition, file-local) ----
extern AudioThread* audioThread;     // ok to keep extern for the pointer
static char g_pendingRttl[384];
static volatile bool g_hasPending = false;

// 5 ms while playing/pending, 20 ms idle
static inline TickType_t pumpIntervalTicks() {
    if (!audioThread) return pdMS_TO_TICKS(20);
    bool playing = audioThread->isPlaying();   // also advances one frame
    return pdMS_TO_TICKS((playing || g_hasPending) ? 5 : 20);
}
#endif

// match the declaration already in NodeDB.h
extern meshtastic_LocalModuleConfig moduleConfig;

// Return the same flag your ExternalNotificationModule uses
static inline bool i2sBuzzerEnabled() {
    // If your LocalModuleConfig doesn’t have this field on some targets,
    // you can temporarily return false to compile:
    // return false;
    return moduleConfig.external_notification.use_i2s_as_buzzer;
}

static inline void queueRttl(const char* rttl) {
    if (!rttl || !*rttl) return;
    std::strncpy(g_pendingRttl, rttl, sizeof(g_pendingRttl) - 1);
    g_pendingRttl[sizeof(g_pendingRttl) - 1] = '\0';
    g_hasPending = true;
}

// Non-blocking start. If audioThread isn't ready yet, queue it.
static inline void startRttlI2S(const char* rttl) {
    if (!rttl || !*rttl) return;
    if (!audioThread) { queueRttl(rttl); return; }
    audioThread->beginRttl(rttl, std::strlen(rttl));
}

void buzzOnAudioThreadReady() {
    if (!audioThread || !g_hasPending) return;
    // If nothing is currently playing, start the queued RTTTL
    if (!audioThread->isPlaying()) {
        audioThread->beginRttl(g_pendingRttl, std::strlen(g_pendingRttl));
        g_hasPending = false;
        // Warm prime the DMA a few frames so the first audible chunk is smooth.
        for (int i = 0; i < 6; ++i) {
            (void)audioThread->isPlaying();  // advances one frame
            delay(0);                         // yield without sleeping
        }
    }
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
// Faster tempo so we can express very short beeps:
// At 480 BPM: whole=500ms, 1/2=250ms, 1/4=125ms, 1/8=62ms, 1/16=31ms, 1/32=15–16ms
static constexpr int kRtttlBpm = 480;

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
    { 1,  false,  500 }, // whole
    { 2,  true,   375 }, // dotted half
    { 2,  false,  250 }, // half
    { 4,  false,  125 }, // quarter
    { 8,  false,   62 }, // eighth
    { 16, false,   31 }, // sixteenth
    { 32, false,   16 }, // thirty-second  (handy for 20–50ms chirps)
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
        // We’ll also add a 1/32 rest after non-final notes to mimic spacing.
        bool veryShort = (td[i].duration_ms < 80);
        const char* sepRest = (i + 1 < n) ? (veryShort ? ",32p" : ",") : "";
        int need = (int)strlen(token) + (int)strlen(sepRest);
        if (pos + (size_t)need >= outCap) break; // avoid overflow; truncated is fine
        memcpy(out + pos, token, strlen(token)); pos += strlen(token);
        if (*sepRest) { memcpy(out + pos, sepRest, strlen(sepRest)); pos += strlen(sepRest); }
        out[pos] = '\0';
    }
    return pos;
}

const int DURATION_1_8 = 125;  // 1/8 note
const int DURATION_1_4 = 250;  // 1/4 note
const int DURATION_1_2 = 500;  // 1/2 note
const int DURATION_3_4 = 750;  // 3/4 note
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
        char rttl[384]; // plenty for the short melodies in buzz.cpp
        tonesToRtttl(rttl, sizeof(rttl), tone_durations, size, "sys");
        startRttlI2S(rttl);
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
#ifdef HAS_I2S
    if (i2sBuzzerEnabled()) {
        char rttl[384];  // match or smaller than g_pendingRttl
        tonesToRtttl(rttl, sizeof(rttl), melody, (int)(sizeof(melody)/sizeof(melody[0])), "start");
        startRttlI2S(rttl);   // fire-and-forget; queues if audioThread not ready
        return;
    }
#endif
    playTones(melody, sizeof(melody) / sizeof(ToneDuration));
}

void playLongBeep()
{
    ToneDuration melody[] = {{NOTE_B3, DURATION_1_1}};
#ifdef HAS_I2S
    if (i2sBuzzerEnabled()) {
        char rttl[384];  // match or smaller than g_pendingRttl
        tonesToRtttl(rttl, sizeof(rttl), melody, (int)(sizeof(melody)/sizeof(melody[0])), "start");
        startRttlI2S(rttl);   // fire-and-forget; queues if audioThread not ready
        return;
    }
#endif
    playTones(melody, sizeof(melody) / sizeof(ToneDuration));
}

void playGPSEnableBeep()
{
    ToneDuration melody[] = {{NOTE_C3, DURATION_1_8}, {NOTE_FS3, DURATION_1_4}, {NOTE_CS4, DURATION_1_4}};
#ifdef HAS_I2S
    if (i2sBuzzerEnabled()) {
        char rttl[384];  // match or smaller than g_pendingRttl
        tonesToRtttl(rttl, sizeof(rttl), melody, (int)(sizeof(melody)/sizeof(melody[0])), "start");
        startRttlI2S(rttl);   // fire-and-forget; queues if audioThread not ready
        return;
    }
#endif
    playTones(melody, sizeof(melody) / sizeof(ToneDuration));
}

void playGPSDisableBeep()
{
    ToneDuration melody[] = {{NOTE_CS4, DURATION_1_8}, {NOTE_FS3, DURATION_1_4}, {NOTE_C3, DURATION_1_4}};
#ifdef HAS_I2S
    if (i2sBuzzerEnabled()) {
        char rttl[384];  // match or smaller than g_pendingRttl
        tonesToRtttl(rttl, sizeof(rttl), melody, (int)(sizeof(melody)/sizeof(melody[0])), "start");
        startRttlI2S(rttl);   // fire-and-forget; queues if audioThread not ready
        return;
    }
#endif
    playTones(melody, sizeof(melody) / sizeof(ToneDuration));
}

void playStartMelody() {
    ToneDuration melody[] = {
        { NOTE_FS3, 250 }, { NOTE_AS3, 250 }, { NOTE_CS4, 500 }
    };
#ifdef HAS_I2S
    if (i2sBuzzerEnabled()) {
        char rttl[384];  // match or smaller than g_pendingRttl
        tonesToRtttl(rttl, sizeof(rttl), melody, (int)(sizeof(melody)/sizeof(melody[0])), "start");
        startRttlI2S(rttl);   // fire-and-forget; queues if audioThread not ready
        return;
    }
#endif
    // GPIO fallback
    playTones(melody, (int)(sizeof(melody)/sizeof(melody[0])));
}

void playShutdownMelody()
{
    ToneDuration melody[] = {{NOTE_CS4, DURATION_1_8}, {NOTE_AS3, DURATION_1_8}, {NOTE_FS3, DURATION_1_4}};
#ifdef HAS_I2S
    if (i2sBuzzerEnabled()) {
        char rttl[384];  // match or smaller than g_pendingRttl
        tonesToRtttl(rttl, sizeof(rttl), melody, (int)(sizeof(melody)/sizeof(melody[0])), "start");
        startRttlI2S(rttl);   // fire-and-forget; queues if audioThread not ready
        return;
    }
#endif
    playTones(melody, sizeof(melody) / sizeof(ToneDuration));
}

void playChirp()
{
    // A short, friendly "chirp" sound for key presses
    ToneDuration melody[] = {{NOTE_AS3, 20}}; // Very short AS3 note
#ifdef HAS_I2S
    if (i2sBuzzerEnabled()) {
        char rttl[384];  // match or smaller than g_pendingRttl
        tonesToRtttl(rttl, sizeof(rttl), melody, (int)(sizeof(melody)/sizeof(melody[0])), "start");
        startRttlI2S(rttl);   // fire-and-forget; queues if audioThread not ready
        return;
    }
#endif
    playTones(melody, sizeof(melody) / sizeof(ToneDuration));
}

void playBoop()
{
    // A short, friendly "boop" sound for button presses
    ToneDuration melody[] = {{NOTE_A3, 50}}; // Very short A3 note
#ifdef HAS_I2S
    if (i2sBuzzerEnabled()) {
        char rttl[384];  // match or smaller than g_pendingRttl
        tonesToRtttl(rttl, sizeof(rttl), melody, (int)(sizeof(melody)/sizeof(melody[0])), "start");
        startRttlI2S(rttl);   // fire-and-forget; queues if audioThread not ready
        return;
    }
#endif
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
#ifdef HAS_I2S
    if (i2sBuzzerEnabled()) {
        char rttl[384];  // match or smaller than g_pendingRttl
        tonesToRtttl(rttl, sizeof(rttl), melody, (int)(sizeof(melody)/sizeof(melody[0])), "start");
        startRttlI2S(rttl);   // fire-and-forget; queues if audioThread not ready
        return;
    }
#endif
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
#ifdef HAS_I2S
    if (i2sBuzzerEnabled()) {
        char rttl[384];  // match or smaller than g_pendingRttl
        tonesToRtttl(rttl, sizeof(rttl), melody, (int)(sizeof(melody)/sizeof(melody[0])), "start");
        startRttlI2S(rttl);   // fire-and-forget; queues if audioThread not ready
        return;
    }
#endif
    playTones(melody, sizeof(melody) / sizeof(ToneDuration));
}
