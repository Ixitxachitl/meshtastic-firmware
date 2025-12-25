#include "buzz.h"
#include "NodeDB.h"
#include "configuration.h"

#ifdef SENSECAP_INDICATOR
#include "mesh/IndicatorSerial.h"
#endif

#if !defined(ARCH_ESP32) && !defined(ARCH_RP2040) && !defined(ARCH_PORTDUINO)
#include "Tone.h"
#endif

#if !defined(ARCH_PORTDUINO)
extern "C" void delay(uint32_t dwMs);
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>

// ==============================================================================
// Constants
// ==============================================================================

// RTTL buffer size for melody conversion
static constexpr size_t RTTL_BUFFER_SIZE = 384;

// Audio boost duration for clean I2S playback startup
static constexpr uint32_t BUZZ_DEFAULT_START_BOOST_MS = 800;

// Standard note durations at 240 BPM (easier to read/understand than 480 BPM)
// These match musical notation more intuitively
static constexpr int DURATION_1_8 = 125;  // 1/8 note
static constexpr int DURATION_1_4 = 250;  // 1/4 note
static constexpr int DURATION_1_2 = 500;  // 1/2 note
static constexpr int DURATION_3_4 = 750;  // 3/4 note
static constexpr int DURATION_1_1 = 1000; // whole note

// ==============================================================================
// Note Frequencies (scientific pitch notation)
// ==============================================================================

// Octave 3
static constexpr int NOTE_C3 = 131;
static constexpr int NOTE_CS3 = 139;
static constexpr int NOTE_D3 = 147;
static constexpr int NOTE_DS3 = 156;
static constexpr int NOTE_E3 = 165;
static constexpr int NOTE_F3 = 175;
static constexpr int NOTE_FS3 = 185;
static constexpr int NOTE_G3 = 196;
static constexpr int NOTE_GS3 = 208;
static constexpr int NOTE_A3 = 220;
static constexpr int NOTE_AS3 = 233;
static constexpr int NOTE_B3 = 247;

// Octave 4
static constexpr int NOTE_C4 = 262;
static constexpr int NOTE_CS4 = 277;
static constexpr int NOTE_D4 = 294;
static constexpr int NOTE_DS4 = 311;
static constexpr int NOTE_E4 = 330;
static constexpr int NOTE_F4 = 349;
static constexpr int NOTE_FS4 = 370;
static constexpr int NOTE_G4 = 392;
static constexpr int NOTE_GS4 = 415;
static constexpr int NOTE_A4 = 440;
static constexpr int NOTE_AS4 = 466;
static constexpr int NOTE_B4 = 494;

// Octave 5 (needed for startup melody)
static constexpr int NOTE_C5 = 523;
static constexpr int NOTE_CS5 = 554;

// ==============================================================================
// I2S/RTTL Audio System
// ==============================================================================

#ifdef HAS_I2S
#include "main.h" // for audioThread

#if defined(HAS_I2S) && defined(ESP32)

// Audio boost timing for clean I2S startup
static volatile uint32_t g_audioBoostUntilMs = 0;

void buzzBoostFor(uint32_t ms)
{
    g_audioBoostUntilMs = millis() + ms;
}

bool buzzBoostActive()
{
    return (int32_t)(millis() - g_audioBoostUntilMs) < 0;
}

// Queued RTTL melody system (for cases where audioThread isn't ready yet)
extern AudioThread *audioThread;
static char g_pendingRttl[RTTL_BUFFER_SIZE];
static volatile bool g_hasPending = false;

// Pump interval: 5ms while active, 20ms idle
static inline TickType_t pumpIntervalTicks()
{
    if (!audioThread)
        return pdMS_TO_TICKS(20);
    bool playing = audioThread->isPlaying();
    return pdMS_TO_TICKS((playing || g_hasPending) ? 5 : 20);
}
#endif

extern meshtastic_LocalModuleConfig moduleConfig;

static inline bool i2sBuzzerEnabled()
{
    return moduleConfig.external_notification.use_i2s_as_buzzer;
}

