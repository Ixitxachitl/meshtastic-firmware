#include "SiFliAudioOut.h"
#include "Arduino.h"
#include "configuration.h"
#include "variant.h"

#include "bf0_hal.h"
#include "bf0_hal_audcodec.h"
#include "bf0_hal_audprc.h"
#include <zephyr/dt-bindings/dma/sf32lb52x-dma.h>

SiFliAudioOut sifliAudioOut;

// The HAL's debug hook. The vendor's own definition is an empty __WEAK stub
// behind USE_HAL_DRIVER, which this build does not set - so rather than
// vendoring bf0_hal_hlp.c and a global define just to get a no-op, supply the
// no-op. Only HAL_AUDCODEC_Config_DACPath_Volume calls it.
extern "C" void HAL_DBG_printf(const char *fmt, ...)
{
    (void)fmt;
}

namespace
{
// One ring, refilled a half at a time from the HAL's half/full TX callbacks.
// 512 frames a half is ~10ms at 48kHz, short enough that a late refill is a
// click rather than a gap, long enough not to interrupt-storm the scheduler.
constexpr size_t kFramesPerHalf = 512;
constexpr size_t kMaxChannels = 2;

// The HAL's DMA reads straight out of this, so it has to be aligned and must
// not live in a cached region.
int16_t __aligned(4) txRing[kFramesPerHalf * 2 * kMaxChannels];

AUDPRC_HandleTypeDef haudprc;
AUDCODEC_HandleTypeDef haudcodec;
DMA_HandleTypeDef htxdma;

// Vendor clock table (drv_audprc.c): xtal-48M divider per sample rate, with the
// 44.1k family forced onto the PLL because 48MHz will not divide to it.
struct ClockEntry {
    uint32_t rate;
    uint8_t clkSel; // 0: xtal 48M, 1: PLL 44.1M
    uint16_t div;
};
constexpr ClockEntry kClocks[] = {
    {48000, 0, 1000}, {32000, 0, 1500}, {24000, 0, 2000}, {16000, 0, 3000}, {12000, 0, 4000},
    {8000, 0, 6000},  {44100, 1, 1000}, {22050, 1, 2000}, {11025, 1, 4000},
};

// The codec needs its own clock descriptor, not just an index: verbatim from
// drv_audcodec.c's codec_dac_clk_config_xtal, in the same rate order as
// kClocks above. The 44.1k family runs off the PLL because 48MHz will not
// divide to it, which is why those three rows differ in the source columns.
const AUDCODE_DAC_CLK_CONFIG_TYPE kCodecDacClocks[] = {
    {48000, 0, 10, 0, 0x14D, 0, 5, 4, 2, 20, 20},  {32000, 0, 10, 1, 0x14D, 0, 5, 4, 2, 20, 20},
    {24000, 0, 20, 0, 0x14D, 0, 10, 2, 2, 10, 10}, {16000, 0, 10, 2, 0x14D, 0, 5, 4, 2, 20, 20},
    {12000, 0, 40, 0, 0x14D, 0, 20, 2, 1, 5, 5},   {8000, 0, 20, 2, 0x14D, 0, 10, 2, 2, 10, 10},
    {44100, 1, 10, 0, 0x14D, 1, 5, 4, 2, 20, 20},  {22050, 1, 20, 0, 0x14D, 1, 10, 2, 2, 10, 10},
    {11025, 1, 40, 0, 0x14D, 1, 20, 2, 1, 5, 5},
};

const ClockEntry *lookupClock(uint32_t rate)
{
    for (const auto &e : kClocks)
        if (e.rate == rate)
            return &e;
    return nullptr;
}
} // namespace

// ── HAL weak callback overrides ─────────────────────────────────────────────
// The HAL calls these from DMA context, once per half of the ring.

extern "C" void HAL_AUDPRC_TxHalfCpltCallback(AUDPRC_HandleTypeDef *haprc, int cid)
{
    (void)haprc;
    if (cid == HAL_AUDPRC_TX_CH0)
        sifliAudioOut.refillHalf(0);
}

extern "C" void HAL_AUDPRC_TxCpltCallback(AUDPRC_HandleTypeDef *haprc, int cid)
{
    (void)haprc;
    if (cid == HAL_AUDPRC_TX_CH0)
        sifliAudioOut.refillHalf(1);
}

// ── SiFliAudioOut ───────────────────────────────────────────────────────────

void SiFliAudioOut::setAmpEnabled(bool on)
{
#ifdef PIN_SPK_CTRL
    pinMode(PIN_SPK_CTRL, OUTPUT);
    digitalWrite(PIN_SPK_CTRL, on ? HIGH : LOW);
#else
    (void)on;
#endif
}

void SiFliAudioOut::refillHalf(int half)
{
    if (!_running)
        return;

    int16_t *dst = txRing + (size_t)half * kFramesPerHalf * _channels;
    const size_t wanted = kFramesPerHalf;
    size_t got = 0;

    if (!_draining && _fill)
        got = _fill(dst, wanted, _ctx);

    if (got < wanted) {
        // Source is finished. Zero the rest so the amp goes quiet instead of
        // replaying stale samples, and stop once this half has been clocked out.
        memset(dst + got * _channels, 0, (wanted - got) * _channels * sizeof(int16_t));
        if (_draining) {
            _running = false;
            HAL_AUDPRC_DMAStop(&haudprc, HAL_AUDPRC_TX_CH0);
            setAmpEnabled(false);
            return;
        }
        _draining = true;
    }
}

