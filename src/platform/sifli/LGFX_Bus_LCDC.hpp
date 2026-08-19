#pragma once

// LovyanGFX bus that drives a QSPI panel through the SF32LB LCDC.
//
// The CO5300 on this board is wired CS/CLK/D0-D3, which the controller calls
// LCDC_INTF_SPI_NODCX_4DATA: one command frame carrying an 8- or 32-bit
// address, then data on all four lines. Zephyr's mipi_dbi_sf32lb driver only
// implements 8080 and 3/4-wire SPI, and the MIPI-DBI API has no quad mode to
// express this through, so the transfers go straight to the vendor HAL.
//
// LovyanGFX splits a transfer into writeCommand() followed by writeData() /
// writeBytes(), while HAL_LCDC_WriteDatas() takes address and payload
// together. Commands are therefore staged until the payload arrives or the
// transaction ends.

#include <lgfx/v1/Bus.hpp>
#include <lgfx/v1/misc/pixelcopy.hpp>
#include <lgfx/v1/platforms/common.hpp> // FlipBuffer

extern "C" {
#include "bf0_hal.h"
#include "bf0_hal_lcdc.h"
}

namespace lgfx
{
inline namespace v1
{

class Bus_SF32LB_LCDC : public IBus
{
  public:
    struct config_t {
        uint32_t freq_write = 24000000;
        uint32_t freq_read = 4000000;
        uint16_t panel_width = 480;
        uint16_t panel_height = 480;
        // TE (tearing effect) input, wired on this board but only used once
        // the panel is driven from a framebuffer.
        bool use_te = false;
    };

    const config_t &config() const { return _cfg; }
    void config(const config_t &cfg) { _cfg = cfg; }

    bus_type_t busType() const override { return bus_type_t::bus_spi; }

    bool init() override;
    void release() override;

    void beginTransaction() override;
    void endTransaction() override;
    void wait() override;
    bool busy() const override;
    void flush() override;

    uint32_t getClock() const override { return _cfg.freq_write; }
    uint32_t getReadClock() const override { return _cfg.freq_read; }
    void setClock(uint32_t freq) override;
    void setReadClock(uint32_t freq) override { _cfg.freq_read = freq; }

    bool writeCommand(uint32_t data, uint_fast8_t bit_length) override;
    void writeData(uint32_t data, uint_fast8_t bit_length) override;
    void writeDataRepeat(uint32_t data, uint_fast8_t bit_length, uint32_t count) override;
    void writePixels(pixelcopy_t *pc, uint32_t length) override;
    void writeBytes(const uint8_t *data, uint32_t length, bool dc, bool use_dma) override;

    void initDMA() override {}
    void addDMAQueue(const uint8_t *data, uint32_t length) override { writeBytes(data, length, true, false); }
    void execDMAQueue() override { flush(); }
    uint8_t *getDMABuffer(uint32_t length) override { return _flip_buffer.getBuffer(length); }

    void beginRead() override {}
    void endRead() override {}
    uint32_t readData(uint_fast8_t bit_length) override;
    bool readBytes(uint8_t *dst, uint32_t length, bool use_dma) override;
    void readPixels(void *dst, pixelcopy_t *pc, uint32_t length) override;

  private:
    // Staging for the pending command frame. 32 bytes covers every parameter
    // list the panel sends inline; pixel payloads bypass it via writeBytes().
    static constexpr size_t STAGE_SIZE = 32;

    void stage(const uint8_t *data, size_t length);

    config_t _cfg;
    LCDC_HandleTypeDef _lcdc = {};
    FlipBuffer _flip_buffer;
    uint32_t _addr = 0;
    uint8_t _addr_len = 0;
    uint8_t _stage[STAGE_SIZE] = {};
    size_t _stage_len = 0;
    bool _initialized = false;
};

} // namespace v1
} // namespace lgfx
