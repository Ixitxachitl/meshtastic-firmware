#include "LGFX_Bus_LCDC.hpp"

#include <string.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control/sf32lb.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>

#define LCDC_NODE DT_NODELABEL(lcdc)
#define LCDC_PIN_NODE DT_NODELABEL(lcdc_dbi)

// The LCDC node stays disabled in the board DTS so Zephyr's mipi_dbi driver
// does not claim the controller, but its clock and pin state are still
// described there and applied from here.
PINCTRL_DT_DEFINE(LCDC_PIN_NODE);
static const struct pinctrl_dev_config *lcdc_pincfg = PINCTRL_DT_DEV_CONFIG_GET(LCDC_PIN_NODE);
static const struct sf32lb_clock_dt_spec lcdc_clock = SF32LB_CLOCK_DT_SPEC_GET(LCDC_NODE);

namespace lgfx
{
inline namespace v1
{

bool Bus_SF32LB_LCDC::init()
{
    if (_initialized)
        return true;

    if (!sf32lb_clock_is_ready_dt(&lcdc_clock))
        return false;
    if (sf32lb_clock_control_on_dt(&lcdc_clock) < 0)
        return false;
    if (pinctrl_apply_state(lcdc_pincfg, PINCTRL_STATE_DEFAULT) < 0)
        return false;

    memset(&_lcdc, 0, sizeof(_lcdc));
    _lcdc.Instance = hwp_lcdc1;
    _lcdc.Init.lcd_itf = LCDC_INTF_SPI_NODCX_4DATA;
    _lcdc.Init.freq = _cfg.freq_write;
    _lcdc.Init.color_mode = LCDC_PIXEL_FORMAT_RGB565;

    auto &spi = _lcdc.Init.cfg.spi;
    spi.dummy_clock = 0;
    spi.syn_mode = _cfg.use_te ? HAL_LCDC_SYNC_VER : HAL_LCDC_SYNC_DISABLE;
    spi.cs_polarity = 0;  // CS active low
    spi.clk_polarity = 0; // CPOL 0
    spi.clk_phase = 0;    // CPHA 0
    spi.vsyn_polarity = 1;
    spi.vsyn_delay_us = 0;
    spi.hsyn_num = 0;
    spi.bytes_gap_us = 0;
    spi.readback_from_Dx = 0;

    if (HAL_LCDC_Init(&_lcdc) != HAL_OK)
        return false;

    HAL_LCDC_SetROIArea(&_lcdc, 0, 0, _cfg.panel_width - 1, _cfg.panel_height - 1);

    _initialized = true;
    return true;
}

void Bus_SF32LB_LCDC::release()
{
    if (!_initialized)
        return;
    HAL_LCDC_DeInit(&_lcdc);
    sf32lb_clock_control_off_dt(&lcdc_clock);
    _initialized = false;
}

void Bus_SF32LB_LCDC::beginTransaction()
{
    _addr_len = 0;
    _stage_len = 0;
}

void Bus_SF32LB_LCDC::endTransaction()
{
    flush();
}

void Bus_SF32LB_LCDC::wait()
{
    flush();
}

bool Bus_SF32LB_LCDC::busy() const
{
    return false; // every transfer below is synchronous
}

void Bus_SF32LB_LCDC::setClock(uint32_t freq)
{
    if (freq == _cfg.freq_write)
        return;
    _cfg.freq_write = freq;
    if (_initialized)
        HAL_LCDC_SetFreq(&_lcdc, freq);
}

void Bus_SF32LB_LCDC::flush()
{
    if (_addr_len == 0)
        return;

    HAL_LCDC_WriteDatas(&_lcdc, _addr, _addr_len, _stage_len ? _stage : nullptr, _stage_len);
    _addr_len = 0;
    _stage_len = 0;
}

void Bus_SF32LB_LCDC::stage(const uint8_t *data, size_t length)
{
    // Overflowing the staging buffer would silently drop panel parameters, so
    // send what is held and start a fresh frame with the same address.
    if (_stage_len + length > STAGE_SIZE) {
        flush();
        return;
    }
    memcpy(_stage + _stage_len, data, length);
    _stage_len += length;
}

bool Bus_SF32LB_LCDC::writeCommand(uint32_t data, uint_fast8_t bit_length)
{
    flush();
    _addr = data;
    _addr_len = bit_length >> 3;
    return true;
}

void Bus_SF32LB_LCDC::writeData(uint32_t data, uint_fast8_t bit_length)
{
    stage(reinterpret_cast<const uint8_t *>(&data), bit_length >> 3);
}

void Bus_SF32LB_LCDC::writeDataRepeat(uint32_t data, uint_fast8_t bit_length, uint32_t count)
{
    const size_t bytes = bit_length >> 3;
    uint8_t buf[STAGE_SIZE];
    const size_t per_pass = STAGE_SIZE / bytes;

    while (count) {
        const size_t n = count < per_pass ? count : per_pass;
        for (size_t i = 0; i < n; i++) {
            memcpy(buf + i * bytes, &data, bytes);
        }
        writeBytes(buf, n * bytes, true, false);
        count -= n;
    }
}

void Bus_SF32LB_LCDC::writePixels(pixelcopy_t *pc, uint32_t length)
{
    const size_t bytes = pc->dst_bits >> 3;
    uint8_t buf[STAGE_SIZE];
    const size_t per_pass = STAGE_SIZE / bytes;

    while (length) {
        const size_t n = length < per_pass ? length : per_pass;
        pc->fp_copy(buf, 0, n, pc);
        writeBytes(buf, n * bytes, true, false);
        length -= n;
    }
}

void Bus_SF32LB_LCDC::writeBytes(const uint8_t *data, uint32_t length, bool dc, bool use_dma)
{
    (void)dc;
    (void)use_dma;

    // Anything already staged belongs in front of this payload, and the HAL
    // takes address and payload in one call, so short writes join the stage
    // and longer ones go out with whatever address is pending.
    if (_stage_len + length <= STAGE_SIZE) {
        stage(data, length);
        return;
    }

    flush();
    HAL_LCDC_WriteDatas(&_lcdc, _addr, _addr_len, const_cast<uint8_t *>(data), length);
}

uint32_t Bus_SF32LB_LCDC::readData(uint_fast8_t bit_length)
{
    uint32_t data = 0;
    readBytes(reinterpret_cast<uint8_t *>(&data), bit_length >> 3, false);
    return data;
}

bool Bus_SF32LB_LCDC::readBytes(uint8_t *dst, uint32_t length, bool use_dma)
{
    (void)use_dma;
    return HAL_LCDC_ReadDatas(&_lcdc, _addr, _addr_len, dst, length) == HAL_OK;
}

void Bus_SF32LB_LCDC::readPixels(void *dst, pixelcopy_t *pc, uint32_t length)
{
    const size_t bytes = pc->src_bits >> 3;
    uint8_t buf[STAGE_SIZE];
    const size_t per_pass = STAGE_SIZE / bytes;
    int32_t dstindex = 0;

    while (length) {
        const size_t n = length < per_pass ? length : per_pass;
        if (!readBytes(buf, n * bytes, false))
            return;
        pc->src_data = buf;
        pc->src_x = 0;
        dstindex = pc->fp_copy(dst, dstindex, dstindex + n, pc);
        length -= n;
    }
}

} // namespace v1
} // namespace lgfx
