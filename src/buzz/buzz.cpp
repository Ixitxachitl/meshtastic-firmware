#include "buzz.h"
#include "NodeDB.h"
#include "configuration.h"

#if !defined(ARCH_ESP32) && !defined(ARCH_RP2040) && !defined(ARCH_PORTDUINO)
#include "Tone.h"
#endif

#if defined(HAS_I2S)
#include "main.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#if defined(ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif
#endif

#if !defined(ARCH_PORTDUINO)
extern "C" void delay(uint32_t dwMs);
#endif

struct ToneDuration {
    int frequency_khz;
    int duration_ms;
};

// Note frequencies (scientific pitch notation)
#define NOTE_SILENT 0
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
#define NOTE_C4 262
#define NOTE_CS4 277
#define NOTE_D4 294
#define NOTE_DS4 311
#define NOTE_E4 330
#define NOTE_F4 349
#define NOTE_FS4 370
#define NOTE_G4 392
#define NOTE_GS4 415
#define NOTE_A4 440
#define NOTE_AS4 466
#define NOTE_B4 494
#define NOTE_C5 523
#define NOTE_CS5 554
#define NOTE_D5 587
#define NOTE_DS5 622
#define NOTE_E5 659
#define NOTE_F5 698
#define NOTE_FS5 740
#define NOTE_G5 784
#define NOTE_GS5 831
#define NOTE_A5 880
#define NOTE_AS5 932
#define NOTE_B5 988
#define NOTE_C6 1047
#define NOTE_CS6 1109
#define NOTE_D6 1175
#define NOTE_DS6 1245
#define NOTE_E6 1319
#define NOTE_F6 1397
#define NOTE_FS6 1480
#define NOTE_G6 1568
#define NOTE_GS6 1661
#define NOTE_A6 1760
#define NOTE_AS6 1865
#define NOTE_B6 1976
#define NOTE_C7 2093
#define NOTE_CS7 2217
#define NOTE_D7 2349
#define NOTE_DS7 2489
#define NOTE_E7 2637
#define NOTE_F7 2794
#define NOTE_FS7 2960
#define NOTE_G7 3136

const int DURATION_1_16 = 62;  // 1/16 note
const int DURATION_1_8 = 125;  // 1/8 note
const int DURATION_1_4 = 250;  // 1/4 note
const int DURATION_1_2 = 500;  // 1/2 note
const int DURATION_3_4 = 750;  // 3/4 note
const int DURATION_1_1 = 1000; // 1/1 note

#ifdef HAS_I2S
// Full chromatic note map — octave stored separately so tonesToRtttl can shift
// octaves into the 4-7 range that AudioGeneratorRTTTL supports.
struct NoteToken {
    int freq;
    const char *note;
    int octave;
};
static const NoteToken kNoteMap[] = {
    {NOTE_C3, "c", 3},   {NOTE_CS3, "c#", 3}, {NOTE_D3, "d", 3},   {NOTE_DS3, "d#", 3}, {NOTE_E3, "e", 3},   {NOTE_F3, "f", 3},
    {NOTE_FS3, "f#", 3}, {NOTE_G3, "g", 3},   {NOTE_GS3, "g#", 3}, {NOTE_A3, "a", 3},   {NOTE_AS3, "a#", 3}, {NOTE_B3, "b", 3},
    {NOTE_C4, "c", 4},   {NOTE_CS4, "c#", 4}, {NOTE_D4, "d", 4},   {NOTE_DS4, "d#", 4}, {NOTE_E4, "e", 4},   {NOTE_F4, "f", 4},
    {NOTE_FS4, "f#", 4}, {NOTE_G4, "g", 4},   {NOTE_GS4, "g#", 4}, {NOTE_A4, "a", 4},   {NOTE_AS4, "a#", 4}, {NOTE_B4, "b", 4},
    {NOTE_C5, "c", 5},   {NOTE_CS5, "c#", 5}, {NOTE_D5, "d", 5},   {NOTE_DS5, "d#", 5}, {NOTE_E5, "e", 5},   {NOTE_F5, "f", 5},
    {NOTE_FS5, "f#", 5}, {NOTE_G5, "g", 5},   {NOTE_GS5, "g#", 5}, {NOTE_A5, "a", 5},   {NOTE_AS5, "a#", 5}, {NOTE_B5, "b", 5},
    {NOTE_C6, "c", 6},   {NOTE_CS6, "c#", 6}, {NOTE_D6, "d", 6},   {NOTE_DS6, "d#", 6}, {NOTE_E6, "e", 6},   {NOTE_F6, "f", 6},
    {NOTE_FS6, "f#", 6}, {NOTE_G6, "g", 6},   {NOTE_GS6, "g#", 6}, {NOTE_A6, "a", 6},   {NOTE_AS6, "a#", 6}, {NOTE_B6, "b", 6},
    {NOTE_C7, "c", 7},   {NOTE_CS7, "c#", 7}, {NOTE_D7, "d", 7},   {NOTE_DS7, "d#", 7}, {NOTE_E7, "e", 7},   {NOTE_F7, "f", 7},
    {NOTE_FS7, "f#", 7}, {NOTE_G7, "g", 7},
};

