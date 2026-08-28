#pragma once

// The AudioThread this board uses, in place of the ESP8266Audio one in
// src/AudioThread.h. Same three-call contract buzz.cpp drives - beginRttl(),
// isPlaying(), stop() - but the engine underneath is RtttlPcm rendering into
// SiFliAudioOut rather than AudioGeneratorRTTTL into AudioOutputI2S.
//
// There is no piezo on this board, so this is the only way it makes a sound.

#include "audio/RtttlPcm.h"
#include "concurrency/OSThread.h"
#include "configuration.h"
#include "platform/sifli/SiFliAudioOut.h"

class AudioThread : public concurrency::OSThread
{
  public:
    AudioThread() : OSThread("Audio") {}

    // data is the RTTTL string buzz.cpp assembled; it is copied by begin(), so
    // the caller's buffer does not have to outlive the call.
    void beginRttl(const void *data, uint32_t len)
    {
        stop();
        if (!_rtttl.begin((const char *)data, len))
            return;
        // RtttlPcm emits interleaved stereo at its own rate; both are values
        // the codec's clock table covers.
        _playing = sifliAudioOut.begin(RtttlPcm::kSampleRate, 2, fill, this);
        if (!_playing)
            _rtttl.reset();
    }

    // buzz.cpp spins on this. The DMA does the work, so unlike the
    // ESP8266Audio version there is nothing to pump here.
    bool isPlaying() { return _playing && sifliAudioOut.isRunning(); }

    void stop()
    {
        if (_playing) {
            sifliAudioOut.end();
            _playing = false;
        }
        _rtttl.reset();
    }

  protected:
    // Nothing to service between callbacks; the thread exists only so the
    // object has somewhere to live and can be stopped from the main loop.
    int32_t runOnce() override { return INT32_MAX; }

  private:
    // Called from DMA context. A short return tells SiFliAudioOut to drain and
    // stop, which is exactly what the end of a melody should do.
    static size_t fill(int16_t *dst, size_t frames, void *ctx) { return ((AudioThread *)ctx)->_rtttl.generate(dst, frames); }

    RtttlPcm _rtttl;
    volatile bool _playing = false;
};
