#include "audio/IndicatorBuzzer.h"

#if defined(SENSECAP_INDICATOR)

#include "concurrency/LockGuard.h"
#include "mesh/Throttle.h"
#include <Arduino.h>

IndicatorBuzzer indicatorBuzzer;

// Clamp to what a Note field carries. NOTE_SILENT (1Hz) and below are rests, the same
// rule the I2S synthesizer applies to a system melody.
static uint16_t noteFrequency(int freqHz)
{
    if (freqHz <= 1)
        return 0;
    return freqHz > UINT16_MAX ? UINT16_MAX : (uint16_t)freqHz;
}

static uint16_t noteDuration(int durationMs)
{
    if (durationMs <= 0)
        return 0;
    return durationMs > UINT16_MAX ? UINT16_MAX : (uint16_t)durationMs;
}

void IndicatorBuzzer::beginMelody()
{
    pending = 0;
    queued = 0;
    totalMs = 0;
    appending = false;
    failed = false;
}

void IndicatorBuzzer::addNote(uint16_t frequency, uint16_t duration_ms)
{
    // A zero duration is inaudible and would stall the co-processor's player
    if (duration_ms == 0 || queued >= kMaxQueuedNotes)
        return;

    frame[pending].frequency = frequency;
    frame[pending].duration_ms = duration_ms;
    pending++;
    queued++;
    totalMs += duration_ms;
    if (pending == SensecapIndicator::max_notes)
        flush();
}

void IndicatorBuzzer::flush()
{
    if (pending == 0)
        return;
    if (!sensecapIndicator || !sensecapIndicator->beep(frame, pending, appending)) {
        // The link refuses everything until the handshake, so name the side that dropped it
        LOG_WARN("Buzzer: co-processor link refused %u notes", (unsigned)pending);
        failed = true;
    }
    appending = true; // whatever follows queues behind the frame just sent
    pending = 0;
}

void IndicatorBuzzer::endMelody()
{
    flush();
    // A melody the link refused never sounds, so it must not hold isPlaying() true
    playingUntil = failed ? 0 : millis() + totalMs;
}

void IndicatorBuzzer::playTones(const ToneDuration *tones, size_t count)
{
    if (!tones || count == 0)
        return;

    concurrency::LockGuard guard(&lock);
    beginMelody();
    for (size_t i = 0; i < count; i++) {
        const uint16_t duration = noteDuration(tones[i].duration_ms);
        addNote(noteFrequency(tones[i].frequency_khz), duration);
        // Articulation the GPIO path gets from its delay(1.3 * duration): without a
        // gap two notes of the same pitch run together as one long one.
        addNote(0, duration * 3 / 10);
    }
    endMelody();
}

bool IndicatorBuzzer::beginRtttl(const char *song, size_t len)
{
    if (!song || len == 0)
        return false;

    concurrency::LockGuard guard(&lock);
    RtttlPcm parser;
    if (!parser.begin(song, len)) {
        LOG_WARN("Ringtone is not valid RTTTL, nothing to play");
        return false;
    }

    beginMelody();
    ToneDuration note;
    // Notes back to back, no added gap: that is how the GPIO RTTTL player sounds, and
    // a ringtone carries its own rests where it wants them
    while (parser.nextNoteEvent(&note))
        addNote(noteFrequency(note.frequency_khz), noteDuration(note.duration_ms));
    endMelody();

    return !failed;
}

bool IndicatorBuzzer::isPlaying() const
{
    return playingUntil != 0 && !Throttle::deadlinePassed(playingUntil);
}

void IndicatorBuzzer::stop()
{
    concurrency::LockGuard guard(&lock);
    playingUntil = 0;
    if (sensecapIndicator)
        sensecapIndicator->beep(NULL, 0, false); // an empty melody drops the queue
}

#endif // SENSECAP_INDICATOR