static inline void queueRttl(const char *rttl)
{
    if (!rttl || !*rttl)
        return;
    std::strncpy(g_pendingRttl, rttl, sizeof(g_pendingRttl) - 1);
    g_pendingRttl[sizeof(g_pendingRttl) - 1] = '\0';
    g_hasPending = true;
}

static inline void startRttlI2S(const char *rttl)
{
    if (!rttl || !*rttl)
        return;
    if (!audioThread) {
        queueRttl(rttl);
        return;
    }
    buzzBoostFor(BUZZ_DEFAULT_START_BOOST_MS);
    audioThread->beginRttl(rttl, std::strlen(rttl));
}

void buzzOnAudioThreadReady()
{
    if (!audioThread || !g_hasPending)
        return;
    if (!audioThread->isPlaying()) {
        buzzBoostFor(BUZZ_DEFAULT_START_BOOST_MS);
        audioThread->beginRttl(g_pendingRttl, std::strlen(g_pendingRttl));
        g_hasPending = false;
        // Prime the DMA buffer for smooth playback
        for (int i = 0; i < 6; ++i) {
            (void)audioThread->isPlaying();
            delay(0);
        }
    }
}
#endif // HAS_I2S

// ==============================================================================
// RTTL Conversion System
// ==============================================================================

// Use 240 BPM for faster tempo to better match GPIO buzzer timing
// At 240 BPM: whole=1000ms, half=500ms, quarter=250ms, eighth=125ms, sixteenth=62.5ms
static constexpr int RTTL_BPM = 240;

// Frequency to RTTL note mapping
// Note: octave is stored separately and only added when it differs from default
struct NoteToken {
    int freq;
    const char *note; // Note name with sharp (e.g., "c#", "f#", "a")
    int octave;       // Octave number
};

static const NoteToken NOTE_MAP[] = {
    // Octave 3
    {NOTE_C3, "c", 3},
    {NOTE_CS3, "c#", 3},
    {NOTE_D3, "d", 3},
    {NOTE_DS3, "d#", 3},
    {NOTE_E3, "e", 3},
    {NOTE_F3, "f", 3},
    {NOTE_FS3, "f#", 3},
    {NOTE_G3, "g", 3},
    {NOTE_GS3, "g#", 3},
    {NOTE_A3, "a", 3},
    {NOTE_AS3, "a#", 3},
    {NOTE_B3, "b", 3},
    // Octave 4
    {NOTE_C4, "c", 4},
    {NOTE_CS4, "c#", 4},
    {NOTE_D4, "d", 4},
    {NOTE_DS4, "d#", 4},
    {NOTE_E4, "e", 4},
    {NOTE_F4, "f", 4},
    {NOTE_FS4, "f#", 4},
    {NOTE_G4, "g", 4},
    {NOTE_GS4, "g#", 4},
    {NOTE_A4, "a", 4},
    {NOTE_AS4, "a#", 4},
    {NOTE_B4, "b", 4},
    // Octave 5
    {NOTE_C5, "c", 5},
    {NOTE_CS5, "c#", 5},
};

// Find the closest note for a given frequency
// Returns index into NOTE_MAP, or -1 for rest/pause
static inline int freqToNoteIndex(int freq)
{
    if (freq == 0)
        return -1; // Rest/pause

    int bestIdx = -1;
    int bestErr = 1000000;

    for (size_t i = 0; i < sizeof(NOTE_MAP) / sizeof(NOTE_MAP[0]); ++i) {
        int err = (NOTE_MAP[i].freq > freq) ? (NOTE_MAP[i].freq - freq) : (freq - NOTE_MAP[i].freq);
        if (err < bestErr) {
            bestErr = err;
            bestIdx = i;
        }
    }
    return bestIdx;
}

// Duration mapping for RTTL conversion at 240 BPM
// More comprehensive coverage for accurate timing
struct DurationCandidate {
    int denom;   // Note denominator (1=whole, 2=half, 4=quarter, etc.)
    bool dotted; // Whether this is a dotted note
    int ms;      // Milliseconds at 240 BPM
};

