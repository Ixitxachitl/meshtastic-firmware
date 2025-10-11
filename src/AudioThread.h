#pragma once
#include "PowerFSM.h"
#include "concurrency/OSThread.h"
#include "configuration.h"
#include "main.h"
#include "sleep.h"

#ifdef HAS_I2S
#include <AudioFileSourcePROGMEM.h>
#include <AudioGeneratorRTTTL.h>
#include <AudioOutputI2S.h>
#include <ESP8266SAM.h>

#ifdef USE_XL9555
#include "ExtensionIOXL9555.hpp"
extern ExtensionIOXL9555 io;
#endif

#define AUDIO_THREAD_INTERVAL_MS 100

class AudioThread : public concurrency::OSThread
{
  public:
    AudioThread() : OSThread("Audio") { initOutput(); }
    // Optional: expose how many times runOnce() has executed
    uint32_t pumpTicks() const { return pump_tick_count_; }

    void beginRttl(const void *data, uint32_t len)
    {
#ifdef T_LORA_PAGER
        io.digitalWrite(EXPANDS_AMP_EN, HIGH);
#endif
        setCPUFast(true);
        rtttlFile = new AudioFileSourcePROGMEM(data, len);
        i2sRtttl = new AudioGeneratorRTTTL();
        i2sRtttl->begin(rtttlFile, audioOut);
    }

    // Also handles actually playing the RTTTL, needs to be called in loop
    bool isPlaying()
    {
        if (i2sRtttl != nullptr) {
            return i2sRtttl->isRunning() && i2sRtttl->loop();
        }
        return false;
    }

    void stop()
    {
        if (i2sRtttl != nullptr) {
            i2sRtttl->stop();
            delete i2sRtttl;
            i2sRtttl = nullptr;
        }
        delete rtttlFile;
        rtttlFile = nullptr;

        setCPUFast(false);
#ifdef T_LORA_PAGER
        io.digitalWrite(EXPANDS_AMP_EN, LOW);
#endif
    }

    void readAloud(const char *text)
    {
        if (i2sRtttl != nullptr) {
            i2sRtttl->stop();
            delete i2sRtttl;
            i2sRtttl = nullptr;
        }

#ifdef T_LORA_PAGER
        io.digitalWrite(EXPANDS_AMP_EN, HIGH);
#endif
        ESP8266SAM *sam = new ESP8266SAM;
        sam->Say(audioOut, text);
        delete sam;
        setCPUFast(false);
#ifdef T_LORA_PAGER
        io.digitalWrite(EXPANDS_AMP_EN, LOW);
#endif
    }

  protected:
    int32_t runOnce() override
    {
     ++pump_tick_count_;
    canSleep = true;                  // by default we allow sleep
    if (i2sRtttl != nullptr && i2sRtttl->isRunning()) {
        canSleep = false;             // keep the board awake while playing
        // Prefill several chunks to survive brief scheduler gaps
        for (int i = 0; i < 6; ++i) {    // 3–5 is a good range; start with 4
            (void)i2sRtttl->loop();
        }
        return 3;  // keep cadence tight while playing
    }
    return AUDIO_THREAD_INTERVAL_MS;  // e.g. 100ms when idle
    }

  private:
    volatile uint32_t pump_tick_count_ = 0;
    
    void initOutput()
    {
        audioOut = new AudioOutputI2S(1, AudioOutputI2S::EXTERNAL_I2S);
        audioOut->SetPinout(DAC_I2S_BCK, DAC_I2S_WS, DAC_I2S_DOUT, DAC_I2S_MCLK);
        audioOut->SetGain(0.2);
    };

    AudioGeneratorRTTTL *i2sRtttl = nullptr;
    AudioOutputI2S *audioOut;

    AudioFileSourcePROGMEM *rtttlFile;
};

#endif
