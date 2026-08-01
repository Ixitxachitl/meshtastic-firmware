#pragma once
#include "PowerFSM.h"
#include "concurrency/LockGuard.h"
#include "concurrency/OSThread.h"
#include "configuration.h"
#include "main.h"
#include "sleep.h"
#include <memory>

#ifdef HAS_I2S
#include "audio/AudioGeneratorRTTTL.h"
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

// Idle poll interval. Nothing is playing, so this only decides how quickly a
// melody queued from another thread starts.
#define AUDIO_THREAD_INTERVAL_MS 100

/**
 * Plays RTTTL melodies out of an I2S DAC.
 *
 * On ESP32 this runs as its own FreeRTOS task (see setFreeRTOSTask() in the
 * constructor) so that a slow main loop cannot starve the I2S DMA. If the task
 * cannot be created, the object stays registered with the cooperative
 * ThreadController and works the same way, just with main-loop timing.
 *
 * Every public method is safe to call from any thread; the mutex serialises
 * them against the task's own pumping in runOnce().
 */
class AudioThread : public concurrency::OSThread
{
  public:
    AudioThread() : OSThread("Audio")
    {
        initOutput();

#if defined(ARDUINO_ARCH_ESP32)
        setFreeRTOSTask(true, kTaskStackBytes, kTaskPriority, kTaskCore);
#endif
    }

    /// Start playing an RTTTL string, replacing anything already playing.
    void beginRttl(const void *data, uint32_t len)
    {
        concurrency::LockGuard lock(&audioMutex);
        stopPlaybackOnly();
        acquireHardware();

        rtttlFile = std::unique_ptr<AudioFileSourcePROGMEM>(new AudioFileSourcePROGMEM(data, len));
        std::unique_ptr<meshtastic::AudioGeneratorRTTTL> generator(new meshtastic::AudioGeneratorRTTTL());
        if (!generator->begin(rtttlFile.get(), audioOut.get())) {
            LOG_WARN("Audio: could not parse RTTTL, not playing");
            rtttlFile = nullptr;
            releaseHardware();
            return;
        }
        i2sRtttl = std::move(generator);
#if defined(ARDUINO_ARCH_ESP32)
        // Start pumping now instead of up to AUDIO_THREAD_INTERVAL_MS from now,
        // so a key click is not audibly late.
        wakeFreeRTOSTask();
#endif
    }

    /// True while a melody is still playing. This is a pure query - the audio
    /// task does the pumping, so callers must not poll it to drive playback.
    bool isPlaying()
    {
        concurrency::LockGuard lock(&audioMutex);
        return i2sRtttl && i2sRtttl->isRunning();
    }

    void stop()
    {
        concurrency::LockGuard lock(&audioMutex);
        stopPlaybackOnly();
        releaseHardware();
    }

    void readAloud(const char *text)
    {
        concurrency::LockGuard lock(&audioMutex);
        stopPlaybackOnly();
        acquireHardware();

        auto sam = std::unique_ptr<ESP8266SAM>(new ESP8266SAM);
        sam->Say(audioOut.get(), text);

        releaseHardware();
    }

  protected:
    int32_t runOnce() override
    {
        concurrency::LockGuard lock(&audioMutex);

        if (i2sRtttl) {
            // One loop() call fills the DMA chain completely, so there is no
            // point pumping several times per wakeup.
            if (i2sRtttl->isRunning() && i2sRtttl->loop()) {
                canSleep = false; // only consulted on the ThreadController fallback path
                return kPumpIntervalMs;
            }
            // The melody ended on its own. Nothing else releases the amp and the
            // CPU boost on the playTones() path - ExternalNotificationModule is
            // the only caller of stop(), and it only runs when that module is
            // enabled - so a system beep would otherwise leave both on forever.
            stopPlaybackOnly();
            idleSinceMs = millis();
        }

        if (hardwareActive) {
            // Linger before powering the amp down, so that a nag repeat or a
            // burst of key clicks does not pop it off and on between melodies.
            if ((millis() - idleSinceMs) < kHardwareLingerMs) {
                canSleep = false;
                return AUDIO_THREAD_INTERVAL_MS;
            }
            releaseHardware();
        }

        canSleep = true;
        return AUDIO_THREAD_INTERVAL_MS;
    }

  private:
#if defined(ARDUINO_ARCH_ESP32)
    // ESP-IDF's xTaskCreate() takes the stack size in bytes, not words.
    static constexpr uint32_t kTaskStackBytes = 4096;
    static constexpr UBaseType_t kTaskPriority = tskIDLE_PRIORITY + 2;
    static constexpr BaseType_t kTaskCore = 1;
#endif

    // ESP8266Audio's AudioOutputI2S uses a fixed 128-frame DMA buffer, and we ask
    // for kDmaBufCount of them below. Poll at a fraction of the time the hardware
    // needs to drain that chain, which is where the margin against underrun comes
    // from - no pre-fill or boost heuristics required.
    static constexpr int kDmaBufCount = 4;
    static constexpr int kDmaBufFrames = 128;
    static constexpr int kSampleRate = 22050; // must match AudioGeneratorRTTTL's rate
    static constexpr int32_t kDmaDrainMs = (1000 * kDmaBufCount * kDmaBufFrames) / kSampleRate;
    static constexpr int32_t kPumpIntervalMs = kDmaDrainMs / 3;
    static_assert(kPumpIntervalMs >= 1, "pump interval rounded down to zero");

    /// How long the amp stays powered after a melody ends, before we shut it down.
    static constexpr uint32_t kHardwareLingerMs = 300;

    /// Tear down the generator without touching the amp, so that replacing one
    /// melody with another does not click the amplifier off and back on.
    void stopPlaybackOnly()
    {
        if (i2sRtttl) {
            i2sRtttl->stop();
            i2sRtttl = nullptr;
        }
        rtttlFile = nullptr;
    }

    // AUDIO_AMP_ENABLE typically writes to an I2C IO expander, and runOnce()
    // reaches these from the audio task rather than the main loop. That is safe
    // for the variants that define it today - ExtensionIOMCP23017::digitalWrite
    // locks internally, and tlora-pager only touches its XL9555 during setup -
    // but a new variant sharing an unlocked expander with the main loop would
    // need its own guard.
    void acquireHardware()
    {
        if (hardwareActive)
            return;
        hardwareActive = true;
#ifdef AUDIO_AMP_ENABLE
        AUDIO_AMP_ENABLE(true);
#endif
        setCPUFast(true);
    }

    void releaseHardware()
    {
        if (!hardwareActive)
            return;
        hardwareActive = false;
        setCPUFast(false);
#ifdef AUDIO_AMP_ENABLE
        AUDIO_AMP_ENABLE(false);
#endif
    }

    void initOutput()
    {
        audioOut = std::unique_ptr<AudioOutputI2S>(new AudioOutputI2S(1, AudioOutputI2S::EXTERNAL_I2S, kDmaBufCount));
        audioOut->SetPinout(DAC_I2S_BCK, DAC_I2S_WS, DAC_I2S_DOUT, DAC_I2S_MCLK);
        audioOut->SetGain(0.2);
    };

    std::unique_ptr<meshtastic::AudioGeneratorRTTTL> i2sRtttl = nullptr;
    std::unique_ptr<AudioOutputI2S> audioOut = nullptr;
    std::unique_ptr<AudioFileSourcePROGMEM> rtttlFile = nullptr;
    bool hardwareActive = false;
    uint32_t idleSinceMs = 0;
    concurrency::Lock audioMutex;
};

#endif
