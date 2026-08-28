#include "SamPcm.h"

#if defined(HAS_I2S) && defined(MESHTASTIC_ENABLE_TTS)

#include "audio/sam/ESP8266SAM.h"
#include <Arduino.h>
#include <string.h>

/// Bridges SAM's push output into the ring. SAM emits 8-bit unsigned mono; the widening
/// to signed 16-bit stereo happens in generate(), so the ring stays one byte per frame.
class SamRingSink : public SamAudioOut
{
  public:
    explicit SamRingSink(SamPcm *owner) : _owner(owner) {}

    bool begin() override { return true; }
    bool SetRate(int) override { return true; }     // fixed at SamPcm::kSampleRate
    bool SetChannels(int) override { return true; } // mono, widened on the way out

    bool ConsumeSample(int16_t sample[2]) override
    {
        // ESP8266SAM::OutputByte() has already converted the byte to signed 16-bit, so
        // undo that rather than carry four bytes per frame through the ring.
        uint8_t b = (uint8_t)(((sample[0] >> 8) + 128) & 0xff);
        return _owner->pushSample(b);
    }

  private:
    SamPcm *_owner;
};

size_t SamPcm::ringCount() const
{
    size_t head = _head.load(std::memory_order_acquire);
    size_t tail = _tail.load(std::memory_order_relaxed);
    return (head + kRingFrames - tail) % kRingFrames;
}

#if defined(__ZEPHYR__)
#define SAM_SLEEP_1MS() k_msleep(1)
// One utterance at a time, so one static stack; see begin().
K_THREAD_STACK_DEFINE(samRenderStack, SamPcm::kRenderStackBytes);
#else
#define SAM_SLEEP_1MS() vTaskDelay(pdMS_TO_TICKS(1))
#endif

bool SamPcm::pushSample(uint8_t sample)
{
    // Never refuse a sample. ESP8266SAM::OutputByte() ignores the return value and spins
    //   while (!output->ConsumeSample(sample)) yield();
    // so returning false wedges the render task inside Say() forever: it never sets
    // _renderDone, reset() eventually gives up on it, and the next utterance starts a
    // second renderer writing into the same ring. Once stopping we are discarding the
    // utterance anyway, so swallow the rest and let Say() run out quickly.
    if (_stop.load(std::memory_order_relaxed))
        return true;

    size_t head = _head.load(std::memory_order_relaxed);
    size_t next = (head + 1) % kRingFrames;

    // Otherwise wait for the consumer rather than dropping: a dropped sample is an
    // audible click, and there is no way to ask SAM to re-emit one.
    while (next == _tail.load(std::memory_order_acquire)) {
        if (_stop.load(std::memory_order_relaxed))
            return true; // same reasoning as above
        SAM_SLEEP_1MS();
    }

    _ring[head] = sample;
    _head.store(next, std::memory_order_release);
    return true;
}

void SamPcm::renderLoop()
{
    {
        SamRingSink sink(this);
        ESP8266SAM sam;
        sam.Say(&sink, _text); // returns once the whole utterance is rendered, or on stop
    }
    _renderDone.store(true, std::memory_order_release);
#if !defined(__ZEPHYR__)
    // Zephyr reaps the thread when the entry point returns; FreeRTOS does not.
    vTaskDelete(nullptr);
#endif
}

void SamPcm::renderEntry(void *self)
{
    static_cast<SamPcm *>(self)->renderLoop();
}

#if defined(__ZEPHYR__)
void SamPcm::renderEntryZephyr(void *self, void *, void *)
{
    renderEntry(self);
}
#endif

