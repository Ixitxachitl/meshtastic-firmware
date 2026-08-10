#pragma once
// SDL-backed audio for the portduino native builds. The real buzzer path (Arduino
// tone()/noTone() driving config.device.buzzer_gpio, and the rtttl PWM player) produces
// no sound on a PC, so these helpers synthesize square-wave tones straight to the host
// audio device instead. Enabled with -D USE_SDL_AUDIO on the SDL-linked native env(s).
//
// All functions are thread-safe and lazily open the audio device on first use, so callers
// don't have to worry about init ordering. Playback is non-blocking: tones are queued and
// play back FIFO in the background.
#include <cstddef>
#include <cstdint>

#if defined(USE_SDL_AUDIO)
namespace portduino_audio
{
// Queue one square-wave tone. freqHz <= 1 is rendered as silence (RTTTL/melody rests).
void tone(uint16_t freqHz, uint16_t durationMs);

// Parse an RTTTL ("Nokia ringtone") string and queue all of its notes. Uses the same
// octave convention as the firmware's rtttl library (octave 4 A = 440 Hz).
void beginRtttl(const char *song);

// Queue raw mono 16-bit PCM already at kSampleRate (e.g. rendered SAM speech). Unlike
// tone()/beginRtttl(), the caller does its own framing/fades - this just appends to the
// device's play queue.
void queuePcm(const int16_t *samples, size_t count);

// True while there is still queued audio to play.
bool isPlaying();

// Drop any queued/playing audio (e.g. when a notification is dismissed).
void stop();
} // namespace portduino_audio
#endif
