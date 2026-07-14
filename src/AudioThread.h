#pragma once
#include "PowerFSM.h"
#include "concurrency/LockGuard.h"
#include "concurrency/OSThread.h"
#include "configuration.h"
#include "main.h"
#include "sleep.h"
#include <memory>

#ifdef HAS_I2S
#include "audio/AudioGeneratorRTTTL3.h"
#include <AudioFileSourcePROGMEM.h>
#include <AudioOutputI2S.h>
#include <ESP8266SAM.h>

// A board with an I2S amplifier opts in by defining AUDIO_AMP_ENABLE(on) in its variant.h to power the
// amp on/off around playback (e.g. an enable pin on an I/O expander). The includes below expose the
// expander instances (io / mcpIoExpander) those macros typically reference.
#ifdef USE_XL9555
#include "ExtensionIOXL9555.hpp"
extern ExtensionIOXL9555 io;
#endif

#ifdef USE_MCP23017
#include "platform/esp32/ExtensionIOMCP23017.h"
#endif

#define AUDIO_THREAD_INTERVAL_MS 50

// Tone entry for direct I2S square-wave playback (bypasses RTTTL octave limits).
struct BuzzerToneEntry {
    int freq_hz; // Hz, 0 = silence
    int dur_ms;  // milliseconds
};

// Lightweight square-wave generator that plays any frequency directly via
// AudioOutputI2S without going through AudioGeneratorRTTTL (which clamps to
// octaves 4-7 and cannot reproduce octave 3 and below).
class BuzzerToneGenerator
{
  public:
    static constexpr int kRate = 22050;

    bool begin(const BuzzerToneEntry *notes, int count, AudioOutput *out)
    {
        list_ = notes;
        count_ = count;
        output_ = out;
        idx_ = 0;
        samplesLeft_ = 0;
        phase_ = 0;
        running_ = (count > 0);
        if (running_)
            loadNote();
        return running_;
    }

    bool loop()
    {
        if (!running_)
            return false;
        while (running_) {
            if (samplesLeft_ == 0) {
                if (++idx_ >= count_) {
                    running_ = false;
                    output_->stop();
                    return false;
                }
                loadNote();
            }
            int16_t val = 0;
            if (wavePeriodFP10_ > 0) {
                int32_t rem = (phase_ << 10) % wavePeriodFP10_;
                val = (rem > wavePeriodFP10_ / 2) ? 8192 : -8192;
            }
            int16_t s[2] = {val, val};
            if (!output_->ConsumeSample(s))
                return true; // DMA full, resume next call
            phase_++;
            samplesLeft_--;
        }
        return false;
    }

    bool stop()
    {
        running_ = false;
        return true;
    }
    bool isRunning() const { return running_; }

  private:
    void loadNote()
    {
        int freq = list_[idx_].freq_hz;
        samplesLeft_ = ((int32_t)list_[idx_].dur_ms * kRate) / 1000;
        phase_ = 0;
        wavePeriodFP10_ = (freq > 0) ? ((kRate << 10) / freq) : 0;
    }

    const BuzzerToneEntry *list_ = nullptr;
    int count_ = 0;
    AudioOutput *output_ = nullptr;
    int idx_ = 0;
    int32_t samplesLeft_ = 0;
    int32_t phase_ = 0;
    int32_t wavePeriodFP10_ = 0;
    bool running_ = false;
};

class AudioThread : public concurrency::OSThread
{
  public:
    AudioThread() : OSThread("Audio")
    {
        initOutput();

#ifdef HAS_FREE_RTOS
        const uint32_t audioStackWords = 4096 / sizeof(StackType_t);
        const UBaseType_t audioPriority = tskIDLE_PRIORITY + 2;
        const BaseType_t audioCore = 1;
        setFreeRTOSTask(true, audioStackWords, audioPriority, audioCore);
#endif
    }

    uint32_t pumpTicks() const { return pump_tick_count_; }

    void beginRttl(const void *data, uint32_t len)
    {
        concurrency::LockGuard lock(&audioMutex);
        stopPlaybackOnly();
#ifdef AUDIO_AMP_ENABLE
        AUDIO_AMP_ENABLE(true);
#endif
        setCPUFast(true);
        rtttlFile = std::unique_ptr<AudioFileSourcePROGMEM>(new AudioFileSourcePROGMEM(data, len));
        i2sRtttl = std::unique_ptr<AudioGeneratorRTTTL3>(new AudioGeneratorRTTTL3());
        if (!i2sRtttl->begin(rtttlFile.get(), audioOut.get())) {
            i2sRtttl = nullptr;
            rtttlFile = nullptr;
            setCPUFast(false);
            return;
        }
    }

    // Also handles actually playing the RTTTL, needs to be called in loop
    bool isPlaying()
    {
        concurrency::LockGuard lock(&audioMutex);
        if (i2sRtttl != nullptr) {
            return i2sRtttl->isRunning() && i2sRtttl->loop();
        }
        return false;
    }

    void stop()
    {
        concurrency::LockGuard lock(&audioMutex);
        stopPlaybackOnly();
        setCPUFast(false);
#ifdef AUDIO_AMP_ENABLE
        AUDIO_AMP_ENABLE(false);
#endif
    }

    void readAloud(const char *text)
    {
        if (i2sRtttl != nullptr) {
            i2sRtttl->stop();
            i2sRtttl = nullptr;
        }

#ifdef AUDIO_AMP_ENABLE
        AUDIO_AMP_ENABLE(true);
#endif
        auto sam = std::unique_ptr<ESP8266SAM>(new ESP8266SAM);
        sam->Say(audioOut.get(), text);
        setCPUFast(false);
#ifdef AUDIO_AMP_ENABLE
        AUDIO_AMP_ENABLE(false);
#endif
    }

  protected:
    int32_t runOnce() override
    {
        concurrency::LockGuard lock(&audioMutex);
        if (i2sRtttl && i2sRtttl->isRunning()) {
            canSleep = false;
#ifdef HAS_I2S
            extern bool buzzBoostActive();
            const bool boost = buzzBoostActive();
#else
            const bool boost = false;
#endif
            // During boost (first 800 ms of a new tone) pump 12 frames at 2 ms
            // to pre-fill the DMA; otherwise 6 frames at 3 ms.
            const int prefill = boost ? 12 : 6;
            for (int i = 0; i < prefill; ++i) {
                if (!i2sRtttl->loop())
                    break;
            }
            return boost ? 2 : 3;
        }
        pump_tick_count_++;
        canSleep = true;
        return AUDIO_THREAD_INTERVAL_MS;
    }

  private:
    volatile uint32_t pump_tick_count_ = 0;
    void stopPlaybackOnly()
    {
        if (i2sRtttl) {
            i2sRtttl->stop();
            i2sRtttl = nullptr;
        }
        rtttlFile = nullptr;
    }

    void initOutput()
    {
        audioOut = std::unique_ptr<AudioOutputI2S>(new AudioOutputI2S(1, AudioOutputI2S::EXTERNAL_I2S, 4));
        audioOut->SetPinout(DAC_I2S_BCK, DAC_I2S_WS, DAC_I2S_DOUT, DAC_I2S_MCLK);
        audioOut->SetGain(0.2);
    };

    std::unique_ptr<AudioGeneratorRTTTL3> i2sRtttl = nullptr;
    std::unique_ptr<AudioOutputI2S> audioOut = nullptr;
    std::unique_ptr<AudioFileSourcePROGMEM> rtttlFile = nullptr;
    concurrency::Lock audioMutex;
};

#endif