bool SamPcm::begin(const char *text)
{
    reset();

    if (!text || !text[0])
        return false;

    if (_task) {
        // reset() could not join the previous renderer. Starting another would give two
        // tasks the same ring; refuse instead, and let the next attempt try again.
        LOG_ERROR("Speech: previous renderer still running, skipping");
        return false;
    }

    // SAM refuses anything longer than one page; truncate rather than say nothing.
    strncpy(_text, text, sizeof(_text) - 1);
    _text[sizeof(_text) - 1] = '\0';
    if (strlen(_text) > 254)
        _text[254] = '\0';

    _head.store(0, std::memory_order_relaxed);
    _tail.store(0, std::memory_order_relaxed);
    _stop.store(false, std::memory_order_relaxed);
    _renderDone.store(false, std::memory_order_relaxed);
    _done = false;

#if defined(__ZEPHYR__)
    // The stack is static: there is only ever one utterance in flight, and a
    // 4KB allocation at speech time is exactly what a low-memory board cannot
    // afford. k_thread_create() cannot fail the way xTaskCreate can.
    k_thread_create(&_thread, samRenderStack, K_THREAD_STACK_SIZEOF(samRenderStack), renderEntryZephyr, this, nullptr, nullptr,
                    K_PRIO_PREEMPT(kTaskPriority), 0, K_NO_WAIT);
    k_thread_name_set(&_thread, "SamRender");
    _taskRunning = true;
#else
    if (xTaskCreatePinnedToCore(renderEntry, "SamRender", kTaskStackBytes, this, kTaskPriority, &_task, kTaskCore) != pdPASS) {
        _task = nullptr;
        _renderDone.store(true, std::memory_order_relaxed);
        _done = true;
        LOG_WARN("Speech: no RAM for render task");
        return false;
    }
#endif
    return true;
}

size_t SamPcm::generate(int16_t *interleavedLR, size_t maxFrames)
{
    if (!interleavedLR || _done)
        return 0;

    uint32_t deadline = millis() + kStarveTimeoutMs;
    size_t n = 0;

    while (n < maxFrames) {
        size_t avail = ringCount();
        if (!avail) {
            // Nothing buffered. If the renderer has finished, so have we; otherwise give
            // it a moment - returning 0 early would be read as end-of-utterance.
            if (_renderDone.load(std::memory_order_acquire)) {
                if (!ringCount())
                    break;
                continue;
            }
            if (millis() > deadline) {
                LOG_WARN("Speech: renderer starved, ending utterance");
                break;
            }
            if (n)
                break; // hand over what we have rather than stalling the feeder
            SAM_SLEEP_1MS();
            continue;
        }

        size_t tail = _tail.load(std::memory_order_relaxed);
        size_t take = avail < (maxFrames - n) ? avail : (maxFrames - n);
        for (size_t i = 0; i < take; i++) {
            // 8-bit unsigned to signed 16-bit, duplicated to both channels.
            int32_t s = (int32_t)((((int16_t)_ring[tail]) - 128) << 8);
            if (kGainQ8 != 256) {
                s = (s * kGainQ8) >> 8;
                // Saturate: SAM is already near full scale, so any gain above unity
                // would otherwise wrap and turn loud passages into noise.
                if (s > 32767)
                    s = 32767;
                else if (s < -32768)
                    s = -32768;
            }
            int16_t v = (int16_t)s;
            interleavedLR[2 * (n + i)] = v;
            interleavedLR[2 * (n + i) + 1] = v;
            tail = (tail + 1) % kRingFrames;
        }
        _tail.store(tail, std::memory_order_release);
        n += take;
    }

    if (!n && _renderDone.load(std::memory_order_acquire) && !ringCount())
        _done = true;

    return n;
}

void SamPcm::reset()
{
    if (_task) {
        _stop.store(true, std::memory_order_relaxed);
        // Unblock a producer waiting on a full ring, then let it unwind. It self-deletes.
        for (int i = 0; i < 500 && !_renderDone.load(std::memory_order_acquire); i++) {
            _tail.store(_head.load(std::memory_order_acquire), std::memory_order_release);
            SAM_SLEEP_1MS();
        }
        if (_renderDone.load(std::memory_order_acquire)) {
            _task = nullptr;
        } else {
            // Keep the handle so begin() can refuse rather than stacking renderers.
            LOG_ERROR("Speech: render task did not exit");
        }
    }
    _head.store(0, std::memory_order_relaxed);
    _tail.store(0, std::memory_order_relaxed);
    _stop.store(false, std::memory_order_relaxed);
    _renderDone.store(true, std::memory_order_relaxed);
    _done = true;
}

#endif // HAS_I2S && MESHTASTIC_ENABLE_TTS
