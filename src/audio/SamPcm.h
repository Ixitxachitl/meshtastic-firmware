#pragma once

#include "configuration.h"

#if defined(HAS_I2S) && defined(MESHTASTIC_ENABLE_TTS)

#include <atomic>
#if defined(__ZEPHYR__)
#include <zephyr/kernel.h>
#else
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif
#include <stddef.h>
#include <stdint.h>

/// Digital gain on speech, in the same spirit as AUDIO_RTTTL_AMPLITUDE. Defaults to
/// unity - SAM's 8-bit output already widens to nearly full scale, so there is little
/// headroom - but boards whose amplifier gain is fixed in hardware have no other knob.
/// Applied with saturation, so overdriving clips rather than wrapping.
#ifndef AUDIO_SPEECH_GAIN
#define AUDIO_SPEECH_GAIN 1.0f
#endif

/**
 * Pull-style PCM source for SAM speech, shaped like RtttlPcm so AudioThread can feed it
 * through the same path as a melody.
 *
 * SAM is a push synthesiser: SAMMain() renders an entire utterance in one call and emits
 * every sample through a callback, with no resumption point. Rendering the whole thing up
 * front is not an option either - a 254 character utterance is a few hundred KB of PCM,
 * and the boards without PSRAM (Cardputer Adv among them) have nowhere to put it.
 *
 * So the render runs on its own task and pushes into a small ring, and generate() drains
 * that ring. The task blocks while the ring is full, which caps the memory at kRingFrames
 * regardless of utterance length, and SAM renders far faster than real time so the ring
 * stays ahead of the feeder.
 *
 * Only one instance may render at a time: SAM keeps its synthesis state in file-scope
 * globals.
 */
class SamPcm
{
  public:
    /// SAM's fixed output rate. Matches RtttlPcm::kSampleRate, so the I2S channel does not
    /// need reconfiguring between a melody and speech.
    static constexpr uint32_t kSampleRate = 22050;

    ~SamPcm() { reset(); }

    /// Start rendering `text`. Returns false if the render task could not be started, in
    /// which case nothing is queued and done() stays true.
    bool begin(const char *text);

    /// Drain up to maxFrames interleaved L/R frames. Returns 0 only once the utterance is
    /// finished - if the ring is momentarily empty this waits briefly for the renderer
    /// rather than reporting completion early.
    size_t generate(int16_t *interleavedLR, size_t maxFrames);

    bool done() const { return _done; }

    /// Abandon any utterance in progress and join the render task.
    void reset();

  private:
    /// ~186ms of audio. Comfortably longer than the feeder's 5ms poll, so the renderer can
    /// be descheduled without starving playback, and small enough to sit in DRAM.
    static constexpr size_t kRingFrames = 4096;

    /// AUDIO_SPEECH_GAIN as Q8 fixed point, so the per-sample path stays integer.
    static constexpr int32_t kGainQ8 = (int32_t)(AUDIO_SPEECH_GAIN * 256.0f);

    /// How long generate() waits for the renderer before treating an empty ring as the end
    /// of the utterance. Only reached if the render task is starved for this long.
    static constexpr uint32_t kStarveTimeoutMs = 250;

    static constexpr uint32_t kTaskStackBytes = 4096;

  public:
    static constexpr uint32_t kRenderStackBytes = kTaskStackBytes;

  private:
    /// Below the feeder (2) so rendering never delays a DMA refill, above idle so it still
    /// runs while the main loop is busy.
    // Plain int: both k_thread_create and xTaskCreate take it, and the
    // FreeRTOS typedefs do not exist on the Zephyr path.
    static constexpr int kTaskPriority = 1;
#if !defined(__ZEPHYR__)
    static constexpr BaseType_t kTaskCore = 1;
#endif

    static void renderEntry(void *self);
#if defined(__ZEPHYR__)
    // Zephyr entry points take three arguments; only the first is used.
    static void renderEntryZephyr(void *self, void *, void *);
#endif
    void renderLoop();

    /// Called by the render task for every sample SAM produces. Blocks while the ring is
    /// full; returns false if reset() asked us to stop, which unwinds the render.
    bool pushSample(uint8_t sample);
    friend class SamRingSink;

    size_t ringCount() const;

    // Single producer (render task), single consumer (feeder task): the indices are the
    // only shared state and each is written by exactly one side.
    uint8_t _ring[kRingFrames] = {};
    std::atomic<size_t> _head{0}; // next write, producer
    std::atomic<size_t> _tail{0}; // next read, consumer

    char _text[256] = {0};
#if defined(__ZEPHYR__)
    // Zephyr wants the stack and the thread object supplied by the caller,
    // where FreeRTOS allocates both itself.
    struct k_thread _thread;
    bool _taskRunning = false;
#else
    TaskHandle_t _task = nullptr;
#endif
    std::atomic<bool> _renderDone{true};
    std::atomic<bool> _stop{false};
    bool _done = true;
};

#endif // HAS_I2S && MESHTASTIC_ENABLE_TTS
