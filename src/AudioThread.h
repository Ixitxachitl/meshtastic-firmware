#pragma once
#include "PowerFSM.h"
#include "concurrency/LockGuard.h"
#include "concurrency/OSThread.h"
#include "configuration.h"
#include "main.h"
#include "sleep.h"
#include <memory>

#ifdef HAS_I2S
#include <AudioFileSourcePROGMEM.h>
#include <AudioGeneratorRTTTL.h>
#include <AudioOutputI2S.h>
#include <ESP8266SAM.h>

#ifdef USE_XL9555
#include "ExtensionIOXL9555.hpp"
extern ExtensionIOXL9555 io;
#endif

#define AUDIO_THREAD_INTERVAL_MS 50

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
#ifdef T_LORA_PAGER
        io.digitalWrite(EXPANDS_AMP_EN, HIGH);
#endif
        setCPUFast(true);
        rtttlFile = std::unique_ptr<AudioFileSourcePROGMEM>(new AudioFileSourcePROGMEM(data, len));
        i2sRtttl = std::unique_ptr<AudioGeneratorRTTTL>(new AudioGeneratorRTTTL());
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
#ifdef T_LORA_PAGER
        io.digitalWrite(EXPANDS_AMP_EN, LOW);
#endif
    }

    void readAloud(const char *text)
    {
        if (i2sRtttl != nullptr) {
            i2sRtttl->stop();
            i2sRtttl = nullptr;
        }

#ifdef T_LORA_PAGER
        io.digitalWrite(EXPANDS_AMP_EN, HIGH);
#endif
        auto sam = std::unique_ptr<ESP8266SAM>(new ESP8266SAM);
        sam->Say(audioOut.get(), text);
        setCPUFast(false);
#ifdef T_LORA_PAGER
        io.digitalWrite(EXPANDS_AMP_EN, LOW);
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

    std::unique_ptr<AudioGeneratorRTTTL> i2sRtttl = nullptr;
    std::unique_ptr<AudioOutputI2S> audioOut = nullptr;
    std::unique_ptr<AudioFileSourcePROGMEM> rtttlFile = nullptr;
    concurrency::Lock audioMutex;
};

#endif