static const DurationCandidate DURATION_MAP[] = {
    {1, false, 1000}, // whole note
    {2, true, 750},   // dotted half
    {2, false, 500},  // half note
    {4, true, 375},   // dotted quarter
    {4, false, 250},  // quarter note
    {8, true, 187},   // dotted eighth
    {8, false, 125},  // eighth note
    {16, true, 94},   // dotted sixteenth
    {16, false, 62},  // sixteenth note
    {32, false, 31},  // thirty-second note (for very short beeps)
};

static inline void chooseDuration(int ms, int &denom, bool &dotted)
{
    int bestIdx = 0;
    int bestErr = 1000000;

    for (size_t i = 0; i < sizeof(DURATION_MAP) / sizeof(DURATION_MAP[0]); ++i) {
        int err = (DURATION_MAP[i].ms > ms) ? (DURATION_MAP[i].ms - ms) : (ms - DURATION_MAP[i].ms);
        if (err < bestErr) {
            bestErr = err;
            bestIdx = i;
        }
    }

    denom = DURATION_MAP[bestIdx].denom;
    dotted = DURATION_MAP[bestIdx].dotted;
}

/**
 * Convert ToneDuration array to RTTL (Ring Tone Text Transfer Language) format
 *
 * RTTL Format: <name>:d=<default_duration>,o=<default_octave>,b=<bpm>:<notes>
 * Notes: <duration><note><octave>[.] where . indicates dotted note
 *
 * @param out Output buffer for RTTL string
 * @param outCap Capacity of output buffer
 * @param td Array of tone durations to convert
 * @param n Number of tones in array
 * @param name Name for the RTTL melody
 * @return Number of bytes written to output buffer
 */
size_t tonesToRtttl(char *out, size_t outCap, const ToneDuration *td, int n, const char *name)
{
    if (!out || outCap == 0 || !td || n <= 0)
        return 0;
    out[0] = '\0';

    // Find the minimum octave in the melody
    // AudioGeneratorRTTTL only supports octaves 4-7, so we normalize to start at 4
    int minOctave = 10;
    for (int i = 0; i < n; ++i) {
        int idx = freqToNoteIndex(td[i].frequency_khz);
        if (idx >= 0 && NOTE_MAP[idx].octave < minOctave) {
            minOctave = NOTE_MAP[idx].octave;
        }
    }

    // Calculate octave shift to normalize minimum octave to 4
    int octaveShift = 0;
    if (minOctave < 10) {
        octaveShift = 4 - minOctave;
    }

    // Default octave is now 4 (the normalized minimum)
    int defaultOctave = 4;

    // Write RTTL header: name:d=default_duration,o=default_octave,b=bpm:
    int wrote = snprintf(out, outCap, "%s:d=4,o=%d,b=%d:", name, defaultOctave, RTTL_BPM);
    if (wrote <= 0 || (size_t)wrote >= outCap) {
        return (size_t)std::max(0, wrote);
    }
    size_t pos = (size_t)wrote;

    int currentOctave = defaultOctave;

    // Convert each tone to RTTL note
    for (int i = 0; i < n; ++i) {
        int denom;
        bool dotted;
        chooseDuration(td[i].duration_ms, denom, dotted);

        int noteIdx = freqToNoteIndex(td[i].frequency_khz);
        char token[16];

        if (noteIdx < 0) {
            // Rest/pause
            if (dotted) {
                snprintf(token, sizeof(token), "%dp.", denom);
            } else {
                snprintf(token, sizeof(token), "%dp", denom);
            }
        } else {
            // Regular note - apply octave shift to normalize to RTTL supported range (4-7)
            const char *noteName = NOTE_MAP[noteIdx].note;
            int noteOctave = NOTE_MAP[noteIdx].octave + octaveShift;

            // Format: <duration><note>[octave if different from current]
            // Sharp notes can have octave numbers: "4f#3" works correctly in parser

            if (noteOctave != currentOctave) {
                // Octave needs to change - always add octave number for clarity
                if (dotted) {
                    snprintf(token, sizeof(token), "%d%s%d.", denom, noteName, noteOctave);
                } else {
                    snprintf(token, sizeof(token), "%d%s%d", denom, noteName, noteOctave);
                }
                currentOctave = noteOctave;
            } else {
                // Same octave - no octave number needed
                if (dotted) {
                    snprintf(token, sizeof(token), "%d%s.", denom, noteName);
                } else {
                    snprintf(token, sizeof(token), "%d%s", denom, noteName);
                }
            }
        }

        // Add separator (comma) between notes, with optional rest for very short notes
        const char *separator;
        if (i + 1 < n) {
            // Add a brief pause after very short notes for better distinction
            separator = (td[i].duration_ms < 80) ? ",32p," : ",";
        } else {
            separator = ""; // No separator after last note
        }

        // Check buffer space and append
        size_t needSpace = strlen(token) + strlen(separator);
        if (pos + needSpace >= outCap)
            break; // Avoid overflow

        memcpy(out + pos, token, strlen(token));
        pos += strlen(token);

        if (*separator) {
            memcpy(out + pos, separator, strlen(separator));
            pos += strlen(separator);
        }

        out[pos] = '\0';
    }

    return pos;
}

