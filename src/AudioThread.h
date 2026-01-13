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

// Thread interval when idle (faster when playing to keep DMA fed)
#define AUDIO_THREAD_INTERVAL_MS 50

class AudioThread : public concurrency::OSThread
{
  public:
    AudioThread() : OSThread("Audio")
    {
        initOutput();

#ifdef HAS_FREE_RTOS
        // Configure as FreeRTOS task with appropriate settings for audio processing
        // Higher priority and more stack for real-time audio processing
        const uint32_t audioStackWords = 4096 / sizeof(StackType_t); // 4KB stack
        const UBaseType_t audioPriority = tskIDLE_PRIORITY + 2;      // Higher priority for real-time
        const BaseType_t audioCore = 1;                              // APP CPU (core 1)

        setFreeRTOSTask(true, audioStackWords, audioPriority, audioCore);
#endif
    }
    // Optional: expose how many times runOnce() has executed
    uint32_t pumpTicks() const { return pump_tick_count_; }

    void beginRttl(const void *data, uint32_t len)
    {
        // Safety check: don't play if data is invalid or empty
        if (!data || len == 0) {
            LOG_WARN("AudioThread: beginRttl called with invalid data or zero length");
            return;
        }

        // Stop any existing playback first to prevent pops and memory leaks
        // Use stopPlaybackOnly() to avoid unnecessary amp/CPU toggling
        stopPlaybackOnly();

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

        if (rtttlFile != nullptr) {
            delete rtttlFile;
            rtttlFile = nullptr;
        }

        setCPUFast(false);
#ifdef T_LORA_PAGER
        io.digitalWrite(EXPANDS_AMP_EN, LOW);
#endif
    }

    void readAloud(const char *text)
    {
        // Stop any existing RTTTL playback first
        stopPlaybackOnly();

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
        if (i2sRtttl && i2sRtttl->isRunning()) {
            canSleep = false;
            // Ask buzzer module if we should over-prefill right now.
            bool boost = false;
#ifdef HAS_I2S
            extern bool buzzBoostActive();
            boost = buzzBoostActive();
#endif
            // Prefill more chunks when boosting, a bit less otherwise.
            const int prefill = boost ? 12 : 6; // try 12; 8–14 is fine too
            for (int i = 0; i < prefill; ++i) {
                (void)i2sRtttl->loop();
            }
            // Tick faster while boosting to keep DMA topped up.
            return boost ? 2 : 3;
        }

        canSleep = true;
        return AUDIO_THREAD_INTERVAL_MS;
    }

  private:
    volatile uint32_t pump_tick_count_ = 0;

    // Internal helper to stop playback without affecting amp/CPU state
    // Used when transitioning between tones to prevent pops
    void stopPlaybackOnly()
    {
        if (i2sRtttl != nullptr) {
            i2sRtttl->stop();
            delete i2sRtttl;
            i2sRtttl = nullptr;
        }

        if (rtttlFile != nullptr) {
            delete rtttlFile;
            rtttlFile = nullptr;
        }
    }

    void initOutput()
    {
        audioOut = new AudioOutputI2S(1, AudioOutputI2S::EXTERNAL_I2S);
        audioOut->SetPinout(DAC_I2S_BCK, DAC_I2S_WS, DAC_I2S_DOUT, DAC_I2S_MCLK);
        audioOut->SetGain(0.2);
    }

    AudioGeneratorRTTTL *i2sRtttl = nullptr;
    AudioOutputI2S *audioOut = nullptr;
    AudioFileSourcePROGMEM *rtttlFile = nullptr;
};

#endif
