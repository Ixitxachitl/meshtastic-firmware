#pragma once

// Display backend for the T-Display SF32's 2.16" 480x480 CO5300 AMOLED.
//
// TFTDisplay picks this up through VARIANT_DISPLAY_DRIVER, so the shared file
// does not grow a branch for a panel only this board has. Pixels reach the
// panel over the LCDC in QSPI mode - see src/platform/sifli/LGFX_Bus_LCDC.hpp.

#include "LGFX_Bus_LCDC.hpp"
#include "lgfx_amoled/Panel_CO5300.hpp"
#include <LovyanGFX.hpp>

// LovyanGFX's stock Panel_CO5300 is sized for the T-Watch Ultra's 502x410
// module, which also carries a 22-column offset. This module is 480x480 with
// no offset; the init sequence is LilyGo's, from doc/lcd_tp/init (3).c in
// their board repo.
class Panel_CO5300_TDisplaySF32 : public lgfx::Panel_CO5300
{
  public:
    Panel_CO5300_TDisplaySF32()
    {
        _cfg.memory_width = _cfg.panel_width = 480;
        _cfg.memory_height = _cfg.panel_height = 480;
        _write_depth = lgfx::color_depth_t::rgb565_2Byte;
        _read_depth = lgfx::color_depth_t::rgb565_2Byte;
    }

    const uint8_t *getInitCommands(uint8_t listno) const override
    {
        static constexpr uint8_t list0[] = {
            0xFE, 1,    0x00,                   // page 0
            0xC4, 1,    0x80,                   // SPI setting, MIPI off
            0x3A, 1,    0x55,                   // 16 bit/pixel (0x77 would be 24)
            0x35, 1,    0x00,                   // tearing effect on
            0x53, 1,    0x20,                   // brightness control on
            0x51, 1,    0xFF,                   // brightness max
            0x63, 1,    0xFF,                   // HBM brightness
            0x2A, 4,    0x00, 0x00, 0x01, 0xDF, // columns 0-479
            0x2B, 4,    0x00, 0x00, 0x01, 0xDF, // rows 0-479
            0x11, 0x80, 60,                     // sleep out, then 60 ms
            0x29, 0x80, 10,                     // display on, then 10 ms
            0xFF, 0xFF                          // end
        };
        return listno == 0 ? list0 : nullptr;
    }
};

class LGFX : public lgfx::LGFX_Device
{
  public:
    LGFX();

  private:
    Panel_CO5300_TDisplaySF32 _panel_instance;
    lgfx::Bus_SF32LB_LCDC _bus_instance;
};