static inline int freqToNoteIndex(int freq)
{
    if (freq <= 0)
        return -1; // rest
    int bestIdx = -1, bestErr = 1000000;
    for (int i = 0; i < (int)(sizeof(kNoteMap) / sizeof(kNoteMap[0])); ++i) {
        int err = (kNoteMap[i].freq > freq) ? (kNoteMap[i].freq - freq) : (freq - kNoteMap[i].freq);
        if (err < bestErr) {
            bestErr = err;
            bestIdx = i;
        }
    }
    return bestIdx;
}

struct DurCand {
    int denom;
    bool dotted;
    int ms;
};
static const DurCand kDur[] = {
    {1, false, 1000}, {2, true, 750},  {2, false, 500}, {4, true, 375},  {4, false, 250},
    {8, true, 187},   {8, false, 125}, {16, true, 94},  {16, false, 62}, {32, false, 31},
};

static inline void chooseDur(int ms, int &denom, bool &dotted)
{
    int bestIdx = 0, bestErr = 0x7fffffff;
    for (int i = 0; i < (int)(sizeof(kDur) / sizeof(kDur[0])); ++i) {
        int err = (kDur[i].ms > ms) ? (kDur[i].ms - ms) : (ms - kDur[i].ms);
        if (err < bestErr) {
            bestErr = err;
            bestIdx = i;
        }
    }
    denom = kDur[bestIdx].denom;
    dotted = kDur[bestIdx].dotted;
}

// Build a full RTTTL string from a ToneDuration array.
// Normalizes octaves into the 4-7 range that AudioGeneratorRTTTL supports.
static size_t tonesToRtttl(char *out, size_t cap, const ToneDuration *td, int n, const char *name = "sys")
{
    if (!out || cap == 0 || !td || n <= 0)
        return 0;
    out[0] = '\0';

    // Find lowest octave in the melody, then shift everything up so it starts at 4.
    int minOctave = 10;
    for (int i = 0; i < n; ++i) {
        int idx = freqToNoteIndex(td[i].frequency_khz);
        if (idx >= 0 && kNoteMap[idx].octave < minOctave)
            minOctave = kNoteMap[idx].octave;
    }
    const int octaveShift = (minOctave < 10) ? (4 - minOctave) : 0;
    const int defaultOctave = 4;

    int wrote = snprintf(out, cap, "%s:d=4,o=%d,b=240:", name, defaultOctave);
    if (wrote <= 0 || (size_t)wrote >= cap)
        return (size_t)std::max(0, wrote);
    size_t pos = (size_t)wrote;

    int currentOctave = defaultOctave;
    for (int i = 0; i < n; ++i) {
        int denom;
        bool dotted;
        chooseDur(td[i].duration_ms, denom, dotted);

        int noteIdx = freqToNoteIndex(td[i].frequency_khz);
        char token[16];

        if (noteIdx < 0) {
            snprintf(token, sizeof(token), dotted ? "%dp." : "%dp", denom);
        } else {
            const char *noteName = kNoteMap[noteIdx].note;
            int noteOctave = kNoteMap[noteIdx].octave + octaveShift;
            if (noteOctave != currentOctave) {
                snprintf(token, sizeof(token), dotted ? "%d%s%d." : "%d%s%d", denom, noteName, noteOctave);
                currentOctave = noteOctave;
            } else {
                snprintf(token, sizeof(token), dotted ? "%d%s." : "%d%s", denom, noteName);
            }
        }

        const char *sep = (i + 1 < n) ? ((td[i].duration_ms < 80) ? ",32p," : ",") : "";
        size_t need = strlen(token) + strlen(sep);
        if (pos + need >= cap)
            break;
        memcpy(out + pos, token, strlen(token));
        pos += strlen(token);
        if (*sep) {
            memcpy(out + pos, sep, strlen(sep));
            pos += strlen(sep);
        }
        out[pos] = '\0';
    }
    return pos;
}

void ensureAudioPumpTaskStarted() {}

#if defined(HAS_I2S) && defined(ESP32)
static volatile uint32_t g_audioBoostUntilMs = 0;

