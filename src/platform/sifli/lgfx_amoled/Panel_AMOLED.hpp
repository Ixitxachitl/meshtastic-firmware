// Copied from LovyanGFX 1.2.26 (src/lgfx/v1/panel/Panel_AMOLED.hpp). The only substantive
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

#include <lgfx/v1/panel/Panel_Device.hpp>
#include <lgfx/v1/panel/Panel_FrameBufferBase.hpp>
#include <lgfx/v1/platforms/common.hpp>
#include <lgfx/v1/platforms/device.hpp>

#if defined LGFX_USE_QSPI

namespace lgfx
{
inline namespace v1
{
//----------------------------------------------------------------------------

struct Panel_AMOLED;

struct Panel_AMOLED_Framebuffer : Panel_FrameBufferBase {
  public:
    Panel_AMOLED_Framebuffer(Panel_AMOLED *panel);
    virtual ~Panel_AMOLED_Framebuffer(void) { deinitFramebuffer(); }
    bool init(bool use_reset) override;
    bool initFramebuffer(uint_fast16_t w, uint_fast16_t h);
    void deinitFramebuffer(void);

    // TODO: reinit frame buffer when one of those functions is called
    // color_depth_t setColorDepth(color_depth_t depth) override;
    // void setRotation(uint_fast8_t r) override;
    void setInvert(bool invert) override;
    void setBrightness(uint8_t brightness) override;
    uint_fast8_t getTouchRaw(touch_point_t *tp, uint_fast8_t count) override;

    void display(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h) override;

  protected:
    uint8_t *_frame_buffer = nullptr;
    Panel_AMOLED *_panel = nullptr;
};

//----------------------------------------------------------------------------

struct Panel_AMOLED : public Panel_Device {
  protected:
    bool _in_transaction = false;

    Panel_AMOLED_Framebuffer *_panel_fb = nullptr;

    uint16_t _colstart = 0;
    uint16_t _rowstart = 0;

  public:
    Panel_AMOLED(void) {}

    ~Panel_AMOLED(void) { deinitPanelFb(); }

    bool init(bool use_reset) override;
    void beginTransaction(void) override;
    void endTransaction(void) override;

    bool initPanelFb()
    {
        if (_panel_fb)
            return true;
        _panel_fb = new Panel_AMOLED_Framebuffer(this);
        _panel_fb->config(_cfg);
        _panel_fb->setColorDepth(_write_depth);
        _panel_fb->setRotation(getRotation());
        return _panel_fb->init(false);
    }

    void deinitPanelFb()
    {
        if (_panel_fb) {
            delete _panel_fb;
            _panel_fb = nullptr;
        }
    }

    Panel_AMOLED_Framebuffer *getPanelFb() { return _panel_fb; }

    void display(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h) override
    {
        if (_panel_fb)
            _panel_fb->display(x, y, w, h);
    }

    void command_list(const uint8_t *addr);

    void update_madctl();

    void write_cmd(uint8_t cmd);
    void start_qspi();
    void end_qspi();
    void write_bytes(const uint8_t *data, uint32_t len, bool use_dma);

    color_depth_t setColorDepth(color_depth_t depth) override;
    void setRotation(uint_fast8_t r) override;
    void setInvert(bool invert) override;
    void setSleep(bool flg) override;
    void setPowerSave(bool flg) override;
    void setBrightness(uint8_t brightness) override;

    void waitDisplay(void) override;
    bool displayBusy(void) override;

    void writePixels(pixelcopy_t *param, uint32_t len, bool use_dma) override;
    void writeBlock(uint32_t rawcolor, uint32_t len) override;

    // not sure what these display commands are for, more testing needed
    void setVerticalPartialArea(uint_fast16_t xs, uint_fast16_t xe);
    void setPartialArea(uint_fast16_t ys, uint_fast16_t ye);

    void setWindow(uint_fast16_t xs, uint_fast16_t ys, uint_fast16_t xe, uint_fast16_t ye) override;
    void drawPixelPreclipped(uint_fast16_t x, uint_fast16_t y, uint32_t rawcolor) override;
    void writeFillRectPreclipped(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, uint32_t rawcolor) override;
    void writeImage(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, pixelcopy_t *param,
                    bool use_dma) override;

    // TODO: implement those
    uint32_t readCommand(uint_fast16_t cmd, uint_fast8_t index, uint_fast8_t len) override { return 0; }
    uint32_t readData(uint_fast8_t index, uint_fast8_t len) override { return 0; }
    void readRect(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, void *dst, pixelcopy_t *param) override{};
};

//----------------------------------------------------------------------------
} // namespace v1

} // namespace lgfx

#endif
