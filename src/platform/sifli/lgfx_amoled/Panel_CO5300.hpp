// Copied from LovyanGFX 1.2.26 (src/lgfx/v1/panel/Panel_CO5300.hpp). The only substantive
// change is dropping the `#if defined (ESP_PLATFORM)` guard around the whole
// file, along with the unused esp_log.h include; the repo's clang-format has
// also reflowed it, so diff against upstream with whitespace ignored. Nothing else in these
// panels is ESP32-specific - they talk to lgfx::IBus and nothing more - so the
// guard is all that kept them off this platform. Upstreaming that would let
// these copies go away.
// Licence: FreeBSD, as in the LovyanGFX header below.

/*----------------------------------------------------------------------------/
 *  Lovyan GFX - Graphics library for embedded devices.
 *
 * Original Source:
 * https://github.com/lovyan03/LovyanGFX/
 *
 * Licence:
 * [FreeBSD](https://github.com/lovyan03/LovyanGFX/blob/master/license.txt)
 *
 * Author:
 * [lovyan03](https://twitter.com/lovyan03)
 *
 * Contributors:
 * [ciniml](https://github.com/ciniml)
 * [mongonta0716](https://github.com/mongonta0716)
 * [tobozo](https://github.com/tobozo)
 * /----------------------------------------------------------------------------*/
#pragma once

#include "Panel_AMOLED.hpp"
#include <lgfx/v1/panel/Panel_FrameBufferBase.hpp>
#include <lgfx/v1/platforms/common.hpp>
#include <lgfx/v1/platforms/device.hpp>

#if defined LGFX_USE_QSPI

namespace lgfx
{
inline namespace v1
{
//----------------------------------------------------------------------------

// Panel used by LilyGO T-Watch-Ultra-AMOLED

struct Panel_CO5300 : public Panel_AMOLED {
  public:
    Panel_CO5300(void)
    {
        _cfg.memory_width = _cfg.panel_width = 502;
        _cfg.memory_height = _cfg.panel_height = 410;
        _write_depth = color_depth_t::rgb565_2Byte;
        _read_depth = color_depth_t::rgb565_2Byte;
        _cfg.dummy_read_pixel = 1;
    }

    const uint8_t *getInitCommands(uint8_t listno) const override
    {
        static constexpr uint8_t list0[] = {
            0xFE, 1, 0x00,                                                 // {0xFE, {0x00}, 0x01}, //SET PAGE 00H
            0xC4, 1, 0x80, 0x3A, 1, 0x55,                                  // Interface Pixel Format: 16bit/pixel
            0x35, 1, 0x00,                                                 // TE ON
            0x53, 1, 0x20, 0x63, 1, 0xFF, 0x2A, 4, 0x00, 0x16, 0x01, 0xAF, // SET COLUMN START ADRESS SC = 0x0016 = 22 and EC =
                                                                           // 0x01AF = 431 (410 columns but an 22 offset)
            0x2B, 4, 0x00, 0x00, 0x01, 0xF5, // SET ROW START ADRESS SP = 0 and EP = 0x1F5 = 501 (502 lines)
            0x11, 0x80, 0,                   // Sleep out
            0x51, 1, 0x01,                   // display brightness dark
            0x29, 0x80, 0,                   // display on
            0x51, 1, 0x80,                   // display brightness (max = 0xff)
            0xff, 0xff                       // end
        };
        switch (listno) {
        case 0:
            return list0;
        default:
            return nullptr;
        }
    }
};

//----------------------------------------------------------------------------
} // namespace v1

} // namespace lgfx

#endif