bool SiFliAudioOut::configureCodec(uint32_t sampleRate)
{
    const ClockEntry *clk = lookupClock(sampleRate);
    if (!clk)
        return false;

    memset(&haudcodec, 0, sizeof(haudcodec));
    haudcodec.Instance = hwp_audcodec;
    haudcodec.Init.en_dly_sel = 0;
    // The index and the descriptor have to agree; both tables are in the same
    // rate order, so one lookup serves for both.
    const size_t idx = (size_t)(clk - kClocks);
    haudcodec.Init.samplerate_index = (uint8_t)idx;
    haudcodec.Init.dac_cfg.dac_clk = (AUDCODE_DAC_CLK_CONFIG_TYPE *)&kCodecDacClocks[idx];

    if (HAL_AUDCODEC_Init(&haudcodec) != HAL_OK)
        return false;

    // bypass=1: DAC path straight through, no codec-side resampling. AUDPRC has
    // already delivered samples at the rate the codec is clocked for.
    HAL_AUDCODEC_Config_DACPath(&haudcodec, 1);
    HAL_AUDCODEC_Config_DACPath_Volume(&haudcodec, 0, AUDIO_DAC_VOLUME);
    HAL_AUDCODEC_Config_DACPath_Volume(&haudcodec, 1, AUDIO_DAC_VOLUME);
    return true;
}

bool SiFliAudioOut::configureProcessor(uint32_t sampleRate, uint8_t channels)
{
    const ClockEntry *clk = lookupClock(sampleRate);
    if (!clk)
        return false;

    memset(&haudprc, 0, sizeof(haudprc));
    haudprc.Instance = hwp_audprc;
    haudprc.Init.clk_sel = clk->clkSel;
    haudprc.Init.clk_div = clk->div;
    haudprc.Init.dac_div = clk->div;
    haudprc.Init.adc_div = clk->div;

    // DAC path mux/mixer selects, from drv_audprc.c's bf0_adc_dac_path_cfg_init.
    auto &dac = haudprc.Init.dac_cfg;
    dac.dst_sel = 0;
    dac.mixrsrc1 = 5;
    dac.mixrsrc0 = 1;
    dac.mixlsrc1 = 5;
    dac.mixlsrc0 = 0;
    dac.muxrsrc1 = 5;
    dac.muxrsrc0 = 1;
    dac.muxlsrc1 = 5;
    dac.muxlsrc0 = 0;
    dac.eq_stage = 1;
    dac.vol_l = 0;
    dac.vol_r = 0;

    // The HAL fills in the rest of the DMA config; it needs the channel and the
    // peripheral request line.
    memset(&htxdma, 0, sizeof(htxdma));
    // Not DMA1_Channel1: sifli_soc_aliases.h retires the SoC header's short
    // aliases, and that macro expands through DMA1. This is what it expands to.
    //
    // Channel 1 by hand, because the vendor HAL allocates nothing - the BSP
    // hands it a channel from board config. Channel 0 is spoken for by mpi2 in
    // the board DTS; nothing else in this port claims one, but Zephyr's own DMA
    // driver is not consulted, so a future dma-using node could collide here.
    htxdma.Instance = (DMA_Channel_TypeDef *)&hwp_dmac1->CCR1;
    htxdma.Init.Request = SF32LB52X_DMA_REQ_AUDPRC_TX_CH0;
    haudprc.hdma[HAL_AUDPRC_TX_CH0] = &htxdma;

    HAL_RCC_EnableModule(RCC_MOD_AUDPRC);
    if (HAL_AUDPRC_Init(&haudprc) != HAL_OK)
        return false;

    HAL_AUDPRC_Config_DACPath(&haudprc, &haudprc.Init.dac_cfg);

    AUDPRC_ChnlCfgTypeDef cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.en = 1;
    cfg.format = 0;                     // 16-bit
    cfg.mode = (channels == 1) ? 0 : 1; // stereo only exists at 16-bit
    HAL_AUDPRC_Config_TChanel(&haudprc, HAL_AUDPRC_TX_CH0, &cfg);
    return true;
}

bool SiFliAudioOut::begin(uint32_t sampleRate, uint8_t channels, FillFn fill, void *ctx)
{
    if (_running)
        end();
    if (!fill || channels < 1 || channels > kMaxChannels)
        return false;
    if (!lookupClock(sampleRate)) {
        LOG_ERROR("Audio: %u Hz is not in the codec's clock table", (unsigned)sampleRate);
        return false;
    }

    _channels = channels;
    _fill = fill;
    _ctx = ctx;
    _draining = false;

    if (!configureCodec(sampleRate) || !configureProcessor(sampleRate, channels)) {
        LOG_ERROR("Audio: codec bring-up failed");
        return false;
    }

    // Prime both halves before the DMA starts, so the first thing clocked out is
    // real audio rather than whatever the ring held.
    _running = true;
    refillHalf(0);
    refillHalf(1);

    setAmpEnabled(true);

    const uint32_t bytes = kFramesPerHalf * 2 * channels * sizeof(int16_t);
    if (HAL_AUDPRC_Transmit_DMA(&haudprc, (uint8_t *)txRing, bytes, HAL_AUDPRC_TX_CH0) != HAL_OK) {
        _running = false;
        setAmpEnabled(false);
        LOG_ERROR("Audio: DMA start failed");
        return false;
    }
    return true;
}

void SiFliAudioOut::end()
{
    if (!_running)
        return;
    _running = false;
    HAL_AUDPRC_DMAStop(&haudprc, HAL_AUDPRC_TX_CH0);
    setAmpEnabled(false);
}