void buzzBoostFor(uint32_t ms)
{
    g_audioBoostUntilMs = millis() + ms;
}

bool buzzBoostActive()
{
    return (int32_t)(millis() - g_audioBoostUntilMs) < 0;
}

// Queued RTTTL: used when audioThread isn't ready yet at boot
static char g_pendingRttl[384];
static volatile bool g_hasPending = false;

static inline void queueRttl(const char *rttl)
{
    std::strncpy(g_pendingRttl, rttl, sizeof(g_pendingRttl) - 1);
    g_pendingRttl[sizeof(g_pendingRttl) - 1] = '\0';
    g_hasPending = true;
}

void buzzOnAudioThreadReady()
{
    if (!audioThread || !g_hasPending)
        return;
    if (!audioThread->isPlaying()) {
        buzzBoostFor(800);
        audioThread->beginRttl(g_pendingRttl, std::strlen(g_pendingRttl));
        g_hasPending = false;
        // Prime DMA buffer for smooth playback
        for (int i = 0; i < 6; ++i) {
            (void)audioThread->isPlaying();
            delay(0);
        }
    }
}

#else
void buzzBoostFor(uint32_t) {}
bool buzzBoostActive()
{
    return false;
}
void buzzOnAudioThreadReady() {}
#endif

static inline bool i2sBuzzerEnabled()
{
    return moduleConfig.external_notification.use_i2s_as_buzzer;
}

static void startRttlI2S(const char *rttl)
{
    if (!rttl || !*rttl)
        return;
#if defined(HAS_I2S) && defined(ESP32)
    if (!audioThread) {
        queueRttl(rttl);
        return;
    }
#endif
    if (!audioThread)
        return;
    buzzBoostFor(800);
    audioThread->beginRttl(rttl, strlen(rttl));
}
#endif // HAS_I2S

void playTones(const ToneDuration *tone_durations, int size)
{
    if (config.device.buzzer_mode == meshtastic_Config_DeviceConfig_BuzzerMode_DISABLED ||
        config.device.buzzer_mode == meshtastic_Config_DeviceConfig_BuzzerMode_NOTIFICATIONS_ONLY) {
        // Buzzer is disabled or not set to system tones
        return;
    }
#ifdef HAS_I2S
    if (i2sBuzzerEnabled()) {
        // Static so AudioFileSourcePROGMEM's raw pointer stays valid after we return.
        static char rttl[384];
        tonesToRtttl(rttl, sizeof(rttl), tone_durations, size);
        startRttlI2S(rttl);
        return;
    }
#endif
#if defined(PIN_BUZZER)
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
    ToneDuration melody[] = {{NOTE_B3, DURATION_1_16}};
    playTones(melody, sizeof(melody) / sizeof(ToneDuration));
}

void playLongBeep()
{
    ToneDuration melody[] = {{NOTE_B3, DURATION_1_1}};
    playTones(melody, sizeof(melody) / sizeof(ToneDuration));
}

void playGPSEnableBeep()
{
#if defined(R1_NEO) || defined(MUZI_BASE)
    ToneDuration melody[] = {
        {NOTE_F5, DURATION_1_2}, {NOTE_G6, DURATION_1_8}, {NOTE_E7, DURATION_1_4}, {NOTE_SILENT, DURATION_1_2}};
#else
    ToneDuration melody[] = {{NOTE_C3, DURATION_1_8}, {NOTE_FS3, DURATION_1_4}, {NOTE_CS4, DURATION_1_4}};
#endif
    playTones(melody, sizeof(melody) / sizeof(ToneDuration));
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
    ToneDuration melody[] = {{NOTE_AS3, 20}}; // Short AS3 note
    playTones(melody, sizeof(melody) / sizeof(ToneDuration));
}

void playClick()
{
    // A very short "click" sound with minimum delay; ideal for rotary encoder events
    ToneDuration melody[] = {{NOTE_AS3, 1}}; // Very Short AS3
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

void play4ClickDown()
{
    ToneDuration melody[] = {{NOTE_G5, 55}, {NOTE_E5, 55}, {NOTE_C5, 60},  {NOTE_A4, 55},  {NOTE_G4, 55},
                             {NOTE_E4, 65}, {NOTE_C4, 80}, {NOTE_G3, 120}, {NOTE_E3, 160}, {NOTE_SILENT, 120}};
    playTones(melody, sizeof(melody) / sizeof(ToneDuration));
}

void play4ClickUp()
{
    // Quick high-pitched notes with trills
    ToneDuration melody[] = {{NOTE_F5, 50}, {NOTE_G6, 45}, {NOTE_E7, 60}};
    playTones(melody, sizeof(melody) / sizeof(ToneDuration));
}