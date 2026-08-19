#pragma once

// LovyanGFX bus that drives a QSPI panel through the SF32LB LCDC.
//
// The CO5300 on this board is wired CS/CLK/D0-D3, which the controller calls
// LCDC_INTF_SPI_NODCX_4DATA: a command frame carrying a 32-bit header, then
// parameters or pixels on all four lines. Zephyr's mipi_dbi_sf32lb driver
// implements only 8080 and 3/4-wire SPI, and the MIPI-DBI API has no quad mode
// to express this through, so transfers go straight to the vendor HAL.
//
// Framing: Panel_AMOLED emits a command as four writeCommand(byte, 8) calls -
// 0x02/0x32, 0x00, cmd, 0x00 - followed by parameters through the same call,
// and ends the frame with wait(). HAL_LCDC_WriteDatas() instead wants the
// header and payload together, so bytes accumulate here and go out as one
// frame when a bulk payload arrives or the panel waits.

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
        // TE input, wired on this board but only useful once frames are pushed
        // from a layer buffer rather than written synchronously.
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
    // Header is 4 bytes; the rest holds inline parameters. Panels send at most
    // a handful (the 4-byte column/row windows are the longest), and bulk
    // pixel data never lands here.
    static constexpr size_t FRAME_SIZE = 32;
    static constexpr size_t HEADER_SIZE = 4;

    void append(const uint8_t *data, size_t length);
    // Splits the accumulated bytes into the HAL's address plus payload and
    // sends them, optionally followed by a bulk buffer.
    void send(const uint8_t *payload, uint32_t payload_len);

    config_t _cfg;
    LCDC_HandleTypeDef _lcdc = {};
    FlipBuffer _flip_buffer;
    uint8_t _frame[FRAME_SIZE] = {};
    size_t _frame_len = 0;
    bool _initialized = false;
};

} // namespace v1
} // namespace lgfx
