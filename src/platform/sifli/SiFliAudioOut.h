#pragma once

#include <stddef.h>
#include <stdint.h>

// PCM playback on the SF32LB52x, over the vendor AUDPRC/AUDCODEC HAL.
//
// Zephyr has no SiFli audio driver and the vendor's own path goes through
// RT-Thread's audio server, so this drives the HAL directly - the same
// approach LGFX_Bus_LCDC takes for the display controller.
//
// The board has no piezo: sound leaves through the SoC's DAC into an NS4150B
// speaker amp, whose enable is a plain GPIO. So every tone, RTTTL melody or
// speech sample has to arrive here as PCM.
//
// UNVERIFIED. Written from LilyGo's pin map and the vendor drv_audprc.c /
// drv_audcodec.c reference; no hardware has run it.

class SiFliAudioOut
{
  public:
    // Called from DMA context to refill one half of the ring. Return the number
    // of frames actually written; a short return ends playback once the buffer
    // drains. Must not block.
    typedef size_t (*FillFn)(int16_t *dst, size_t frames, void *ctx);

    // sampleRate must be one the vendor clock table covers: 8000, 11025, 12000,
    // 16000, 22050, 24000, 32000, 44100 or 48000.
    bool begin(uint32_t sampleRate, uint8_t channels, FillFn fill, void *ctx);
    void end();
    bool isRunning() const { return _running; }

    // Amp enable, separate from playback so callers can mute without tearing
    // the DMA down.
    void setAmpEnabled(bool on);

    // Invoked from the HAL's weak TX callbacks; not part of the public API.
    void refillHalf(int half);

  private:
    bool configureCodec(uint32_t sampleRate);
    bool configureProcessor(uint32_t sampleRate, uint8_t channels);

    volatile bool _running = false;
    bool _draining = false;
    uint8_t _channels = 1;
    FillFn _fill = nullptr;
    void *_ctx = nullptr;
};

extern SiFliAudioOut sifliAudioOut;
