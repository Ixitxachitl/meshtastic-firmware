#pragma once

// The SenseCAP Indicator's buzzer hangs off the RP2040 co-processor, not off an ESP32
// pin: whole melodies travel the interdevice link and the co-processor times and
// generates the tones itself, so nothing here has to be ticked while one plays.

#include "audio/RtttlPcm.h"
#include "configuration.h"

#if defined(SENSECAP_INDICATOR)

#include "concurrency/Lock.h"
#include "mesh/IndicatorSerial.h"
#include <stddef.h>
#include <stdint.h>

class IndicatorBuzzer
{
  public:
    /// Play a system melody (buzz.cpp). Returns as soon as the notes are on the link.
    void playTones(const ToneDuration *tones, size_t count);
    /// Hand over a whole RTTTL ringtone. Returns false when the song is malformed or
    /// the link is down; nothing further is needed to keep a started song going.
    bool beginRtttl(const char *song, size_t len);
    /// True while the melody last handed over is still sounding.
    bool isPlaying() const;
    /// Silence the buzzer and drop whatever is still queued on the co-processor.
    void stop();

  private:
    /// Queue depth of the co-processor's player (BEEP_QUEUE_SIZE in indicator_rp2040).
    /// Notes past it are dropped here, where the melody length is still known.
    static constexpr size_t kMaxQueuedNotes = 128;

    // Touch events reach playTones() from the UI task while the main loop is handing
    // over a notification, and the two would otherwise share `frame` mid-melody
    concurrency::Lock lock;

    void beginMelody();
    void addNote(uint16_t frequency, uint16_t duration_ms);
    void endMelody();
    void flush();

    // The frame being filled. A melody longer than one Beep message continues in
    // further frames, appended so the co-processor plays them back to back.
    meshtastic_Note frame[SensecapIndicator::max_notes] = {};
    size_t pending = 0;   // notes in `frame`
    size_t queued = 0;    // notes handed over for this melody
    uint32_t totalMs = 0; // how long this melody runs
    bool appending = false;
    bool failed = false;
    uint32_t playingUntil = 0;
};

extern IndicatorBuzzer indicatorBuzzer;

#endif // SENSECAP_INDICATOR