// ==============================================================================
// Melody Playback Functions
// ==============================================================================

/**
 * Helper function to play a melody via I2S or GPIO buzzer
 * Automatically handles I2S conversion and fallback to GPIO
 */
static void playMelody(const ToneDuration *melody, int count, const char *name = "sys")
{
    if (config.device.buzzer_mode == meshtastic_Config_DeviceConfig_BuzzerMode_DISABLED ||
        config.device.buzzer_mode == meshtastic_Config_DeviceConfig_BuzzerMode_NOTIFICATIONS_ONLY) {
        return; // Buzzer disabled or not configured for system sounds
    }

#ifdef SENSECAP_INDICATOR
    // For SenseCAP Indicator, send melody notes to RP2040 buzzer
    // RP2040 now supports tone() with frequency control
    extern SensecapIndicator *sensecapIndicator;
    if (sensecapIndicator) {
        // Play each note in the melody
        for (int i = 0; i < count; i++) {
            uint16_t frequency = melody[i].frequency_khz;
            uint16_t duration = melody[i].duration_ms;

            LOG_DEBUG("Note %d: freq=%u Hz, dur=%u ms", i, frequency, duration);
            sensecapIndicator->sendBeep(frequency, duration);

            // Wait for note to complete plus spacing (30% longer)
            delay(duration * 1.3);
        }
        return;
    }
#endif

#ifdef HAS_I2S
    if (i2sBuzzerEnabled()) {
        char rttl[RTTL_BUFFER_SIZE];
        tonesToRtttl(rttl, sizeof(rttl), melody, count, name);
        startRttlI2S(rttl);
        return;
    }
#endif

    // GPIO buzzer fallback
#ifdef PIN_BUZZER
    if (!config.device.buzzer_gpio) {
        config.device.buzzer_gpio = PIN_BUZZER;
    }
#endif

    if (config.device.buzzer_gpio) {
        for (int i = 0; i < count; i++) {
            const auto &td = melody[i];
            tone(config.device.buzzer_gpio, td.frequency_khz, td.duration_ms);
            // Add spacing between notes (30% longer than note duration)
            delay(1.3 * td.duration_ms);
        }
    }
}

void playTones(const ToneDuration *tone_durations, int size)
{
    playMelody(tone_durations, size, "sys");
}

void playBeep()
{
    ToneDuration melody[] = {{NOTE_B3, DURATION_1_8}};
    LOG_DEBUG("playBeep: NOTE_B3=%d, DURATION_1_8=%d, melody[0].freq=%d, melody[0].dur=%d", NOTE_B3, DURATION_1_8,
              melody[0].frequency_khz, melody[0].duration_ms);
    playMelody(melody, sizeof(melody) / sizeof(ToneDuration), "beep");
}

void playLongBeep()
{
    ToneDuration melody[] = {{NOTE_B3, DURATION_1_1}};
    playMelody(melody, sizeof(melody) / sizeof(ToneDuration), "longbeep");
}

