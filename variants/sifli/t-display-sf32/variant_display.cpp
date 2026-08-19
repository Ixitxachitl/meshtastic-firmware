#include "variant_display.h"
#include "variant.h"

LGFX::LGFX()
{
    {
        auto cfg = _bus_instance.config();
        // 24 MHz is a conservative starting point; the LCDC and the panel both
        // go faster, but the write path is synchronous for now.
        cfg.freq_write = 24000000;
        cfg.panel_width = TFT_WIDTH;
        cfg.panel_height = TFT_HEIGHT;
        cfg.use_te = false;
        _bus_instance.config(cfg);
    }

    {
        auto cfg = _panel_instance.config();
        // The LCDC drives CS itself, so the panel must not toggle a GPIO for it.
        cfg.pin_cs = -1;
        cfg.pin_rst = LCD_RST;
        cfg.pin_busy = -1;
        cfg.panel_width = TFT_WIDTH;
        cfg.panel_height = TFT_HEIGHT;
        cfg.memory_width = TFT_WIDTH;
        cfg.memory_height = TFT_HEIGHT;
        cfg.offset_x = 0;
        cfg.offset_y = 0;
        cfg.readable = false;
        cfg.invert = false;
        cfg.rgb_order = false;
        cfg.dlen_16bit = false;
        cfg.bus_shared = false;
        _panel_instance.config(cfg);
    }

    _panel_instance.setBus(&_bus_instance);
    setPanel(&_panel_instance);
}
