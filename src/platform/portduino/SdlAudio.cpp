#include "SdlAudio.h"

#if defined(USE_SDL_AUDIO)

#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#elif __has_include(<SDL.h>)
#include <SDL.h>
#endif

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace portduino_audio
{
namespace
{
// Fixed at SAM's native output rate (see ESP8266SAM::Say(), which hardcodes SetRate(22050))
// rather than a conventional 44.1kHz, so speech can be queued straight to the device with
// no resampling. Square-wave tones/RTTTL are synthesized in software at whatever rate the
// device runs, so sharing this rate with speech costs them nothing audible.
constexpr int kSampleRate = 22050;
constexpr int16_t kAmplitude = 6000; // well below full scale; raw square waves are harsh
constexpr int kFadeSamples = 96;     // ~2ms linear attack/release to soften clicks

SDL_AudioDeviceID g_dev = 0;
bool g_initFailed = false;
SDL_mutex *g_mutex = nullptr;

bool ensureInit()
{
    if (g_dev)
        return true;
    if (g_initFailed)
        return false;

    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            g_initFailed = true;
            return false;
        }
    }

    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = kSampleRate;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    want.callback = nullptr; // push mode via SDL_QueueAudio

    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
    if (g_dev == 0) {
        g_initFailed = true;
        return false;
    }
    if (!g_mutex)
        g_mutex = SDL_CreateMutex();
    SDL_PauseAudioDevice(g_dev, 0); // start playback
    return true;
}

// Render a square wave (or silence) and queue it for playback.
void queueTone(int freqHz, int durationMs)
{
    if (durationMs <= 0)
        return;
    int count = (int)((int64_t)kSampleRate * durationMs / 1000);
    if (count <= 0)
        return;

    std::vector<int16_t> buf(count);
    if (freqHz <= 1) {
        std::memset(buf.data(), 0, (size_t)count * sizeof(int16_t)); // rest
    } else {
        int period = kSampleRate / freqHz;
        if (period < 2)
            period = 2;
        for (int i = 0; i < count; ++i) {
            int v = ((i % period) < period / 2) ? kAmplitude : -kAmplitude;
            if (i < kFadeSamples)
                v = v * i / kFadeSamples;
            else if (i > count - kFadeSamples)
                v = v * (count - i) / kFadeSamples;
            buf[(size_t)i] = (int16_t)v;
        }
    }
    SDL_QueueAudio(g_dev, buf.data(), (Uint32)((size_t)count * sizeof(int16_t)));
}

// Frequency of a note (index 0..11 within an octave starting at C; <0 => rest).
// octave 4 A = 440 Hz, matching the firmware's rtttl library convention.
int noteFreq(int noteIdx, int octave)
{
    if (noteIdx < 0)
        return 0;
    int midi = octave * 12 + noteIdx + 12; // C4 -> 60, A4 -> 69
    double f = 440.0 * std::pow(2.0, (midi - 69) / 12.0);
    return (int)(f + 0.5);
}
} // namespace

void tone(uint16_t freqHz, uint16_t durationMs)
{
    if (!ensureInit())
        return;
    SDL_LockMutex(g_mutex);
    queueTone(freqHz, durationMs);
    SDL_UnlockMutex(g_mutex);
}

void queuePcm(const int16_t *samples, size_t count)
{
    if (!samples || !count || !ensureInit())
        return;
    SDL_LockMutex(g_mutex);
    SDL_QueueAudio(g_dev, samples, (Uint32)(count * sizeof(int16_t)));
    SDL_UnlockMutex(g_mutex);
}

void beginRtttl(const char *song)
{
    if (!song || !ensureInit())
        return;

    std::string s(song);
    size_t p = s.find(':'); // skip the tune name
    if (p == std::string::npos)
        return;
    ++p;

    int defaultDur = 4;
    int defaultOct = 6;
    int bpm = 63;

    size_t ctrlEnd = s.find(':', p);
    if (ctrlEnd == std::string::npos)
        return;
    {
        std::string ctrl = s.substr(p, ctrlEnd - p);
        size_t i = 0;
        while (i < ctrl.size()) {
            while (i < ctrl.size() && (ctrl[i] == ' ' || ctrl[i] == ','))
                ++i;
            if (i + 1 < ctrl.size() && ctrl[i + 1] == '=') {
                char key = (char)std::tolower((unsigned char)ctrl[i]);
                int val = std::atoi(ctrl.c_str() + i + 2);
                if (key == 'd' && val > 0)
                    defaultDur = val;
                else if (key == 'o' && val > 0)
                    defaultOct = val;
                else if (key == 'b' && val > 0)
                    bpm = val;
            }
            while (i < ctrl.size() && ctrl[i] != ',')
                ++i;
        }
    }
    p = ctrlEnd + 1;

    const double wholeMs = (60000.0 / bpm) * 4.0; // whole note = 4 beats

    SDL_LockMutex(g_mutex);
    while (p < s.size()) {
        while (p < s.size() && (s[p] == ' ' || s[p] == ','))
            ++p;
        if (p >= s.size())
            break;

        int dur = 0;
        while (p < s.size() && std::isdigit((unsigned char)s[p]))
            dur = dur * 10 + (s[p++] - '0');
        if (dur == 0)
            dur = defaultDur;

        int noteIdx = -1;
        if (p < s.size()) {
            switch (std::tolower((unsigned char)s[p])) {
            case 'c': noteIdx = 0; break;
            case 'd': noteIdx = 2; break;
            case 'e': noteIdx = 4; break;
            case 'f': noteIdx = 5; break;
            case 'g': noteIdx = 7; break;
            case 'a': noteIdx = 9; break;
            case 'b': noteIdx = 11; break;
            default: noteIdx = -1; break; // 'p' pause or unknown
            }
            ++p;
        }
        if (p < s.size() && s[p] == '#') {
            if (noteIdx >= 0)
                ++noteIdx;
            ++p;
        }
        bool dotted = false;
        if (p < s.size() && s[p] == '.') {
            dotted = true;
            ++p;
        }
        int oct = defaultOct;
        if (p < s.size() && std::isdigit((unsigned char)s[p]))
            oct = s[p++] - '0';
        if (p < s.size() && s[p] == '.') {
            dotted = true;
            ++p;
        }

        double noteMs = wholeMs / dur;
        if (dotted)
            noteMs *= 1.5;
        queueTone(noteFreq(noteIdx, oct), (int)(noteMs + 0.5));
    }
    SDL_UnlockMutex(g_mutex);
}

bool isPlaying()
{
    if (!g_dev)
        return false;
    return SDL_GetQueuedAudioSize(g_dev) > 0;
}

void stop()
{
    if (g_dev)
        SDL_ClearQueuedAudio(g_dev);
}

} // namespace portduino_audio

#endif // USE_SDL_AUDIO