void playGPSEnableBeep()
{
#if defined(R1_NEO) || defined(MUZI_BASE)
    ToneDuration melody[] = {
        {NOTE_F5, DURATION_1_2}, {NOTE_G6, DURATION_1_8}, {NOTE_E7, DURATION_1_4}, {NOTE_SILENT, DURATION_1_2}};
#else
    ToneDuration melody[] = {{NOTE_C3, DURATION_1_8}, {NOTE_FS3, DURATION_1_4}, {NOTE_CS4, DURATION_1_4}};
#endif
    playMelody(melody, sizeof(melody) / sizeof(ToneDuration), "gpson");
}

void playGPSDisableBeep()
{
#if defined(R1_NEO) || defined(MUZI_BASE)
    ToneDuration melody[] = {{NOTE_B4, DURATION_1_16}, {NOTE_B4, DURATION_1_16},   {NOTE_SILENT, DURATION_1_8},
                             {NOTE_F3, DURATION_1_16}, {NOTE_F3, DURATION_1_16},   {NOTE_SILENT, DURATION_1_8},
                             {NOTE_C3, DURATION_1_1},  {NOTE_SILENT, DURATION_1_1}};
#else
    ToneDuration melody[] = {{NOTE_CS4, DURATION_1_8}, {NOTE_FS3, DURATION_1_4}, {NOTE_C3, DURATION_1_4}};
#endif
    playMelody(melody, sizeof(melody) / sizeof(ToneDuration), "gpsoff");
}

void playStartMelody()
{
    // Original: F#3 (185Hz), A#3 (233Hz), C#4 (277Hz) - low, mid, high progression
    // The tonesToRtttl() function normalizes octaves so minimum becomes 4 (RTTL parser requirement)
    // This melody will be converted to: 4f#, 4a#, 2c#5 (octaves 4, 4, 5)
    ToneDuration melody[] = {{NOTE_FS3, 250}, {NOTE_AS3, 250}, {NOTE_CS4, 500}};
    playMelody(melody, sizeof(melody) / sizeof(ToneDuration), "start");
}

void playShutdownMelody()
{
    ToneDuration melody[] = {{NOTE_CS4, DURATION_1_8}, {NOTE_AS3, DURATION_1_8}, {NOTE_FS3, DURATION_1_4}};
    playMelody(melody, sizeof(melody) / sizeof(ToneDuration), "shutdown");
}

void playChirp()
{
    ToneDuration melody[] = {{NOTE_AS3, 20}};
    playMelody(melody, sizeof(melody) / sizeof(ToneDuration), "chirp");
}

void playBoop()
{
    ToneDuration melody[] = {{NOTE_A3, 50}};
    playMelody(melody, sizeof(melody) / sizeof(ToneDuration), "boop");
}

void playLongPressLeadUp()
{
    ToneDuration melody[] = {{NOTE_C3, 100}, {NOTE_E3, 100}, {NOTE_G3, 100}, {NOTE_B3, 150}};
    playMelody(melody, sizeof(melody) / sizeof(ToneDuration), "leadup");
}

// Static state for progressive lead-up notes
static int leadUpNoteIndex = 0;
static const ToneDuration leadUpNotes[] = {{NOTE_C3, 100}, {NOTE_E3, 100}, {NOTE_G3, 100}, {NOTE_B3, 150}};
static const int leadUpNotesCount = sizeof(leadUpNotes) / sizeof(ToneDuration);

bool playNextLeadUpNote()
{
    if (leadUpNoteIndex >= leadUpNotesCount) {
        return false; // All notes have been played
    }

    const auto &note = leadUpNotes[leadUpNoteIndex];
    playTones(&note, 1);

    leadUpNoteIndex++;

    return (leadUpNoteIndex < leadUpNotesCount);
}

void resetLeadUpSequence()
{
    leadUpNoteIndex = 0;
}

void playComboTune()
{
    ToneDuration melody[] = {{NOTE_G3, 80}, {NOTE_B3, 60}, {NOTE_CS4, 80}, {NOTE_G3, 60}, {NOTE_CS4, 60}, {NOTE_B3, 120}};
    playMelody(melody, sizeof(melody) / sizeof(ToneDuration), "combo");
}
