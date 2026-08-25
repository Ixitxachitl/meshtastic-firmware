#include "configuration.h"
#include "main.h"
#include "memory/MemAudit.h"
#if USE_TFTDISPLAY

#if ARCH_PORTDUINO
#include "platform/portduino/PortduinoGlue.h"
#endif

#if defined(ARCH_ESP32)
#include <esp_heap_caps.h> // heap_caps_malloc(), for DMA-reachable pixel buffers
#endif

#ifndef TFT_BACKLIGHT_ON
#define TFT_BACKLIGHT_ON HIGH
#endif

#ifdef GPIO_EXTENDER
#include <SparkFunSX1509.h>
#include <Wire.h>
extern SX1509 gpioExtender;
#endif

#ifdef TFT_MESH_OVERRIDE
uint16_t TFT_MESH = TFT_MESH_OVERRIDE;
#else
uint16_t TFT_MESH = COLOR565(0x67, 0xEA, 0x94);
#endif

#if defined(CO5300_CS)
#include <LovyanGFX.hpp> // Graphics and font library for AMOLED driver chip

// Panel_CO5300's init table sends Sleep Out and Display On with zero delay. The CO5300 needs up
// to 120 ms after Sleep Out, or Display On is occasionally ignored and the panel stays dark.
class Panel_CO5300_Delayed : public lgfx::Panel_CO5300
{
  protected:
    const uint8_t *getInitCommands(uint8_t listno) const override
    {
        // clang-format off
        static constexpr uint8_t list0[] = {
            0xFE, 1, 0x00,                   // page 0
            0xC4, 1, 0x80,
            0x3A, 1, 0x55,                   // 16 bit/pixel
            0x35, 1, 0x00,                   // TE on
            0x53, 1, 0x20,
            0x63, 1, 0xFF,
            0x2A, 4, 0x00, 0x16, 0x01, 0xAF, // column 22..431
            0x2B, 4, 0x00, 0x00, 0x01, 0xF5, // row 0..501
            0x11, 0x80, 120,                 // sleep out, then the settle time the datasheet requires
            0x51, 1, 0x01,                   // brightness dark
            0x29, 0x80, 20,                  // display on
            0x51, 1, 0x80,                   // brightness
            0xff, 0xff
        };
        // clang-format on
        switch (listno) {
        case 0:
            return list0;
        default:
            return nullptr;
        }
    }
};

class LGFX : public lgfx::LGFX_Device
{
    Panel_CO5300_Delayed _panel_instance;
    lgfx::Bus_SPI _bus_instance;

  public:
    LGFX(void)
    {
        {
            auto cfg = _bus_instance.config();

            // configure SPI
            cfg.spi_host = CO5300_SPI_HOST; // ESP32-S2,S3,C3 : SPI2_HOST or SPI3_HOST / ESP32 : VSPI_HOST or HSPI_HOST
            cfg.spi_mode = SPI_MODE0;
            cfg.freq_write = SPI_FREQUENCY; // SPI clock for transmission (up to 80MHz, rounded to the value obtained by dividing
                                            // 80MHz by an integer)
            cfg.freq_read = SPI_READ_FREQUENCY; // SPI clock when receiving
            cfg.spi_3wire = false;              // Set to true if reception is done on the MOSI pin
            cfg.use_lock = true;                // Set to true to use transaction locking
            cfg.dma_channel = SPI_DMA_CH_AUTO;  // SPI_DMA_CH_AUTO; // Set DMA channel to use (0=not use DMA / 1=1ch / 2=ch /
                                                // SPI_DMA_CH_AUTO=auto setting)
            cfg.pin_sclk = CO5300_SCK;          // Set SPI SCLK pin number
            cfg.pin_io0 = CO5300_IO0;
            cfg.pin_io1 = CO5300_IO1;
            cfg.pin_io2 = CO5300_IO2;
            cfg.pin_io3 = CO5300_IO3;

            _bus_instance.config(cfg);              // applies the set value to the bus.
            _panel_instance.setBus(&_bus_instance); // set the bus on the panel.
        }

        {                                        // Set the display panel control.
            auto cfg = _panel_instance.config(); // Gets a structure for display panel settings.

            cfg.pin_cs = CO5300_CS;                    // Pin number where CS is connected (-1 = disable)
            cfg.pin_rst = CO5300_RESET;                // Pin number where RST is connected  (-1 = disable)
            cfg.panel_width = TFT_WIDTH;               // actual displayable width
            cfg.panel_height = TFT_HEIGHT;             // actual displayable height
            cfg.offset_rotation = TFT_OFFSET_ROTATION; // Rotation direction value offset 0~7 (4~7 is upside down)
            cfg.offset_x = TFT_OFFSET_X;
            cfg.offset_y = TFT_OFFSET_Y;
            cfg.dummy_read_pixel = 8; // Number of bits for dummy read before pixel readout
            cfg.dummy_read_bits = 1;  // Number of bits for dummy read before non-pixel data read
            cfg.readable = true;      // Set to true if data can be read
            cfg.invert = false;       // Set to true if the light/darkness of the panel is reversed
            cfg.rgb_order = false;    // Set to true if the panel's red and blue are swapped
            cfg.dlen_16bit = false;   // Set to true for panels that transmit data length in 16-bit units
            cfg.bus_shared = true;    // If the bus is shared with the SD card, set to true (bus control with drawJpgFile etc.)

            // Set the following only when the display is shifted with a driver with a variable number of pixels
            cfg.memory_width = TFT_WIDTH;   // Maximum width supported by the driver IC
            cfg.memory_height = TFT_HEIGHT; // Maximum height supported by the driver IC
            _panel_instance.config(cfg);
        }

        setPanel(&_panel_instance);
    }

    bool init()
    {
#ifdef CO5300_RESET
        LOG_DEBUG("LGFX_Panel_CO5300::init()");
        lgfx::pinMode(CO5300_RESET, lgfx::pin_mode_t::output);
        lgfx::gpio_hi(CO5300_RESET);
        delay(20);
        lgfx::gpio_lo(CO5300_RESET);
        delay(30);
        lgfx::gpio_hi(CO5300_RESET);
        delay(20);
#endif
        return lgfx::LGFX_Device::init();
    }
};

static LGFX *tft = nullptr;

#endif

#if defined(ST7735S)
#include <LovyanGFX.hpp> // Graphics and font library for ST7735 driver chip

#ifndef TFT_INVERT
#define TFT_INVERT true
#endif

class LGFX : public lgfx::LGFX_Device
{
    lgfx::Panel_ST7735S _panel_instance;
    lgfx::Bus_SPI _bus_instance;
    lgfx::Light_PWM _light_instance;

  public:
    LGFX(void)
    {
        {
            auto cfg = _bus_instance.config();

            // configure SPI
            cfg.spi_host = ST7735_SPI_HOST; // ESP32-S2,S3,C3 : SPI2_HOST or SPI3_HOST / ESP32 : VSPI_HOST or HSPI_HOST
            cfg.spi_mode = 0;
            cfg.freq_write = SPI_FREQUENCY; // SPI clock for transmission (up to 80MHz, rounded to the value obtained by dividing
                                            // 80MHz by an integer)
            cfg.freq_read = SPI_READ_FREQUENCY; // SPI clock when receiving
            cfg.spi_3wire = false;              // Set to true if reception is done on the MOSI pin
            cfg.use_lock = true;                // Set to true to use transaction locking
            cfg.dma_channel = SPI_DMA_CH_AUTO;  // SPI_DMA_CH_AUTO; // Set DMA channel to use (0=not use DMA / 1=1ch / 2=ch /
                                                // SPI_DMA_CH_AUTO=auto setting)
            cfg.pin_sclk = ST7735_SCK;          // Set SPI SCLK pin number
            cfg.pin_mosi = ST7735_SDA;          // Set SPI MOSI pin number
            cfg.pin_miso = ST7735_MISO;         // Set SPI MISO pin number (-1 = disable)
            cfg.pin_dc = ST7735_RS;             // Set SPI DC pin number (-1 = disable)

            _bus_instance.config(cfg);              // applies the set value to the bus.
            _panel_instance.setBus(&_bus_instance); // set the bus on the panel.
        }

        {                                        // Set the display panel control.
            auto cfg = _panel_instance.config(); // Gets a structure for display panel settings.

            cfg.pin_cs = ST7735_CS;     // Pin number where CS is connected (-1 = disable)
            cfg.pin_rst = ST7735_RESET; // Pin number where RST is connected  (-1 = disable)
            cfg.pin_busy = ST7735_BUSY; // Pin number where BUSY is connected (-1 = disable)

            // The following setting values ​​are general initial values ​​for each panel, so please comment out any
            // unknown items and try them.

            cfg.panel_width = TFT_WIDTH;   // actual displayable width
            cfg.panel_height = TFT_HEIGHT; // actual displayable height
            cfg.offset_x = TFT_OFFSET_X;   // Panel offset amount in X direction
            cfg.offset_y = TFT_OFFSET_Y;   // Panel offset amount in Y direction
            cfg.offset_rotation = 0;       // Rotation direction value offset 0~7 (4~7 is upside down)
            cfg.dummy_read_pixel = 8;      // Number of bits for dummy read before pixel readout
            cfg.dummy_read_bits = 1;       // Number of bits for dummy read before non-pixel data read
            cfg.readable = true;           // Set to true if data can be read
            cfg.invert = TFT_INVERT;       // Set to true if the light/darkness of the panel is reversed
            cfg.rgb_order = false;         // Set to true if the panel's red and blue are swapped
            cfg.dlen_16bit =
                false;             // Set to true for panels that transmit data length in 16-bit units with 16-bit parallel or SPI
            cfg.bus_shared = true; // If the bus is shared with the SD card, set to true (bus control with drawJpgFile etc.)

            // Set the following only when the display is shifted with a driver with a variable number of pixels, such as the
            // ST7735 or ILI9163.
            cfg.memory_width = TFT_WIDTH;   // Maximum width supported by the driver IC
            cfg.memory_height = TFT_HEIGHT; // Maximum height supported by the driver IC
            _panel_instance.config(cfg);
        }

#ifdef TFT_BL
        // Set the backlight control
        {
            auto cfg = _light_instance.config(); // Gets a structure for backlight settings.

            cfg.pin_bl = TFT_BL; // Pin number to which the backlight is connected
            cfg.invert = true;   // true to invert the brightness of the backlight
            // cfg.freq = 44100;    // PWM frequency of backlight
            // cfg.pwm_channel = 1; // PWM channel number to use

            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance); // Set the backlight on the panel.
        }
#endif

        setPanel(&_panel_instance);
    }
};

static LGFX *tft = nullptr;

#elif defined(RAK14014)
#include <RAK14014_FT6336U.h>
#include <TFT_eSPI.h>
TFT_eSPI *tft = nullptr;
FT6336U ft6336u;

static uint8_t _rak14014_touch_int = false; // TP interrupt generation flag.
static void rak14014_tpIntHandle(void)
{
    _rak14014_touch_int = true;
}

#elif defined(USE_ARDUINO_GFX)
#include <Arduino_GFX_Library.h>
Arduino_GFX *tft = nullptr;

#elif defined(ST72xx_DE)
#include <LovyanGFX.hpp>
#include <TCA9534.h>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
TCA9534 ioex;

class LGFX : public lgfx::LGFX_Device
{
    lgfx::Bus_RGB _bus_instance;
    lgfx::Panel_RGB _panel_instance;
    lgfx::Touch_GT911 _touch_instance;

  public:
    const uint16_t screenWidth = TFT_WIDTH;
    const uint16_t screenHeight = TFT_HEIGHT;

    bool init_impl(bool use_reset, bool use_clear) override
    {
        ioex.attach(Wire);
        ioex.setDeviceAddress(0x18);
        ioex.config(1, TCA9534::Config::OUT);
        ioex.config(2, TCA9534::Config::OUT);
        ioex.config(3, TCA9534::Config::OUT);
        ioex.config(4, TCA9534::Config::OUT);

        ioex.output(1, TCA9534::Level::H);
        ioex.output(3, TCA9534::Level::L);
        ioex.output(4, TCA9534::Level::H);

        pinMode(1, OUTPUT);
        digitalWrite(1, LOW);
        ioex.output(2, TCA9534::Level::L);
        delay(20);
        ioex.output(2, TCA9534::Level::H);
        delay(100);
        pinMode(1, INPUT);

        return LGFX_Device::init_impl(use_reset, use_clear);
    }

    LGFX(void)
    {
        {
            auto cfg = _panel_instance.config();

            cfg.memory_width = screenWidth;
            cfg.memory_height = screenHeight;
            cfg.panel_width = screenWidth;
            cfg.panel_height = screenHeight;
            cfg.offset_x = 0;
            cfg.offset_y = 0;
            cfg.offset_rotation = 0;
            _panel_instance.config(cfg);
        }

        {
            auto cfg = _panel_instance.config_detail();
            cfg.use_psram = 0;
            _panel_instance.config_detail(cfg);
        }

        {
            auto cfg = _bus_instance.config();
            cfg.panel = &_panel_instance;
            cfg.pin_d0 = ST72xx_B0;  // B0
            cfg.pin_d1 = ST72xx_B1;  // B1
            cfg.pin_d2 = ST72xx_B2;  // B2
            cfg.pin_d3 = ST72xx_B3;  // B3
            cfg.pin_d4 = ST72xx_B4;  // B4
            cfg.pin_d5 = ST72xx_G0;  // G0
            cfg.pin_d6 = ST72xx_G1;  // G1
            cfg.pin_d7 = ST72xx_G2;  // G2
            cfg.pin_d8 = ST72xx_G3;  // G3
            cfg.pin_d9 = ST72xx_G4;  // G4
            cfg.pin_d10 = ST72xx_G5; // G5
            cfg.pin_d11 = ST72xx_R0; // R0
            cfg.pin_d12 = ST72xx_R1; // R1
            cfg.pin_d13 = ST72xx_R2; // R2
            cfg.pin_d14 = ST72xx_R3; // R3
            cfg.pin_d15 = ST72xx_R4; // R4

            cfg.pin_henable = ST72xx_DE;
            cfg.pin_vsync = ST72xx_VSYNC;
            cfg.pin_hsync = ST72xx_HSYNC;
            cfg.pin_pclk = ST72xx_PCLK;
            cfg.freq_write = 13000000;

#ifdef ST7265_HSYNC_POLARITY
            cfg.hsync_polarity = ST7265_HSYNC_POLARITY;
            cfg.hsync_front_porch = ST7265_HSYNC_FRONT_PORCH; // 8;
            cfg.hsync_pulse_width = ST7265_HSYNC_PULSE_WIDTH; // 4;
            cfg.hsync_back_porch = ST7265_HSYNC_BACK_PORCH;   // 8;

            cfg.vsync_polarity = ST7265_VSYNC_POLARITY;       // 0;
            cfg.vsync_front_porch = ST7265_VSYNC_FRONT_PORCH; // 8;
            cfg.vsync_pulse_width = ST7265_VSYNC_PULSE_WIDTH; // 4;
            cfg.vsync_back_porch = ST7265_VSYNC_BACK_PORCH;   // 8;

            cfg.pclk_idle_high = 1;
            cfg.pclk_active_neg = ST7265_PCLK_ACTIVE_NEG; // 0;
            // cfg.pclk_idle_high = 0;
            // cfg.de_idle_high = 1;
#endif

#ifdef ST7262_HSYNC_POLARITY
            cfg.hsync_polarity = ST7262_HSYNC_POLARITY;
            cfg.hsync_front_porch = ST7262_HSYNC_FRONT_PORCH; // 8;
            cfg.hsync_pulse_width = ST7262_HSYNC_PULSE_WIDTH; // 4;
            cfg.hsync_back_porch = ST7262_HSYNC_BACK_PORCH;   // 8;

            cfg.vsync_polarity = ST7262_VSYNC_POLARITY;       // 0;
            cfg.vsync_front_porch = ST7262_VSYNC_FRONT_PORCH; // 8;
            cfg.vsync_pulse_width = ST7262_VSYNC_PULSE_WIDTH; // 4;
            cfg.vsync_back_porch = ST7262_VSYNC_BACK_PORCH;   // 8;

            cfg.pclk_idle_high = 1;
            cfg.pclk_active_neg = ST7262_PCLK_ACTIVE_NEG; // 0;
            // cfg.pclk_idle_high = 0;
            // cfg.de_idle_high = 1;
#endif

#ifdef SC7277_HSYNC_POLARITY
            cfg.hsync_polarity = SC7277_HSYNC_POLARITY;
            cfg.hsync_front_porch = SC7277_HSYNC_FRONT_PORCH; // 8;
            cfg.hsync_pulse_width = SC7277_HSYNC_PULSE_WIDTH; // 4;
            cfg.hsync_back_porch = SC7277_HSYNC_BACK_PORCH;   // 8;

            cfg.vsync_polarity = SC7277_VSYNC_POLARITY;       // 0;
            cfg.vsync_front_porch = SC7277_VSYNC_FRONT_PORCH; // 8;
            cfg.vsync_pulse_width = SC7277_VSYNC_PULSE_WIDTH; // 4;
            cfg.vsync_back_porch = SC7277_VSYNC_BACK_PORCH;   // 8;

            cfg.pclk_idle_high = 1;
            cfg.pclk_active_neg = SC7277_PCLK_ACTIVE_NEG; // 0;
            // cfg.pclk_idle_high = 0;
            // cfg.de_idle_high = 1;
#endif

            _bus_instance.config(cfg);
        }
        _panel_instance.setBus(&_bus_instance);

        {
            auto cfg = _touch_instance.config();
            cfg.x_min = 0;
            cfg.x_max = TFT_WIDTH;
            cfg.y_min = 0;
            cfg.y_max = TFT_HEIGHT;
            cfg.pin_int = -1;
            cfg.pin_rst = -1;
            cfg.bus_shared = true;
            cfg.offset_rotation = 0;

            cfg.i2c_port = 0;
            cfg.i2c_addr = 0x5D;
            cfg.pin_sda = I2C_SDA;
            cfg.pin_scl = I2C_SCL;
            cfg.freq = 400000;
            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }

        setPanel(&_panel_instance);
    }
};

static LGFX *tft = nullptr;

#elif defined(ILI9488_CS)
#include <LovyanGFX.hpp> // Graphics and font library for ILI9488 driver chip

class LGFX : public lgfx::LGFX_Device
{
    lgfx::Panel_ILI9488 _panel_instance;
    lgfx::Bus_SPI _bus_instance;
    lgfx::Light_PWM _light_instance;
    lgfx::Touch_GT911 _touch_instance;

  public:
    LGFX(void)
    {
        {
            auto cfg = _bus_instance.config();

            // configure SPI
            cfg.spi_host = ILI9488_SPI_HOST; // ESP32-S2,S3,C3 : SPI2_HOST or SPI3_HOST / ESP32 : VSPI_HOST or HSPI_HOST
            cfg.spi_mode = 0;
            cfg.freq_write = SPI_FREQUENCY; // SPI clock for transmission (up to 80MHz, rounded to the value obtained by dividing
                                            // 80MHz by an integer)
            cfg.freq_read = SPI_READ_FREQUENCY; // SPI clock when receiving
            cfg.spi_3wire = false;              // Set to true if reception is done on the MOSI pin
            cfg.use_lock = true;                // Set to true to use transaction locking
            cfg.dma_channel = SPI_DMA_CH_AUTO;  // SPI_DMA_CH_AUTO; // Set DMA channel to use (0=not use DMA / 1=1ch / 2=ch /
                                                // SPI_DMA_CH_AUTO=auto setting)
            cfg.pin_sclk = ILI9488_SCK;         // Set SPI SCLK pin number
            cfg.pin_mosi = ILI9488_SDA;         // Set SPI MOSI pin number
            cfg.pin_miso = ILI9488_MISO;        // Set SPI MISO pin number (-1 = disable)
            cfg.pin_dc = ILI9488_RS;            // Set SPI DC pin number (-1 = disable)

            _bus_instance.config(cfg);              // applies the set value to the bus.
            _panel_instance.setBus(&_bus_instance); // set the bus on the panel.
        }

        {                                        // Set the display panel control.
            auto cfg = _panel_instance.config(); // Gets a structure for display panel settings.

            cfg.pin_cs = ILI9488_CS; // Pin number where CS is connected (-1 = disable)
            cfg.pin_rst = -1;        // Pin number where RST is connected  (-1 = disable)
            cfg.pin_busy = -1;       // Pin number where BUSY is connected (-1 = disable)

            // The following setting values ​​are general initial values ​​for each panel, so please comment out any
            // unknown items and try them.

            cfg.memory_width = TFT_WIDTH;                 // Maximum width supported by the driver IC
            cfg.memory_height = TFT_HEIGHT;               // Maximum height supported by the driver IC
            cfg.panel_width = TFT_WIDTH;                  // actual displayable width
            cfg.panel_height = TFT_HEIGHT;                // actual displayable height
            cfg.offset_x = TFT_OFFSET_X;                  // Panel offset amount in X direction
            cfg.offset_y = TFT_OFFSET_Y;                  // Panel offset amount in Y direction
            cfg.offset_rotation = TFT_OFFSET_ROTATION;    // Rotation direction value offset 0~7 (4~7 is mirrored)
#ifdef TFT_DUMMY_READ_PIXELS
            cfg.dummy_read_pixel = TFT_DUMMY_READ_PIXELS; // Number of bits for dummy read before pixel readout
#else
            cfg.dummy_read_pixel = 9; // Number of bits for dummy read before pixel readout
#endif
            cfg.dummy_read_bits = 1;                      // Number of bits for dummy read before non-pixel data read
            cfg.readable = true;                          // Set to true if data can be read
            cfg.invert = true;                            // Set to true if the light/darkness of the panel is reversed
            cfg.rgb_order = false;                        // Set to true if the panel's red and blue are swapped
            cfg.dlen_16bit =
                false;             // Set to true for panels that transmit data length in 16-bit units with 16-bit parallel or SPI
            cfg.bus_shared = true; // If the bus is shared with the SD card, set to true (bus control with drawJpgFile etc.)

            // Set the following only when the display is shifted with a driver with a variable number of pixels, such as the
            // ST7735 or ILI9163.
            // cfg.memory_width = TFT_WIDTH;   // Maximum width supported by the driver IC
            // cfg.memory_height = TFT_HEIGHT; // Maximum height supported by the driver IC
            _panel_instance.config(cfg);
        }

#ifdef ILI9488_BL
        // Set the backlight control
        {
            auto cfg = _light_instance.config(); // Gets a structure for backlight settings.

            cfg.pin_bl = ILI9488_BL; // Pin number to which the backlight is connected
            cfg.invert = false;      // true to invert the brightness of the backlight
            // cfg.freq = 44100;    // PWM frequency of backlight
            // cfg.pwm_channel = 1; // PWM channel number to use

            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance); // Set the backlight on the panel.
        }
#endif

#if HAS_TOUCHSCREEN
        // Configure settings for touch screen control.
        {
            auto cfg = _touch_instance.config();

            cfg.pin_cs = -1;
            cfg.x_min = 0;
            cfg.x_max = TFT_HEIGHT - 1;
            cfg.y_min = 0;
            cfg.y_max = TFT_WIDTH - 1;
            cfg.pin_int = SCREEN_TOUCH_INT;
#ifdef SCREEN_TOUCH_RST
            cfg.pin_rst = SCREEN_TOUCH_RST;
#endif
            cfg.bus_shared = true;
            cfg.offset_rotation = TFT_OFFSET_ROTATION;
            // cfg.freq = 2500000;

            // I2C
            cfg.i2c_port = TOUCH_I2C_PORT;
            cfg.i2c_addr = TOUCH_SLAVE_ADDRESS;
#ifdef SCREEN_TOUCH_USE_I2C1
            cfg.pin_sda = I2C_SDA1;
            cfg.pin_scl = I2C_SCL1;
#else
            cfg.pin_sda = I2C_SDA;
            cfg.pin_scl = I2C_SCL;
#endif
            // cfg.freq = 400000;

            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }
#endif

        setPanel(&_panel_instance);
    }
};

static LGFX *tft = nullptr;

#elif defined(ST7789_CS)
#include <LovyanGFX.hpp> // Graphics and font library for ST7735 driver chip
#if defined(HELTEC_V4_TFT) || defined(HELTEC_V4_R8_TFT)
#include "chsc6x.h"
#include "lgfx/v1/Touch.hpp"
namespace lgfx
{
inline namespace v1
{
class TOUCH_CHSC6X : public ITouch
{
  public:
    TOUCH_CHSC6X(void)
    {
        _cfg.i2c_addr = TOUCH_SLAVE_ADDRESS;
        _cfg.x_min = 0;
        _cfg.x_max = 240;
        _cfg.y_min = 0;
        _cfg.y_max = 320;
    };

    bool init(void) override
    {
        if (chsc6xTouch == nullptr) {
#if (TOUCH_I2C_PORT == 1)
            chsc6xTouch = new chsc6x(&Wire1, TOUCH_SDA_PIN, TOUCH_SCL_PIN, TOUCH_INT_PIN, TOUCH_RST_PIN);
#else
            chsc6xTouch = new chsc6x(&Wire, TOUCH_SDA_PIN, TOUCH_SCL_PIN, TOUCH_INT_PIN, TOUCH_RST_PIN);
#endif
        }
        chsc6xTouch->chsc6x_init();
        return true;
    };

    uint_fast8_t getTouchRaw(touch_point_t *tp, uint_fast8_t count) override
    {
        uint16_t raw_x, raw_y;
        if (chsc6xTouch->chsc6x_read_touch_info(&raw_x, &raw_y) == 0) {
            tp[0].x = 320 - 1 - raw_y;
            tp[0].y = 240 - 1 - raw_x;
            tp[0].size = 1;
            tp[0].id = 1;
            return 1;
        }
        tp[0].size = 0;
        return 0;
    };

    void wakeup(void) override{};
    void sleep(void) override{};

  private:
    chsc6x *chsc6xTouch = nullptr;
};
} // namespace v1
} // namespace lgfx
#endif
class LGFX : public lgfx::LGFX_Device
{
    lgfx::Panel_ST7789 _panel_instance;
    lgfx::Bus_SPI _bus_instance;
    lgfx::Light_PWM _light_instance;
#if HAS_TOUCHSCREEN
#if defined(T_WATCH_S3) || defined(ELECROW)
    lgfx::Touch_FT5x06 _touch_instance;
#elif defined(HELTEC_V4_TFT) || defined(HELTEC_V4_R8_TFT)
    lgfx::TOUCH_CHSC6X _touch_instance;
#else
    lgfx::Touch_GT911 _touch_instance;
#endif
#endif

  public:
    LGFX(void)
    {
        {
            auto cfg = _bus_instance.config();

            // SPI
            cfg.spi_host = ST7789_SPI_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = SPI_FREQUENCY; // SPI clock for transmission (up to 80MHz, rounded to the value obtained by dividing
                                            // 80MHz by an integer)
            cfg.freq_read = SPI_READ_FREQUENCY; // SPI clock when receiving
#ifdef SPI_3_WIRE
            cfg.spi_3wire = SPI_3_WIRE;
#else
            cfg.spi_3wire = true;                      // Set to true if reception is done on the MOSI pin
#endif
            cfg.use_lock = true;               // Set to true to use transaction locking
            cfg.dma_channel = SPI_DMA_CH_AUTO; // SPI_DMA_CH_AUTO; // Set DMA channel to use (0=not use DMA / 1=1ch / 2=ch /
                                               // SPI_DMA_CH_AUTO=auto setting)
            cfg.pin_sclk = ST7789_SCK;         // Set SPI SCLK pin number
            cfg.pin_mosi = ST7789_SDA;         // Set SPI MOSI pin number
            cfg.pin_miso = ST7789_MISO;        // Set SPI MISO pin number (-1 = disable)
            cfg.pin_dc = ST7789_RS;            // Set SPI DC pin number (-1 = disable)

            _bus_instance.config(cfg);              // applies the set value to the bus.
            _panel_instance.setBus(&_bus_instance); // set the bus on the panel.
        }

        {                                        // Set the display panel control.
            auto cfg = _panel_instance.config(); // Gets a structure for display panel settings.

            cfg.pin_cs = ST7789_CS;     // Pin number where CS is connected (-1 = disable)
            cfg.pin_rst = ST7789_RESET; // Pin number where RST is connected  (-1 = disable)
            cfg.pin_busy = ST7789_BUSY; // Pin number where BUSY is connected (-1 = disable)

            // The following setting values ​​are general initial values ​​for each panel, so please comment out any
            // unknown items and try them.
#if defined(T_WATCH_S3)
            cfg.panel_width = 240;
            cfg.panel_height = 240;
            cfg.memory_width = 240;
            cfg.memory_height = 320;
            cfg.offset_x = 0;
            cfg.offset_y = 0;                             // No vertical shift needed - panel is top-aligned
            cfg.offset_rotation = 2;                      // Rotate 180° to correct upside-down layout
#else
            cfg.memory_width = TFT_WIDTH;              // Maximum width supported by the driver IC
            cfg.memory_height = TFT_HEIGHT;            // Maximum height supported by the driver IC
            cfg.panel_width = TFT_WIDTH;               // actual displayable width
            cfg.panel_height = TFT_HEIGHT;             // actual displayable height
            cfg.offset_x = TFT_OFFSET_X;               // Panel offset amount in X direction
            cfg.offset_y = TFT_OFFSET_Y;               // Panel offset amount in Y direction
            cfg.offset_rotation = TFT_OFFSET_ROTATION; // Rotation direction value offset 0~7 (4~7 is mirrored)
#endif
#ifdef TFT_DUMMY_READ_PIXELS
            cfg.dummy_read_pixel = TFT_DUMMY_READ_PIXELS; // Number of bits for dummy read before pixel readout
#else
            cfg.dummy_read_pixel = 9;                  // Number of bits for dummy read before pixel readout
#endif
            cfg.dummy_read_bits = 1;                      // Number of bits for dummy read before non-pixel data read
            cfg.readable = true;                          // Set to true if data can be read
            cfg.invert = true;                            // Set to true if the light/darkness of the panel is reversed
            cfg.rgb_order = false;                        // Set to true if the panel's red and blue are swapped
            cfg.dlen_16bit =
                false;             // Set to true for panels that transmit data length in 16-bit units with 16-bit parallel or SPI
#if defined(HAS_SDCARD)
            cfg.bus_shared = true; // If the bus is shared with the SD card, set to true (bus control with drawJpgFile etc.)
#else
            cfg.bus_shared = false;
#endif
            // Set the following only when the display is shifted with a driver with a variable number of pixels, such as the
            // ST7735 or ILI9163.
            // cfg.memory_width = TFT_WIDTH;   // Maximum width supported by the driver IC
            // cfg.memory_height = TFT_HEIGHT; // Maximum height supported by the driver IC
            _panel_instance.config(cfg);
        }

#ifdef ST7789_BL
        // Set the backlight control. (delete if not necessary)
        {
            auto cfg = _light_instance.config(); // Gets a structure for backlight settings.

            cfg.pin_bl = ST7789_BL; // Pin number to which the backlight is connected
            cfg.invert = false;     // true to invert the brightness of the backlight
            // cfg.pwm_channel = 0;

            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance); // Set the backlight on the panel.
        }
#endif

#if HAS_TOUCHSCREEN
        // Configure settings for touch screen control.
        {
            auto cfg = _touch_instance.config();

            cfg.pin_cs = -1;
            cfg.x_min = 0;
            cfg.x_max = TFT_HEIGHT - 1;
            cfg.y_min = 0;
            cfg.y_max = TFT_WIDTH - 1;
            cfg.pin_int = SCREEN_TOUCH_INT;
#ifdef SCREEN_TOUCH_RST
            cfg.pin_rst = SCREEN_TOUCH_RST;
#endif
            cfg.bus_shared = true;
            cfg.offset_rotation = TFT_OFFSET_ROTATION;
            // cfg.freq = 2500000;

            // I2C
            cfg.i2c_port = TOUCH_I2C_PORT;
            cfg.i2c_addr = TOUCH_SLAVE_ADDRESS;
#ifdef SCREEN_TOUCH_USE_I2C1
            cfg.pin_sda = I2C_SDA1;
            cfg.pin_scl = I2C_SCL1;
#else
            cfg.pin_sda = I2C_SDA;
            cfg.pin_scl = I2C_SCL;
#endif
            // cfg.freq = 400000;

            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }
#endif

        setPanel(&_panel_instance); // Sets the panel to use.
    }
};

static LGFX *tft = nullptr;

#elif defined(ST7796_CS)
#include <LovyanGFX.hpp> // Graphics and font library for ST7796 driver chip

class LGFX : public lgfx::LGFX_Device
{
    lgfx::Panel_ST7796 _panel_instance;
    lgfx::Bus_SPI _bus_instance;
    lgfx::Light_PWM _light_instance;

  public:
    LGFX(void)
    {
        {
            auto cfg = _bus_instance.config();

            // SPI
            cfg.spi_host = ST7796_SPI_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = SPI_FREQUENCY; // SPI clock for transmission (up to 80MHz, rounded to the value obtained by dividing
                                            // 80MHz by an integer)
            cfg.freq_read = SPI_READ_FREQUENCY; // SPI clock when receiving
            cfg.spi_3wire = false;
            cfg.use_lock = true;               // Set to true to use transaction locking
            cfg.dma_channel = SPI_DMA_CH_AUTO; // SPI_DMA_CH_AUTO; // Set DMA channel to use (0=not use DMA / 1=1ch / 2=ch /
                                               // SPI_DMA_CH_AUTO=auto setting)
            cfg.pin_sclk = ST7796_SCK;         // Set SPI SCLK pin number
            cfg.pin_mosi = ST7796_SDA;         // Set SPI MOSI pin number
            cfg.pin_miso = ST7796_MISO;        // Set SPI MISO pin number (-1 = disable)
            cfg.pin_dc = ST7796_RS;            // Set SPI DC pin number (-1 = disable)

            _bus_instance.config(cfg);              // applies the set value to the bus.
            _panel_instance.setBus(&_bus_instance); // set the bus on the panel.
        }

        {                                        // Set the display panel control.
            auto cfg = _panel_instance.config(); // Gets a structure for display panel settings.

            cfg.pin_cs = ST7796_CS;     // Pin number where CS is connected (-1 = disable)
            cfg.pin_rst = ST7796_RESET; // Pin number where RST is connected  (-1 = disable)
            cfg.pin_busy = ST7796_BUSY; // Pin number where BUSY is connected (-1 = disable)

            // cfg.memory_width = TFT_WIDTH;              // Maximum width supported by the driver IC
            // cfg.memory_height = TFT_HEIGHT;            // Maximum height supported by the driver IC
            cfg.panel_width = TFT_WIDTH;                  // actual displayable width
            cfg.panel_height = TFT_HEIGHT;                // actual displayable height
            cfg.offset_x = TFT_OFFSET_X;                  // Panel offset amount in X direction
            cfg.offset_y = TFT_OFFSET_Y;                  // Panel offset amount in Y direction
            cfg.offset_rotation = TFT_OFFSET_ROTATION;    // Rotation direction value offset 0~7 (4~7 is mirrored)
#ifdef TFT_DUMMY_READ_PIXELS
            cfg.dummy_read_pixel = TFT_DUMMY_READ_PIXELS; // Number of bits for dummy read before pixel readout
#else
            cfg.dummy_read_pixel = 8; // Number of bits for dummy read before pixel readout
#endif
            cfg.dummy_read_bits = 1;                      // Number of bits for dummy read before non-pixel data read
            cfg.readable = true;                          // Set to true if data can be read
            cfg.invert = true;                            // Set to true if the light/darkness of the panel is reversed
            cfg.rgb_order = false;                        // Set to true if the panel's red and blue are swapped
            cfg.dlen_16bit =
                false;             // Set to true for panels that transmit data length in 16-bit units with 16-bit parallel or SPI
            cfg.bus_shared = true; // If the bus is shared with the SD card, set to true (bus control with drawJpgFile etc.)

            _panel_instance.config(cfg);
        }

#ifdef ST7796_BL
        // Set the backlight control. (delete if not necessary)
        {
            auto cfg = _light_instance.config(); // Gets a structure for backlight settings.

            cfg.pin_bl = ST7796_BL; // Pin number to which the backlight is connected
            cfg.invert = false;     // true to invert the brightness of the backlight
            cfg.freq = 44100;
            cfg.pwm_channel = 7;

            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance); // Set the backlight on the panel.
        }
#endif

        setPanel(&_panel_instance); // Sets the panel to use.
    }
};

static LGFX *tft = nullptr;

#elif defined(ILI9341_DRIVER) || defined(ILI9342_DRIVER)

#include <LovyanGFX.hpp> // Graphics and font library for ILI9341/ILI9342 driver chip

#if defined(ILI9341_BACKLIGHT_EN) && !defined(TFT_BL)
#define TFT_BL ILI9341_BACKLIGHT_EN
#endif

class LGFX : public lgfx::LGFX_Device
{
#if defined(ILI9341_DRIVER)
    lgfx::Panel_ILI9341 _panel_instance;
#elif defined(ILI9342_DRIVER)
    lgfx::Panel_ILI9342 _panel_instance;
#endif
    lgfx::Bus_SPI _bus_instance;
    lgfx::Light_PWM _light_instance;

  public:
    LGFX(void)
    {
        {
            auto cfg = _bus_instance.config();

            // configure SPI
#if defined(ILI9341_DRIVER)
            cfg.spi_host = ILI9341_SPI_HOST; // ESP32-S2,S3,C3 : SPI2_HOST or SPI3_HOST / ESP32 : VSPI_HOST or HSPI_HOST
#elif defined(ILI9342_DRIVER)
            cfg.spi_host = ILI9342_SPI_HOST; // ESP32-S2,S3,C3 : SPI2_HOST or SPI3_HOST / ESP32 : VSPI_HOST or HSPI_HOST
#endif
            cfg.spi_mode = 0;
            cfg.freq_write = SPI_FREQUENCY; // SPI clock for transmission (up to 80MHz, rounded to the value obtained by dividing
                                            // 80MHz by an integer)
            cfg.freq_read = SPI_READ_FREQUENCY; // SPI clock when receiving
            cfg.spi_3wire = false;              // Set to true if reception is done on the MOSI pin
            cfg.use_lock = true;                // Set to true to use transaction locking
            cfg.dma_channel = SPI_DMA_CH_AUTO;  // SPI_DMA_CH_AUTO; // Set DMA channel to use (0=not use DMA / 1=1ch / 2=ch /
                                                // SPI_DMA_CH_AUTO=auto setting)
            cfg.pin_sclk = TFT_SCLK;            // Set SPI SCLK pin number
            cfg.pin_mosi = TFT_MOSI;            // Set SPI MOSI pin number
            cfg.pin_miso = TFT_MISO;            // Set SPI MISO pin number (-1 = disable)
            cfg.pin_dc = TFT_DC;                // Set SPI DC pin number (-1 = disable)

            _bus_instance.config(cfg);              // applies the set value to the bus.
            _panel_instance.setBus(&_bus_instance); // set the bus on the panel.
        }

        {                                        // Set the display panel control.
            auto cfg = _panel_instance.config(); // Gets a structure for display panel settings.

            cfg.pin_cs = TFT_CS;     // Pin number where CS is connected (-1 = disable)
            cfg.pin_rst = TFT_RST;   // Pin number where RST is connected  (-1 = disable)
            cfg.pin_busy = TFT_BUSY; // Pin number where BUSY is connected (-1 = disable)

            // The following setting values ​​are general initial values ​​for each panel, so please comment out any
            // unknown items and try them.

            cfg.panel_width = TFT_WIDTH;   // actual displayable width
            cfg.panel_height = TFT_HEIGHT; // actual displayable height
            cfg.offset_x = TFT_OFFSET_X;   // Panel offset amount in X direction
            cfg.offset_y = TFT_OFFSET_Y;   // Panel offset amount in Y direction
            cfg.offset_rotation = 0;       // Rotation direction value offset 0~7 (4~7 is upside down)
            cfg.dummy_read_pixel = 8;      // Number of bits for dummy read before pixel readout
            cfg.dummy_read_bits = 1;       // Number of bits for dummy read before non-pixel data read
            cfg.readable = true;           // Set to true if data can be read
            cfg.invert = false;            // Set to true if the light/darkness of the panel is reversed
            cfg.rgb_order = false;         // Set to true if the panel's red and blue are swapped
            cfg.dlen_16bit =
                false;             // Set to true for panels that transmit data length in 16-bit units with 16-bit parallel or SPI
            cfg.bus_shared = true; // If the bus is shared with the SD card, set to true (bus control with drawJpgFile etc.)

            // Set the following only when the display is shifted with a driver with a variable number of pixels, such as the
            // ST7735 or ILI9163.
            cfg.memory_width = TFT_WIDTH;   // Maximum width supported by the driver IC
            cfg.memory_height = TFT_HEIGHT; // Maximum height supported by the driver IC
            _panel_instance.config(cfg);
        }

#ifdef TFT_BL
        // Set the backlight control
        {
            auto cfg = _light_instance.config(); // Gets a structure for backlight settings.

            cfg.pin_bl = TFT_BL; // Pin number to which the backlight is connected
            cfg.invert = false;  // true to invert the brightness of the backlight
            // cfg.freq = 44100;    // PWM frequency of backlight
            // cfg.pwm_channel = 1; // PWM channel number to use

            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance); // Set the backlight on the panel.
        }
#endif

        setPanel(&_panel_instance);
    }
};

static LGFX *tft = nullptr;

#elif defined(ST7735_CS)
#include <TFT_eSPI.h> // Graphics and font library for ILI9342 driver chip

static TFT_eSPI *tft = nullptr; // Invoke library, pins defined in User_Setup.h
#elif ARCH_PORTDUINO
#include "Panel_sdl.hpp"
#include <LovyanGFX.hpp> // Graphics and font library for ST7735 driver chip

class LGFX : public lgfx::LGFX_Device
{
    lgfx::Bus_SPI _bus_instance;

    lgfx::ITouch *_touch_instance = nullptr;

  public:
    lgfx::Panel_Device *_panel_instance;

    LGFX(void)
    {
        if (portduino_config.displayPanel == st7789)
            _panel_instance = new lgfx::Panel_ST7789;
        else if (portduino_config.displayPanel == st7735)
            _panel_instance = new lgfx::Panel_ST7735;
        else if (portduino_config.displayPanel == st7735s)
            _panel_instance = new lgfx::Panel_ST7735S;
        else if (portduino_config.displayPanel == st7796)
            _panel_instance = new lgfx::Panel_ST7796;
        else if (portduino_config.displayPanel == ili9341)
            _panel_instance = new lgfx::Panel_ILI9341;
        else if (portduino_config.displayPanel == ili9342)
            _panel_instance = new lgfx::Panel_ILI9342;
        else if (portduino_config.displayPanel == ili9488)
            _panel_instance = new lgfx::Panel_ILI9488;
        else if (portduino_config.displayPanel == hx8357d)
            _panel_instance = new lgfx::Panel_HX8357D;
#if defined(SDL_h_)

        else if (portduino_config.displayPanel == sdl)
            _panel_instance = new lgfx::Panel_sdl;
#endif
        else {
            _panel_instance = new lgfx::Panel_NULL;
            LOG_ERROR("Unknown display panel configured");
        }

        auto buscfg = _bus_instance.config();
        buscfg.spi_mode = 0;
        buscfg.spi_host = portduino_config.display_spi_dev_int;

        buscfg.pin_dc = portduino_config.displayDC.pin; // Set SPI DC pin number (-1 = disable)

        _bus_instance.config(buscfg); // applies the set value to the bus.
        if (portduino_config.displayPanel != x11 && portduino_config.displayPanel != sdl)
            _panel_instance->setBus(&_bus_instance); // set the bus on the panel.

        auto cfg = _panel_instance->config(); // Gets a structure for display panel settings.
        LOG_DEBUG("Width: %d, Height: %d", portduino_config.displayWidth, portduino_config.displayHeight);
        cfg.pin_cs = portduino_config.displayCS.pin; // Pin number where CS is connected (-1 = disable)
        cfg.pin_rst = portduino_config.displayReset.pin;
        if (portduino_config.displayRotate) {
            cfg.panel_width = portduino_config.displayHeight; // actual displayable width
            cfg.panel_height = portduino_config.displayWidth; // actual displayable height
        } else {
            cfg.panel_width = portduino_config.displayWidth;   // actual displayable width
            cfg.panel_height = portduino_config.displayHeight; // actual displayable height
        }
        cfg.offset_x = portduino_config.displayOffsetX;             // Panel offset amount in X direction
        cfg.offset_y = portduino_config.displayOffsetY;             // Panel offset amount in Y direction
        cfg.offset_rotation = portduino_config.displayOffsetRotate; // Rotation direction value offset 0~7 (4~7 is mirrored)
        cfg.invert = portduino_config.displayInvert;                // Set to true if the light/darkness of the panel is reversed

        _panel_instance->config(cfg);

        // Configure settings for touch  control.
        if (portduino_config.touchscreenModule) {
            if (portduino_config.touchscreenModule == xpt2046) {
                _touch_instance = new lgfx::Touch_XPT2046;
            } else if (portduino_config.touchscreenModule == stmpe610) {
                _touch_instance = new lgfx::Touch_STMPE610;
            } else if (portduino_config.touchscreenModule == ft5x06) {
                _touch_instance = new lgfx::Touch_FT5x06;
            }
            // Not every module in the config enum has a branch above (gt911 is handled by the
            // color-UI path in tftSetup.cpp), so the pointer can legitimately still be null here.
            if (_touch_instance) {
                auto touch_cfg = _touch_instance->config();

                touch_cfg.pin_cs = portduino_config.touchscreenCS.pin;
                touch_cfg.x_min = 0;
                touch_cfg.x_max = portduino_config.displayHeight - 1;
                touch_cfg.y_min = 0;
                touch_cfg.y_max = portduino_config.displayWidth - 1;
                touch_cfg.pin_int = portduino_config.touchscreenIRQ.pin;
                touch_cfg.bus_shared = true;
                touch_cfg.offset_rotation = portduino_config.touchscreenRotate;
                if (portduino_config.touchscreenI2CAddr != -1) {
                    touch_cfg.i2c_addr = portduino_config.touchscreenI2CAddr;
                } else {
                    touch_cfg.spi_host = portduino_config.touchscreen_spi_dev_int;
                }

                _touch_instance->config(touch_cfg);
                _panel_instance->setTouch(_touch_instance);
            }
        }
#if defined(SDL_h_)
        else if (portduino_config.displayPanel == sdl) {
            // No hardware touch module: feed the SDL window's mouse events through a
            // Touch_sdl so BaseUI's TouchScreenImpl (which calls tft->getTouch()) works,
            // mirroring what the device-ui LGFXConfig driver does. Touch_sdl already reports
            // raw coordinates in final panel-pixel space (see Panel_sdl's own touch_x/y clamp
            // against panel_width/panel_height), so calibrate 1:1 - otherwise LGFX's ITouch
            // default calibration range (0..3600) squashes every click into a small corner.
            _touch_instance = new lgfx::Touch_sdl((lgfx::Panel_sdl *)_panel_instance);
            auto touch_cfg = _touch_instance->config();
            touch_cfg.x_min = 0;
            touch_cfg.x_max = cfg.panel_width - 1;
            touch_cfg.y_min = 0;
            touch_cfg.y_max = cfg.panel_height - 1;
            _touch_instance->config(touch_cfg);
            _panel_instance->setTouch(_touch_instance);
        }
        if (portduino_config.displayPanel == sdl) {
            lgfx::Panel_sdl *sdl_panel_ = (lgfx::Panel_sdl *)_panel_instance;
            sdl_panel_->setup();
            sdl_panel_->addKeyCodeMapping(SDLK_RETURN, SDL_SCANCODE_KP_ENTER);
            // kb_found is set earlier in main.cpp (before CannedMessageModule is constructed) so
            // it's already true here.

            // Whole-number window-scale multiplier for the simulator window, e.g. `Zoom: 2`
            // in config.yaml's Display block. Must be set before init() creates the window.
            if (portduino_config.displayZoom > 1)
                sdl_panel_->setScaling(portduino_config.displayZoom, portduino_config.displayZoom);
        }
#endif
        setPanel(_panel_instance); // Sets the panel to use.
    }
};

static LGFX *tft = nullptr;

#elif defined(HX8357_CS)
#include <LovyanGFX.hpp> // Graphics and font library for HX8357 driver chip

class LGFX : public lgfx::LGFX_Device
{
    lgfx::Panel_HX8357D _panel_instance;
    lgfx::Bus_SPI _bus_instance;
#if defined(USE_XPT2046)
    lgfx::Touch_XPT2046 _touch_instance;
#endif

  public:
    LGFX(void)
    {
        // Panel_HX8357D
        {
            // configure SPI
            auto cfg = _bus_instance.config();

            cfg.spi_host = HX8357_SPI_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = SPI_FREQUENCY; // SPI clock for transmission (up to 80MHz, rounded to the value obtained by dividing
                                            // 80MHz by an integer)
            cfg.freq_read = SPI_READ_FREQUENCY; // SPI clock when receiving
            cfg.spi_3wire = false;              // Set to true if reception is done on the MOSI pin
            cfg.use_lock = true;                // Set to true to use transaction locking
            cfg.dma_channel = SPI_DMA_CH_AUTO;  // SPI_DMA_CH_AUTO; // Set DMA channel to use (0=not use DMA / 1=1ch / 2=ch /
                                                // SPI_DMA_CH_AUTO=auto setting)
            cfg.pin_sclk = HX8357_SCK;          // Set SPI SCLK pin number
            cfg.pin_mosi = HX8357_MOSI;         // Set SPI MOSI pin number
            cfg.pin_miso = HX8357_MISO;         // Set SPI MISO pin number (-1 = disable)
            cfg.pin_dc = HX8357_RS;             // Set SPI DC pin number (-1 = disable)

            _bus_instance.config(cfg);              // applies the set value to the bus.
            _panel_instance.setBus(&_bus_instance); // set the bus on the panel.
        }
        {
            // Set the display panel control.
            auto cfg = _panel_instance.config(); // Gets a structure for display panel settings.

            cfg.pin_cs = HX8357_CS;     // Pin number where CS is connected (-1 = disable)
            cfg.pin_rst = HX8357_RESET; // Pin number where RST is connected  (-1 = disable)
            cfg.pin_busy = HX8357_BUSY; // Pin number where BUSY is connected (-1 = disable)

            cfg.panel_width = TFT_WIDTH;               // actual displayable width
            cfg.panel_height = TFT_HEIGHT;             // actual displayable height
            cfg.offset_x = TFT_OFFSET_X;               // Panel offset amount in X direction
            cfg.offset_y = TFT_OFFSET_Y;               // Panel offset amount in Y direction
            cfg.offset_rotation = TFT_OFFSET_ROTATION; // Rotation direction value offset 0~7 (4~7 is upside down)
            cfg.dummy_read_pixel = 8;                  // Number of bits for dummy read before pixel readout
            cfg.dummy_read_bits = 1;                   // Number of bits for dummy read before non-pixel data read
            cfg.readable = true;                       // Set to true if data can be read
            cfg.invert = TFT_INVERT;                   // Set to true if the light/darkness of the panel is reversed
            cfg.rgb_order = false;                     // Set to true if the panel's red and blue are swapped
            cfg.dlen_16bit = false;
            cfg.bus_shared = true; // If the bus is shared with the SD card, set to true (bus control with drawJpgFile etc.)

            _panel_instance.config(cfg);
        }
#if defined(USE_XPT2046)
        {
            // Configure settings for touch control.
            auto touch_cfg = _touch_instance.config();

            touch_cfg.pin_cs = TOUCH_CS;
            touch_cfg.x_min = 0;
            touch_cfg.x_max = TFT_HEIGHT - 1;
            touch_cfg.y_min = 0;
            touch_cfg.y_max = TFT_WIDTH - 1;
            touch_cfg.pin_int = -1;
            touch_cfg.bus_shared = true;
            touch_cfg.offset_rotation = 1;

            _touch_instance.config(touch_cfg);
            _panel_instance.setTouch(&_touch_instance);
        }
#endif
        setPanel(&_panel_instance);
    }
};

static LGFX *tft = nullptr;

#elif defined(ST7701_CS)
#include <LovyanGFX.hpp> // Graphics and font library for ST7701 driver chip
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>

class PanelInit_ST7701 : public lgfx::Panel_ST7701
{
  public:
    const uint8_t *getInitCommands(uint8_t listno) const override
    {
        // 180 degree hw rotation: vertical flip, horizontal flip
        static constexpr const uint8_t list1[] = {0x36, 1,   0x10,                         // MADCTL for vertical flip
                                                  0xFF, 5,   0x77, 0x01, 0x00, 0x00, 0x10, // Command2 BK0 SEL
                                                  0xC7, 1,   0x04, // SDIR: X-direction Control (Horizontal Flip)
                                                  0xFF, 5,   0x77, 0x01, 0x00, 0x00, 0x00, // Command2 BK0 DIS
                                                  0xFF, 0xFF};
        switch (listno) {
        case 1:
            return list1;
        default:
            return lgfx::Panel_ST7701::getInitCommands(listno);
        }
    }
};

class LGFX : public lgfx::LGFX_Device
{
    PanelInit_ST7701 _panel_instance;
    lgfx::Bus_RGB _bus_instance;
    lgfx::Light_PWM _light_instance;
    lgfx::Touch_FT5x06 _touch_instance;

  public:
    LGFX(void)
    {
        {
            auto cfg = _panel_instance.config();
            cfg.memory_width = 800;
            cfg.memory_height = 480;
            cfg.panel_width = TFT_WIDTH;
            cfg.panel_height = TFT_HEIGHT;
            cfg.offset_x = TFT_OFFSET_X;
            cfg.offset_y = TFT_OFFSET_Y;
            _panel_instance.config(cfg);
        }

        {
            auto cfg = _panel_instance.config_detail();
            cfg.pin_cs = ST7701_CS;
            cfg.pin_sclk = ST7701_SCK;
            cfg.pin_mosi = ST7701_SDA;
            // cfg.use_psram = 1;
            _panel_instance.config_detail(cfg);
        }

        {
            auto cfg = _bus_instance.config();
            cfg.panel = &_panel_instance;
#ifdef SENSECAP_INDICATOR
            cfg.pin_d0 = GPIO_NUM_15; // B0
            cfg.pin_d1 = GPIO_NUM_14; // B1
            cfg.pin_d2 = GPIO_NUM_13; // B2
            cfg.pin_d3 = GPIO_NUM_12; // B3
            cfg.pin_d4 = GPIO_NUM_11; // B4

            cfg.pin_d5 = GPIO_NUM_10; // G0
            cfg.pin_d6 = GPIO_NUM_9;  // G1
            cfg.pin_d7 = GPIO_NUM_8;  // G2
            cfg.pin_d8 = GPIO_NUM_7;  // G3
            cfg.pin_d9 = GPIO_NUM_6;  // G4
            cfg.pin_d10 = GPIO_NUM_5; // G5

            cfg.pin_d11 = GPIO_NUM_4; // R0
            cfg.pin_d12 = GPIO_NUM_3; // R1
            cfg.pin_d13 = GPIO_NUM_2; // R2
            cfg.pin_d14 = GPIO_NUM_1; // R3
            cfg.pin_d15 = GPIO_NUM_0; // R4

            cfg.pin_henable = GPIO_NUM_18;
            cfg.pin_vsync = GPIO_NUM_17;
            cfg.pin_hsync = GPIO_NUM_16;
            cfg.pin_pclk = GPIO_NUM_21;
            cfg.freq_write = 12000000;

            cfg.hsync_polarity = 0;
            cfg.hsync_front_porch = 10;
            cfg.hsync_pulse_width = 8;
            cfg.hsync_back_porch = 50;

            cfg.vsync_polarity = 0;
            cfg.vsync_front_porch = 10;
            cfg.vsync_pulse_width = 8;
            cfg.vsync_back_porch = 20;

            cfg.pclk_active_neg = 0;
            cfg.de_idle_high = 1;
            cfg.pclk_idle_high = 0;
#endif
            _bus_instance.config(cfg);
        }
        _panel_instance.setBus(&_bus_instance);

        {
            auto cfg = _light_instance.config();
            cfg.pin_bl = ST7701_BL;
            _light_instance.config(cfg);
        }
        _panel_instance.light(&_light_instance);

        {
            auto cfg = _touch_instance.config();
            cfg.pin_cs = -1;
            cfg.x_min = 0;
            cfg.x_max = 479;
            cfg.y_min = 0;
            cfg.y_max = 479;
            cfg.pin_int = -1; // don't use SCREEN_TOUCH_INT;
            cfg.pin_rst = SCREEN_TOUCH_RST;
            cfg.bus_shared = true;
            cfg.offset_rotation = TFT_OFFSET_ROTATION;

            cfg.i2c_port = TOUCH_I2C_PORT;
            cfg.i2c_addr = TOUCH_SLAVE_ADDRESS;
            cfg.pin_sda = I2C_SDA;
            cfg.pin_scl = I2C_SCL;
            cfg.freq = 400000;
            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }

        setPanel(&_panel_instance);
    }
};

static LGFX *tft = nullptr;

#elif defined(VARIANT_DISPLAY_DRIVER)
// Board-specific framebuffer backends (class LGFX) can livee in the
// variant files - variant_display.h (declaration) and
// variant_display.cpp (bodies) - so this shared
// file isn't inflated for a single board. It exposes the same surface TFTDisplay
// drives, so the generic `tft = new LGFX;` in connect() works.
#include "variant_display.h"

static LGFX *tft = nullptr;

#endif
#include "SPILock.h"
#include "TFTColorRegions.h"
#include "TFTDisplay.h"
#include "TFTPalette.h"
#include <SPI.h>
#include <algorithm>
#include <cstdlib>

#if BASEUI_HAS_TOUCH_CALIBRATION
#include "NodeDB.h" // uiconfig.calibration_data, shared with device-ui
#if defined(ARCH_ESP32)
#include <esp_task_wdt.h> // fed from the blocking waits in calibrateTouch()
#endif
#endif

#ifdef UNPHONE
#include "unPhone.h"
extern unPhone unphone;
#endif

GpioPin *TFTDisplay::backlightEnable = NULL;

// TFT_eSPI's DMA API is a different shape entirely - it needs an explicit initDMA() and its own
// push calls - so the DMA push below is for the LovyanGFX backends only.
#if defined(ST7735_CS)
#define TFT_HAS_LGFX_DMA_PUSH 0
#else
#define TFT_HAS_LGFX_DMA_PUSH 1
#endif

// -- CO5300 partial-repaint tunables ------------------------------------------------------------
// The t-watch-ultra AMOLED repainted the whole panel every frame for a long time because the
// diff-based partial path corrupted narrow updates - the clock's seconds digits came out as
// scattered wrong-coloured pixels. Isolated on hardware: the corruption is not in the staging or
// the row pairing, it is Bus_SPI::writeBytes() routing small transfers through the SPI FIFO
// registers instead of DMA (see kSpiFifoThresholdBytes and the widening in display()). Keeping
// every push above that threshold fixes it, so the partial path is on by default now.
//
// Two knobs remain, mostly for re-bisecting if this panel misbehaves again:
//   -D CO5300_FORCE_FULL_REPAINT=1  go back to repainting the whole panel every frame.
//   -D CO5300_ROWS_PER_PUSH=1       send single rows instead of row pairs.
//
// Row pairing is not a driver requirement - Panel_AMOLED enforces even x and width only, and writes
// RASET unguarded - but single rows were observed to produce no visible update at all on this
// panel, so 2 is the working default.
#if defined(CO5300_CS)
#ifndef CO5300_FORCE_FULL_REPAINT
#define CO5300_FORCE_FULL_REPAINT 0
#endif
#ifndef CO5300_ROWS_PER_PUSH
#define CO5300_ROWS_PER_PUSH 2
#endif
static_assert(CO5300_ROWS_PER_PUSH == 1 || CO5300_ROWS_PER_PUSH == 2,
              "CO5300_ROWS_PER_PUSH must be 1 or 2: the change scan masks both rows of a push out of "
              "one buffer byte, which only holds for a power-of-two run inside an 8-row page");

// Transfers of this size or smaller go out through the SPI W0 registers rather than DMA - see the
// `length <= 64` branch at the top of Bus_SPI::writeBytes(). Mirrored here because it is a property
// of the bus driver we have to design around, not something we can ask it for.
static constexpr uint32_t kSpiFifoThresholdBytes = 64;
#endif

namespace
{
#ifdef UI_PERF_DEBUG
// Times display() and reports roughly once a second. Build with -D UI_PERF_DEBUG when a drag feels
// laggy. Pairs with the touch-poll cadence reported by TouchScreenBase under the same flag.
//
// Splits the two things that keep frames off the screen, because they need opposite fixes: time
// spent blocked on spiLock (the radio holds it across a transmit) versus time actually converting
// and pushing pixels. Frames-per-report is the third number that matters - a low frame count with
// both timings small means the thread is simply not being run.
struct DisplayFrameTimer {
    const uint32_t startMs = millis();
    uint32_t lockedMs = 0;
    void locked() { lockedMs = millis(); }
    ~DisplayFrameTimer()
    {
        static uint32_t waitTotal = 0, drawTotal = 0, frames = 0, lastReportMs = 0;
        const uint32_t now = millis();
        if (lockedMs == 0)
            lockedMs = startMs;
        waitTotal += lockedMs - startMs;
        drawTotal += now - lockedMs;
        frames++;
        if (now - lastReportMs >= 1000) {
            LOG_INFO("TFT display(): %u frames in %u ms, %u ms draw, %u ms spiLock wait", (unsigned)frames,
                     (unsigned)(now - lastReportMs), (unsigned)(drawTotal / frames), (unsigned)(waitTotal / frames));
            waitTotal = 0;
            drawTotal = 0;
            frames = 0;
            lastReportMs = now;
        }
    }
};
#define UI_PERF_TIME_FRAME() DisplayFrameTimer _uiPerfFrameTimer
#define UI_PERF_FRAME_LOCKED() _uiPerfFrameTimer.locked()
#else
#define UI_PERF_TIME_FRAME() (void)0
#define UI_PERF_FRAME_LOCKED() (void)0
#endif

static constexpr uint8_t kFullRepaintChunkRows = 8;

// Chunk buffers the full repaint alternates between, so converting one chunk overlaps transferring
// the previous one. Two is all the overlap there is to get; more would just cost RAM.
static constexpr uint8_t kFullRepaintChunkSlots = 2;

// Allocate a pixel buffer that SPI DMA can read from, reporting whether that succeeded.
//
// Plain malloc() will not reliably do on ESP32: main.cpp calls heap_caps_malloc_extmem_enable(),
// which sends allocations past a small threshold to PSRAM. Buffers this size land there, which
// both puts them out of easy reach of the DMA engine and makes the per-pixel fill loop pay bus
// time on boards where PSRAM shares its SPI bus with flash.
//
// Falls back to malloc() so a board too tight on internal RAM still comes up - just without DMA,
// which is exactly the behavior it had before.
static uint16_t *tryAllocDmaPixelBuffer(size_t pixels)
{
#if defined(ARCH_ESP32)
    // Leave the internal heap room to breathe. Drivers that come up after the display - the touch
    // controller among them - need internal RAM too, and taking the last of it to make the repaint
    // marginally faster is a bad trade.
    static constexpr size_t kInternalHeapReserve = 24 * 1024;
    const size_t bytes = pixels * sizeof(uint16_t);
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < bytes + kInternalHeapReserve) {
        return nullptr;
    }
    return static_cast<uint16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
#else
    (void)pixels;
    return nullptr;
#endif
}

static uint16_t *allocPixelBuffer(size_t pixels, bool *dmaCapable)
{
    uint16_t *dmaBuf = tryAllocDmaPixelBuffer(pixels);
    if (dmaBuf) {
        *dmaCapable = true;
        return dmaBuf;
    }
    *dmaCapable = false;
    return static_cast<uint16_t *>(malloc(pixels * sizeof(uint16_t)));
}

static inline uint16_t getThemeDefaultOnColor()
{
    return graphics::TFTPalette::White;
}

static inline uint16_t getThemeDefaultOffColor()
{
#if GRAPHICS_TFT_COLORING_ENABLED
    return graphics::getThemeBodyBg();
#else
    return TFT_BLACK;
#endif
}
} // namespace

TFTDisplay::TFTDisplay(uint8_t address, int sda, int scl, OLEDDISPLAY_GEOMETRY geometry, HW_I2C i2cBus)
{
    LOG_DEBUG("TFTDisplay");

#ifdef TFT_BL
    GpioPin *p = new GpioHwPin(TFT_BL);

    if (!TFT_BACKLIGHT_ON) { // Need to invert the pin before hardware
        auto virtPin = new GpioVirtPin();
        new GpioNotTransformer(
            virtPin, p); // We just leave this created object on the heap so it can stay watching virtPin and driving en_gpio
        p = virtPin;
    }
#else
    GpioPin *p = new GpioVirtPin(); // Just simulate a pin
#endif
    backlightEnable = p;

#if ARCH_PORTDUINO
    // setGeometry(g, width, height): the BaseUI framebuffer must match the panel's
    // displayable geometry. When rotated the panel swaps width/height (see the LGFX
    // panel config below), so swap here too. Passing the same dimension twice built a
    // square buffer that under-filled non-square panels (e.g. 240x240 on a 320x240 window).
    if (portduino_config.displayRotate) {
        setGeometry(GEOMETRY_RAWMODE, portduino_config.displayHeight, portduino_config.displayWidth);
    } else {
        setGeometry(GEOMETRY_RAWMODE, portduino_config.displayWidth, portduino_config.displayHeight);
    }

#elif defined(SCREEN_ROTATE)
    setGeometry(GEOMETRY_RAWMODE, TFT_HEIGHT, TFT_WIDTH);
#else
    setGeometry(GEOMETRY_RAWMODE, TFT_WIDTH, TFT_HEIGHT);
#endif
}

TFTDisplay::~TFTDisplay()
{
    // Clean up allocated line pixel buffer to prevent memory leak
    if (linePixelBuffer != nullptr) {
        free(linePixelBuffer);
        linePixelBuffer = nullptr;
    }
    if (repaintChunkBuffer != nullptr) {
        free(repaintChunkBuffer);
        repaintChunkBuffer = nullptr;
    }
    memaudit::set("display", 0);
}

#if defined(USE_ARDUINO_GFX)
// Arduino_GFX backend: display() calls draw16bitBeRGBBitmap() directly and never routes through
// here. These exist only to satisfy the declarations.
void TFTDisplay::pushPixelBlock(int32_t, int32_t, int32_t, int32_t, uint16_t *) {}
void TFTDisplay::beginPixelBatch() {}
void TFTDisplay::endPixelBatch() {}
#else
// Hold the panel's bus transaction open across a run of pushPixelBlock() calls.
//
// Without this, each push ends its own transaction - and ending one waits for the bus to drain, so
// the CPU would stall through every chunk's DMA before starting to build the next. Held open, a
// push returns while its transfer is still in flight, and the caller's next round of pixel
// conversion overlaps it. The panel driver re-issues CS and the address window on every push, so
// batching them changes nothing the panel sees.
//
// Callers that batch must alternate between two source buffers, or they will overwrite the one
// still being transferred.
void TFTDisplay::beginPixelBatch()
{
    tft->startWrite();
}

void TFTDisplay::endPixelBatch()
{
    tft->endWrite(); // ends the transaction, which waits for the last transfer
}

// Send a block of pre-swapped RGB565 pixels to the panel.
//
// Worth going through here rather than calling tft->pushImage() directly: that overload defaults
// to use_dma=false, and Bus_SPI only auto-promotes transfers under 1024 bytes to DMA. Every
// full-width block we push is comfortably over that, so all of them would otherwise take the PIO
// fallback, which walks the SPI FIFO 64 bytes at a time and busy-waits for each one to drain -
// leaving the bus idle in between. Ask for DMA explicitly instead.
//
// Safe to reuse the source buffer as soon as this returns: pushImage() wraps the transfer in
// startWrite()/endWrite(), and endWrite() ends the transaction, which waits on the bus.
void TFTDisplay::pushPixelBlock(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t *data)
{
#if TFT_HAS_LGFX_DMA_PUSH
    if (pixelBuffersAreDmaCapable) {
        tft->pushImageDMA(x, y, w, h, data);
        return;
    }
#endif
    tft->pushImage(x, y, w, h, data);
}
#endif

// Write the buffer to the display memory
void TFTDisplay::display(bool fromBlank)
{
    UI_PERF_TIME_FRAME(); // before the lock: waiting for it is exactly what we need to see
    concurrency::LockGuard g(spiLock);
    UI_PERF_FRAME_LOCKED();

    uint32_t x, y;
    uint32_t y_byteIndex;
    uint8_t y_byteMask;
    uint32_t x_FirstPixelUpdate;
    uint32_t x_LastPixelUpdate;
    bool isset;
    uint16_t colorTftWhite, colorTftBlack;
    bool somethingChanged = false;

    // Theme defaults for non-role pixels.
    const uint16_t defaultOnColor = getThemeDefaultOnColor();
    const uint16_t defaultOffColor = getThemeDefaultOffColor();
    static uint16_t lastDefaultOnColor = 0;
    static uint16_t lastDefaultOffColor = 0;
    static bool haveLastDefaults = false;
    const bool themeDefaultsChanged =
        !haveLastDefaults || (defaultOnColor != lastDefaultOnColor) || (defaultOffColor != lastDefaultOffColor);
    const bool forceFullRepaint = fromBlank || themeDefaultsChanged;

    // If theme defaults changed, reset panel background immediately so stale pixels don't linger.
    if (forceFullRepaint) {
        tft->fillScreen(defaultOffColor);
    }

    colorTftWhite = (defaultOnColor >> 8) | ((defaultOnColor & 0xFF) << 8);
    colorTftBlack = (defaultOffColor >> 8) | ((defaultOffColor & 0xFF) << 8);

#if GRAPHICS_TFT_COLORING_ENABLED
    static uint32_t lastColorFrameSignature = 0;
    const bool hasColorRegions = graphics::getTFTColorRegionCount() > 0;
    const uint32_t colorFrameSignature = graphics::getTFTColorFrameSignature();
#if defined(CO5300_CS) && CO5300_FORCE_FULL_REPAINT
    // Opt-in escape hatch: repaint the whole panel every frame on the t-watch-ultra AMOLED.
    //
    // This used to be unconditional, to work around narrow partial updates corrupting. That cause is
    // understood and fixed now (see the widening in the partial path below), so this is off by
    // default. Costs ~410 x 502 x 2 B over QSPI every frame, roughly 40-50 ms, whether one pixel
    // changed or all of them.
    const bool forceFullColorRepaint = true;
#else
    const bool forceFullColorRepaint = forceFullRepaint || (colorFrameSignature != lastColorFrameSignature);
#endif

    // When region roles/layout changed, color can differ even with identical monochrome glyph bits.
    // Repaint full frame only for those frames, then return to diff-based updates.
    if (forceFullColorRepaint) {
        // Hold the bus open for the whole repaint and alternate between the two chunk slots, so
        // each chunk's pixel conversion runs against the previous chunk's transfer rather than
        // after it. See beginPixelBatch().
        beginPixelBatch();
        uint8_t chunkSlot = 0;
        for (uint32_t yStart = 0; yStart < displayHeight; yStart += kFullRepaintChunkRows) {
            const uint32_t rowsThisChunk = min<uint32_t>(kFullRepaintChunkRows, displayHeight - yStart);
            uint16_t *const chunkBuffer = repaintChunkBuffer + ((size_t)chunkSlot * displayWidth * kFullRepaintChunkRows);
            chunkSlot = (uint8_t)((chunkSlot + 1) % chunkBufferSlots);
            for (uint32_t row = 0; row < rowsThisChunk; row++) {
                y = yStart + row;
                y_byteIndex = (y / 8) * displayWidth;
                y_byteMask = (1 << (y & 7));

                uint16_t *chunkRow = chunkBuffer + (row * displayWidth);

                // Step 1: fill the whole row with the default colors. No per-pixel
                // region scan, so background pixels (the bulk of the screen) are O(1).
                for (x = 0; x < displayWidth; x++) {
                    isset = (buffer[x + y_byteIndex] & y_byteMask) != 0;
                    chunkRow[x] = isset ? colorTftWhite : colorTftBlack;
                }

                // Step 2: overprint each region overlapping this row, applied in
                // ascending index order so the highest-index region wins (matches
                // resolveTFTColorPixel precedence). Only region-covered pixels are
                // re-touched, so total cost is ~screen + sum of region spans.
                if (hasColorRegions) {
                    graphics::beginTFTColorRow(static_cast<int16_t>(y));
                    for (uint8_t k = 0; k < graphics::tftColorRowCount; k++) {
                        const graphics::TFTColorRegion &r = graphics::colorRegions[graphics::tftColorRowRegions[k]];
                        int32_t xs = r.x > 0 ? r.x : 0;
                        int32_t xe = r.x + r.width;
                        if (xe > (int32_t)displayWidth)
                            xe = (int32_t)displayWidth;
                        for (int32_t xx = xs; xx < xe; xx++) {
                            isset = (buffer[xx + y_byteIndex] & y_byteMask) != 0;
                            chunkRow[xx] = isset ? r.onColorBe : r.offColorBe;
                        }
                    }
                }
            }
#if defined(USE_ARDUINO_GFX)
            tft->draw16bitBeRGBBitmap(0, yStart, chunkBuffer, displayWidth, rowsThisChunk);
#else
            pushPixelBlock(0, yStart, displayWidth, rowsThisChunk, chunkBuffer);
#endif
        }
        endPixelBatch();

        memcpy(buffer_back, buffer, displayBufferSize);
        lastColorFrameSignature = colorFrameSignature;
        haveLastDefaults = true;
        lastDefaultOnColor = defaultOnColor;
        lastDefaultOffColor = defaultOffColor;
        graphics::clearTFTColorRegions();
        return;
    }
#endif

    // Rows sent per push. The CO5300 goes out a row pair at a time; every other panel pushes single
    // rows. Pairing is not a driver requirement - Panel_AMOLED enforces even x and width only, and
    // writes RASET unguarded - so CO5300_ROWS_PER_PUSH=1 is the control case when bisecting.
    //
    // The loop below steps by whole pushes rather than by single rows. That is what stops a changed
    // pair being sent twice - scanning per row and pushing the pair containing it did the work, and
    // paid the address-window setup, once for each row of the pair. It also makes the change scan
    // cheaper: both rows of a pair always live in the same buffer byte, so one mask covers them.
#if defined(CO5300_CS)
    constexpr uint32_t kRowsPerPush = CO5300_ROWS_PER_PUSH;
#else
    constexpr uint32_t kRowsPerPush = 1;
#endif
    const uint8_t rowsPerPushBits = (uint8_t)((1U << kRowsPerPush) - 1U);

#if defined(CO5300_CS)
    // Narrowest push, in pixels, that still reaches the panel as a DMA transfer. Derived rather than
    // hardcoded so it stays correct if kRowsPerPush changes: at two rows the floor is 18 px (72 B),
    // at one row it is 34 px (68 B). Rounded up to even to preserve the even-width rule.
    constexpr uint32_t kMinPushPixels = ((kSpiFifoThresholdBytes / (kRowsPerPush * sizeof(uint16_t))) + 2) & ~1U;
    static_assert(kMinPushPixels * kRowsPerPush * sizeof(uint16_t) > kSpiFifoThresholdBytes,
                  "widened span must exceed the FIFO threshold, or the push still bypasses DMA");
#endif

#if defined(CO5300_PARTIAL_DEBUG)
    uint32_t dbgPushes = 0;
    uint32_t dbgRejects = 0;
    static bool dbgGeometryLogged = false;
    if (!dbgGeometryLogged) {
        dbgGeometryLogged = true;
        // displayWidth/Height are OLEDDisplay's geometry; tft->width()/height() are the panel's own,
        // after rotation and offsets. If they disagree, the even-x/even-w reasoning is built on the
        // wrong bounds and the clip inside LGFXBase::pushImage() can shave w to an odd number.
        LOG_DEBUG("CO5300 geometry: displayWidth=%u displayHeight=%u panelW=%d panelH=%d rowsPerPush=%u", (unsigned)displayWidth,
                  (unsigned)displayHeight, (int)tft->width(), (int)tft->height(), (unsigned)kRowsPerPush);
    }
#endif

    y = 0;
    while (y < displayHeight) {
        y_byteIndex = (y / 8) * displayWidth;
        y_byteMask = (uint8_t)(rowsPerPushBits << (y & 7)); // every row this push will cover

        // Step 1: Do a quick scan of 8 rows together. This allows fast-forwarding over unchanged screen areas.
        if ((y & 7) == 0) {
            if (!forceFullRepaint) {
                for (x = 0; x < displayWidth; x++) {
                    if (buffer[x + y_byteIndex] != buffer_back[x + y_byteIndex])
                        break;
                }
            } else {
                for (x = 0; x < displayWidth; x++) {
                    if (buffer[x + y_byteIndex] != 0)
                        break;
                }
            }
            if (x >= displayWidth) {
                // No changed pixels found in these 8 rows, fast-forward to the next 8
                y = y + 8;
                continue;
            }
        }

        // Step 2: Scan the rows this push covers for the changed span (first and last changed
        // pixel). Compares masked bytes rather than a single bit, so a change on any covered row
        // counts - a bool would collapse "row 0 changed" and "row 1 changed" into the same value.
        uint32_t x_FirstChanged = 0;
        for (x_FirstChanged = 0; x_FirstChanged < displayWidth; x_FirstChanged++) {
            const uint8_t bits = buffer[x_FirstChanged + y_byteIndex] & y_byteMask;

            if (!forceFullRepaint) {
                // get src pixel in the page based ordering the OLED lib uses
                if (bits != (buffer_back[x_FirstChanged + y_byteIndex] & y_byteMask)) {
                    break;
                }
            } else if (bits) {
                break;
            }
        }

        // Did we find a pixel that needs updating on these rows?
        if (x_FirstChanged < displayWidth) {
            uint32_t x_LastChanged = displayWidth - 1;
            while (x_LastChanged > x_FirstChanged) {
                const uint8_t bits = buffer[x_LastChanged + y_byteIndex] & y_byteMask;
                if (!forceFullRepaint) {
                    if (bits != (buffer_back[x_LastChanged + y_byteIndex] & y_byteMask)) {
                        break;
                    }
                } else if (bits) {
                    break;
                }
                x_LastChanged--;
            }

            // Align the first pixel for update to an even number so the total alignment of
            // the data will be at 32-bit boundary, which is required by GDMA SPI transfers.
            x_FirstPixelUpdate = x_FirstChanged & ~1U;
            x_LastPixelUpdate = x_LastChanged | 1U;
            if (x_LastPixelUpdate >= displayWidth) {
                x_LastPixelUpdate = displayWidth - 1;
            }

#if defined(CO5300_CS)
            // Widen narrow spans so the transfer clears Bus_SPI's FIFO threshold.
            //
            // Bus_SPI::writeBytes() sends anything <= kSpiFifoThresholdBytes straight through the
            // SPI W0 registers instead of setting up DMA. This panel does not render those writes:
            // measured on hardware, a clock-seconds update pushed 4 to 48 bytes and came out as
            // scattered wrong-coloured pixels, while the same content widened past the threshold
            // renders correctly. The full-repaint path never hit this because its chunks are
            // 6560 bytes and always take DMA - which is why repainting the whole panel every frame
            // looked like the only option for so long.
            //
            // Widening is safe: the extra pixels are rendered from the live buffer below, exactly
            // as the changed ones are, so they carry correct content rather than stale data. Cost
            // is at most 72 bytes per push against 412 KB for a full repaint.
            if ((x_LastPixelUpdate - x_FirstPixelUpdate + 1) < kMinPushPixels) {
                uint32_t want = kMinPushPixels;
                if (want > displayWidth)
                    want = displayWidth;
                // Grow right first, then take whatever is still missing from the left. Keeping the
                // start even and the end odd preserves the even-x/even-width rule Panel_AMOLED
                // enforces, and the 32-bit source alignment GDMA needs.
                uint32_t last = x_FirstPixelUpdate + want - 1;
                if (last > displayWidth - 1)
                    last = displayWidth - 1;
                uint32_t first = (last + 1 >= want) ? (last + 1 - want) : 0;
                x_FirstPixelUpdate = first & ~1U;
                x_LastPixelUpdate = last | 1U;
                if (x_LastPixelUpdate >= displayWidth)
                    x_LastPixelUpdate = displayWidth - 1;
            }
#endif

            const uint32_t spanWidth = x_LastPixelUpdate - x_FirstPixelUpdate + 1;

            // Step 3: Copy the changed span of every covered row into the pixel line buffer, rows
            // laid out back to back so the push below is one contiguous block.
            for (uint32_t row = 0; row < kRowsPerPush; row++) {
                const uint32_t rowY = y + row;
                const uint32_t rowByteIndex = (rowY / 8) * displayWidth;
                const uint8_t rowBitMask = (uint8_t)(1U << (rowY & 7));
                uint16_t *const dst = &linePixelBuffer[x_FirstPixelUpdate + (row * spanWidth)];
#if GRAPHICS_TFT_COLORING_ENABLED
                // Re-cached per row: resolveTFTColorPixelRow() resolves against the regions cached
                // for whichever row was last passed here, so colouring a second row off the first
                // row's cache would pick the wrong regions wherever a region edge falls between the
                // two.
                if (hasColorRegions)
                    graphics::beginTFTColorRow(static_cast<int16_t>(rowY));
#endif
                for (x = x_FirstPixelUpdate; x <= x_LastPixelUpdate; x++) {
                    isset = buffer[x + rowByteIndex] & rowBitMask;
#if GRAPHICS_TFT_COLORING_ENABLED
                    if (hasColorRegions) {
                        dst[x - x_FirstPixelUpdate] =
                            graphics::resolveTFTColorPixelRow(static_cast<int16_t>(x), isset, colorTftWhite, colorTftBlack);
                    } else {
                        dst[x - x_FirstPixelUpdate] = isset ? colorTftWhite : colorTftBlack;
                    }
#else
                    dst[x - x_FirstPixelUpdate] = isset ? colorTftWhite : colorTftBlack;
#endif
                }
            }

#if defined(USE_ARDUINO_GFX)
            tft->draw16bitBeRGBBitmap(x_FirstPixelUpdate, y, &linePixelBuffer[x_FirstPixelUpdate], spanWidth, kRowsPerPush);
#else
            // Step 4: Send the changed pixels on these rows to the screen as a single block transfer.
            // This function accepts pixel data MSB first so it can dump the memory straight out the SPI port.
#if defined(CO5300_PARTIAL_DEBUG)
            // Panel_AMOLED::writeImage() and ::setWindow() both silently return when x or w is odd,
            // so log what they will actually see. dbgRejects counting above zero means the push never
            // reached the panel - and because buffer_back is updated regardless, that pixel is lost
            // until the next full repaint.
            dbgPushes++;
            if ((x_FirstPixelUpdate & 1U) || (spanWidth & 1U)) {
                if (dbgRejects < 4)
                    LOG_WARN("CO5300 odd push: x=%u y=%u w=%u h=%u (xF=%u xL=%u)", (unsigned)x_FirstPixelUpdate, (unsigned)y,
                             (unsigned)spanWidth, (unsigned)kRowsPerPush, (unsigned)x_FirstChanged, (unsigned)x_LastChanged);
                dbgRejects++;
            } else if (dbgPushes <= 3) {
                LOG_DEBUG("CO5300 push: x=%u y=%u w=%u h=%u", (unsigned)x_FirstPixelUpdate, (unsigned)y, (unsigned)spanWidth,
                          (unsigned)kRowsPerPush);
            }
#endif
            pushPixelBlock(x_FirstPixelUpdate, y, spanWidth, kRowsPerPush, &linePixelBuffer[x_FirstPixelUpdate]);
#endif
            somethingChanged = true;
        }
        y += kRowsPerPush;
    }
    // Copy the Buffer to the Back Buffer
    if (somethingChanged)
        memcpy(buffer_back, buffer, displayBufferSize);

#if GRAPHICS_TFT_COLORING_ENABLED
    lastColorFrameSignature = colorFrameSignature;
#endif
    haveLastDefaults = true;
    lastDefaultOnColor = defaultOnColor;
    lastDefaultOffColor = defaultOffColor;
    graphics::clearTFTColorRegions();
}

void TFTDisplay::sdlLoop()
{
#if defined(SDL_h_)
    static int lastPressed = 0;
    static int shuttingDown = false;
    if (portduino_config.displayPanel == sdl) {
        lgfx::Panel_sdl *sdl_panel_ = (lgfx::Panel_sdl *)tft->_panel_instance;
        if (sdl_panel_->loop() && !shuttingDown) {
            LOG_WARN("Window Closed");
            shuttingDown = true;
            InputEvent event = {.inputEvent = (input_broker_event)INPUT_BROKER_SHUTDOWN, .kbchar = 0, .touchX = 0, .touchY = 0};
            inputBroker->injectInputEvent(&event);
        }

        // Drain characters/backspace/escape/tab typed on the physical keyboard, queued by the
        // SDL event thread in Panel_sdl. Arrow keys and Enter are handled below via gpio state.
        lgfx::Panel_sdl::QueuedKeyEvent queuedKey;
        while (lgfx::Panel_sdl::dequeueKeyEvent(&queuedKey)) {
            InputEvent event = {.inputEvent = queuedKey.inputEvent, .kbchar = queuedKey.kbchar, .touchX = 0, .touchY = 0};
            inputBroker->injectInputEvent(&event);
        }

        // Enter/Select is timed: a quick tap emits SELECT, holding past the threshold emits
        // SELECT_LONG (matching the physical user button, e.g. hold to open the games menu).
        static uint32_t enterPressedAt = 0;
        static bool enterLongSent = false;
        constexpr uint32_t kEnterLongPressMs = 500;
        if (!sdl_panel_->gpio_in(SDL_SCANCODE_KP_ENTER)) {
            lastPressed = SDL_SCANCODE_KP_ENTER; // suppress directional keys while Enter is held
            if (enterPressedAt == 0) {
                enterPressedAt = millis();
                enterLongSent = false;
            } else if (!enterLongSent && (millis() - enterPressedAt) >= kEnterLongPressMs) {
                enterLongSent = true;
                InputEvent event = {
                    .inputEvent = (input_broker_event)INPUT_BROKER_SELECT_LONG, .kbchar = 0, .touchX = 0, .touchY = 0};
                inputBroker->injectInputEvent(&event);
            }
            return;
        } else if (enterPressedAt != 0) {
            // Released: emit the short SELECT only if we didn't already fire the long press.
            bool wasLong = enterLongSent;
            enterPressedAt = 0;
            enterLongSent = false;
            lastPressed = 0;
            if (!wasLong) {
                InputEvent event = {.inputEvent = (input_broker_event)INPUT_BROKER_SELECT, .kbchar = 0, .touchX = 0, .touchY = 0};
                inputBroker->injectInputEvent(&event);
            }
            return;
        }

        // Directional keys: fire once on press, then auto-repeat while held - same 500ms initial
        // delay / 300ms repeat cadence as TrackballInterruptBase::runOnce()'s real-hardware
        // directionDetected path, so keyboard nav on this simulator feels like a real trackball.
        static uint32_t directionPressedAt = 0;
        static uint32_t lastDirectionRepeatAt = 0;
        constexpr uint32_t kDirectionRepeatDelayMs = 500;
        constexpr uint32_t kDirectionRepeatIntervalMs = 300;

        int heldGpio = 0;
        input_broker_event heldEvent = INPUT_BROKER_NONE;
        if (!sdl_panel_->gpio_in(37)) {
            heldGpio = 37;
            heldEvent = (input_broker_event)INPUT_BROKER_RIGHT;
        } else if (!sdl_panel_->gpio_in(36)) {
            heldGpio = 36;
            heldEvent = (input_broker_event)INPUT_BROKER_UP;
        } else if (!sdl_panel_->gpio_in(38)) {
            heldGpio = 38;
            heldEvent = (input_broker_event)INPUT_BROKER_DOWN;
        } else if (!sdl_panel_->gpio_in(39)) {
            heldGpio = 39;
            heldEvent = (input_broker_event)INPUT_BROKER_LEFT;
        }

        if (heldGpio == 0) {
            lastPressed = 0;
            directionPressedAt = 0;
            return;
        }

        if (lastPressed != heldGpio) {
            // New press (or switched direction without releasing first).
            lastPressed = heldGpio;
            directionPressedAt = millis();
            lastDirectionRepeatAt = 0;
            InputEvent event = {.inputEvent = heldEvent, .kbchar = 0, .touchX = 0, .touchY = 0};
            inputBroker->injectInputEvent(&event);
            return;
        }

        uint32_t heldDuration = millis() - directionPressedAt;
        if (heldDuration >= kDirectionRepeatDelayMs &&
            (lastDirectionRepeatAt == 0 || millis() - lastDirectionRepeatAt >= kDirectionRepeatIntervalMs)) {
            lastDirectionRepeatAt = millis();
            InputEvent event = {.inputEvent = heldEvent, .kbchar = 0, .touchX = 0, .touchY = 0};
            inputBroker->injectInputEvent(&event);
        }
    }
#endif
}

int TFTDisplay::heldXZone()
{
#if defined(SDL_h_)
    if (portduino_config.displayPanel != sdl)
        return 0;
    // Same GPIO numbers/active-low convention as the debounced path in sdlLoop() above.
    if (!lgfx::Panel_sdl::gpio_in(39)) // LEFT
        return -1;
    if (!lgfx::Panel_sdl::gpio_in(37)) // RIGHT
        return 1;
#endif
    return 0;
}

// Send a command to the display (low level function)
void TFTDisplay::sendCommand(uint8_t com)
{
    // handle display on/off directly
    switch (com) {
    case DISPLAYON: {
        LOG_DEBUG("Display on");
#if defined(TFT_NV3001B)
        // DISPLAYOFF cuts the panel rail, so the controller loses its configuration and sleep-out
        // alone cannot bring it back. Restore the rail, let it settle, then re-run the init sequence.
        digitalWrite(VTFT_CTRL, TFT_EN_ON);
        delay(10);
        if (!tft->begin(SPI_FREQUENCY)) {
            // Nothing below this point can reach the panel, so skip the wake instead of lighting
            // the backlight and repainting over a bus that did not come up.
            LOG_ERROR("NV3001B re-init failed on wake");
            break;
        }
#endif
        backlightEnable->set(true);
#if ARCH_PORTDUINO
        display(true);
        if (portduino_config.displayBacklight.pin > 0)
            digitalWrite(portduino_config.displayBacklight.pin, TFT_BACKLIGHT_ON);
#elif defined(USE_ARDUINO_GFX)
        tft->displayOn();
#elif !defined(RAK14014) && !defined(M5STACK) && !defined(UNPHONE) && !defined(HELTEC_MESH_NODE_T096) &&                         \
    !defined(HELTEC_MESH_NODE_T1)
        tft->wakeup();
        tft->powerSaveOff();
#endif

#if defined(TFT_NV3001B)
        // Re-init left display RAM undefined, so repaint in full rather than diff against a
        // buffer that no longer describes the panel.
        display(true);
#endif

#if defined(VTFT_CTRL) && !defined(TFT_NV3001B) // NV3001B panels already powered the rail above
        digitalWrite(VTFT_CTRL, LOW);
#endif
#ifdef UNPHONE
        unphone.backlight(true); // using unPhone library
#endif
#if defined(RAK14014) || defined(HELTEC_MESH_NODE_T096) || defined(HELTEC_MESH_NODE_T1)
#elif !defined(M5STACK) && !defined(ST7789_CS) &&                                                                                \
    !defined(USE_ARDUINO_GFX) // T-Deck gets brightness set in Screen.cpp in the handleSetOn function
        tft->setBrightness(172);
#endif
        break;
    }
    case DISPLAYOFF: {
        LOG_DEBUG("Display off");
        backlightEnable->set(false);
#if ARCH_PORTDUINO
        tft->clear();
        if (portduino_config.displayBacklight.pin > 0)
            digitalWrite(portduino_config.displayBacklight.pin, !TFT_BACKLIGHT_ON);
#elif defined(USE_ARDUINO_GFX)
        tft->displayOff();
#elif !defined(RAK14014) && !defined(M5STACK) && !defined(UNPHONE) && !defined(HELTEC_MESH_NODE_T096) &&                         \
    !defined(HELTEC_MESH_NODE_T1)
        tft->sleep();
        tft->powerSaveOn();
#endif

#ifdef VTFT_CTRL
        digitalWrite(VTFT_CTRL, HIGH);
#endif
#ifdef UNPHONE
        unphone.backlight(false); // using unPhone library
#endif
#if defined(RAK14014) || defined(HELTEC_MESH_NODE_T096) || defined(HELTEC_MESH_NODE_T1)
#elif !defined(M5STACK) && !defined(USE_ARDUINO_GFX)
        tft->setBrightness(0);
#endif
        break;
    }
    default:
        break;
    }

    // Drop all other commands to device (we just update the buffer)
}

void TFTDisplay::setDisplayBrightness(uint8_t _brightness)
{
#if defined(RAK14014) || defined(HELTEC_MESH_NODE_T096) || defined(HELTEC_MESH_NODE_T1)
    // todo
#elif !defined(USE_ARDUINO_GFX)
    tft->setBrightness(_brightness);
    LOG_DEBUG("Brightness is set to value: %i ", _brightness);
#endif
}

void TFTDisplay::flipScreenVertically()
{
#if defined(T_WATCH_S3)
    LOG_DEBUG("Flip TFT vertically"); // T-Watch S3 right-handed orientation
    tft->setRotation(0);
#endif
}

bool TFTDisplay::hasTouch(void)
{
#ifdef RAK14014
    return true;
#elif !defined(M5STACK) && !defined(USE_ARDUINO_GFX) && !defined(HELTEC_MESH_NODE_T096) && !defined(HELTEC_MESH_NODE_T1)
    return tft->touch() != nullptr;
#else
    return false;
#endif
}

bool TFTDisplay::getTouch(int16_t *x, int16_t *y)
{
#ifdef RAK14014
    if (_rak14014_touch_int) {
        _rak14014_touch_int = false;
        /* The X and Y axes have to be switched */
        *y = ft6336u.read_touch1_x();
        *x = TFT_HEIGHT - ft6336u.read_touch1_y();
        return true;
    } else {
        return false;
    }
#elif !defined(M5STACK) && !defined(USE_ARDUINO_GFX) && !defined(HELTEC_MESH_NODE_T096) && !defined(HELTEC_MESH_NODE_T1)
    return tft->getTouch(x, y);
#else
    return false;
#endif
}

#if BASEUI_HAS_TOUCH_CALIBRATION

// -- Touch calibration --------------------------------------------------------------------------
// A reimplementation of LovyanGFX's LGFX_Device::calibrate_touch(), which is what device-ui runs
// from its calibration screen. Two reasons not to just call it:
//
//  1. It waits for four taps with no upper bound. device-ui can afford that because it calibrates
//     from the LVGL task while the mesh loop keeps running; BaseUI has no such split - menu
//     callbacks run on the main cooperative thread, so an unbounded wait there stalls the 90s ESP32
//     app watchdog and reboots the device out from under the user.
//  2. Its prompt is drawn by the library. Doing our own leaves room to say which corner is next.
//
// The sampling geometry is kept identical - the rotation cancellation, the corner order, the paired
// -read noise filter - because that is what makes the resulting parameters interchangeable with the
// ones device-ui stores in the very same uiconfig field.

// Consecutive raw readings this far apart are treated as noise from a finger still settling.
static constexpr int32_t kCalibrationRawError = 20;

// How long a single corner waits for a usable tap before the run is abandoned. Long enough not to
// rush anyone, short enough that a half-finished calibration recovers on its own rather than
// sitting on the watchdog's doorstep.
static constexpr uint32_t kCalibrationCornerTimeoutMs = 25000;

// Nothing else services the ESP32 app watchdog while these loops block, so do it here.
static void calibrationDelay(uint32_t ms)
{
    delay(ms);
#ifdef ARCH_ESP32
    esp_task_wdt_reset();
#endif
}

// Waits for the panel to report a touch (or the absence of one). False on timeout.
static bool waitForRawTouch(bool wantTouch, uint32_t deadline, lgfx::touch_point_t *tp)
{
    lgfx::touch_point_t scratch;
    while ((int32_t)(millis() - deadline) < 0) {
        const bool touched = tft->getTouchRaw(tp ? tp : &scratch, 1) != 0;
        if (touched == wantTouch)
            return true;
        calibrationDelay(2);
    }
    return false;
}

// The crosshair the user is asked to tap. LovyanGFX draws its own, but draw_calibrate_point() is
// protected, so this rebuilds the same shape out of public primitives.
static void drawCalibrationTarget(int32_t x, int32_t y, int32_t r, uint16_t fg, uint16_t bg)
{
    tft->fillRect(x - r, y - r, r * 2 + 1, r * 2 + 1, bg);
    if (fg == bg)
        return;
    const int32_t w = std::max<int32_t>(1, r >> 3);
    tft->fillRect(x - w, y - r, w * 2 + 1, r * 2 + 1, fg);
    tft->fillRect(x - r, y - w, r * 2 + 1, w * 2 + 1, fg);
}

bool TFTDisplay::calibrateTouch(uint16_t parameters[8])
{
    if (!tft || !hasTouch() || !parameters)
        return false;

    // Held for the whole run, as device-ui does: the routine drives the panel and the touch
    // controller directly, outside the framebuffer flush that normally owns this lock.
    concurrency::LockGuard g(spiLock);

    // uint16_t, not uint32_t: LovyanGFX picks its colour conversion off the argument's type, and a
    // 565 value handed over as uint32_t would be read back as RGB888.
    const uint16_t bg = tft->color565(0, 0, 0);
    const uint16_t fg = tft->color565(0xFF, 0xFF, 0xFF);

    const uint_fast8_t rot = tft->getRotation();
    const uint_fast8_t panelRot = tft->panel()->config().offset_rotation;
    const uint_fast8_t touchRot = tft->touch()->config().offset_rotation;

    // The prompt goes down first, while the panel is still in the orientation the user holds the
    // device in - on a landscape board like the T-Deck the sampling orientation below is portrait,
    // and text drawn there comes out sideways. Nothing clears the panel afterwards, so it stays put
    // and stays readable while the corners are tapped.
    tft->fillScreen(bg);
    tft->setTextColor(fg, bg);
    tft->setTextSize(1);
    tft->setTextDatum(lgfx::textdatum_t::middle_center);
    tft->drawString("Tap the centre of each marker", tft->width() >> 1, tft->height() >> 1);
    tft->setTextDatum(lgfx::textdatum_t::top_left);

    // Cancel both rotation offsets so the corners are sampled in the controller's own orientation,
    // which is the frame setCalibrate() below expects to read them back in.
    tft->setRotation(((touchRot ^ panelRot) & 4) | (-(touchRot + panelRot) & 3));

    const int32_t width = tft->width();
    const int32_t height = tft->height();
    const int32_t markerRadius = std::max<int32_t>(4, std::max(width, height) >> 4);

    uint16_t sampled[8] = {0};
    bool ok = true;

    for (int i = 0; i < 4 && ok; ++i) {
        const int32_t px = (width - 1) * ((i >> 1) & 1);
        const int32_t py = (height - 1) * (i & 1);
        drawCalibrationTarget(px, py, markerRadius, fg, bg);

        const uint32_t deadline = millis() + kCalibrationCornerTimeoutMs;

        // Whatever is still under the finger from the previous corner is not a tap on this one.
        ok = waitForRawTouch(false, deadline, nullptr);

        // Sixteen readings, taken as eight agreeing pairs, averaged. The pairing is what rejects
        // the smeared coordinates a capacitive controller reports while a finger is still landing.
        int32_t sumX = 0, sumY = 0;
        for (int j = 0; j < 8 && ok; ++j) {
            lgfx::touch_point_t tp, tp2;
            for (;;) {
                if (!waitForRawTouch(true, deadline, &tp)) {
                    ok = false;
                    break;
                }
                calibrationDelay(10);
                if (tft->getTouchRaw(&tp2, 1) && std::abs(tp.x - tp2.x) <= kCalibrationRawError &&
                    std::abs(tp.y - tp2.y) <= kCalibrationRawError)
                    break;
                if ((int32_t)(millis() - deadline) >= 0) {
                    ok = false;
                    break;
                }
            }
            sumX += tp.x + tp2.x;
            sumY += tp.y + tp2.y;
        }

        sampled[i * 2] = sumX >> 4;
        sampled[i * 2 + 1] = sumY >> 4;
        drawCalibrationTarget(px, py, markerRadius, bg, bg);
    }

    // Four taps that all landed in much the same place would produce a mapping that makes the panel
    // unusable - and on a touch-only board there would then be no way back into this menu to redo
    // it. Raw units are not panel pixels (resistive controllers count to 4095), so this is only a
    // floor against the degenerate case, not a real accuracy check.
    if (ok) {
        const auto cornerGap = [&sampled](int a, int b) {
            const int32_t dx = (int32_t)sampled[a * 2] - (int32_t)sampled[b * 2];
            const int32_t dy = (int32_t)sampled[a * 2 + 1] - (int32_t)sampled[b * 2 + 1];
            return dx * dx + dy * dy;
        };
        const int32_t minGap = std::min(width, height) / 4;
        if (cornerGap(0, 1) < minGap * minGap || cornerGap(0, 2) < minGap * minGap) {
            LOG_WARN("Touch calibration rejected: corners too close together");
            ok = false;
        }
    }

    tft->setRotation(rot);

    if (!ok) {
        LOG_WARN("Touch calibration abandoned, keeping previous settings");
        return false;
    }

    tft->setTouchCalibrate(sampled);
    memcpy(parameters, sampled, sizeof(sampled));
    LOG_INFO("Touch calibration: {%u, %u, %u, %u, %u, %u, %u, %u}", sampled[0], sampled[1], sampled[2], sampled[3], sampled[4],
             sampled[5], sampled[6], sampled[7]);
    return true;
}

void TFTDisplay::applyTouchCalibration(const uint16_t parameters[8])
{
    if (!tft || !hasTouch() || !parameters)
        return;

    // setTouchCalibrate() only recomputes the panel's affine transform - no bus traffic, so no lock
    // needed, which is what lets connect() call this while already holding spiLock.
    uint16_t writable[8];
    memcpy(writable, parameters, sizeof(writable));
    tft->setTouchCalibrate(writable);
}

void TFTDisplay::clearTouchCalibration(void)
{
    if (!tft || !hasTouch())
        return;

    // Rebuilds the mapping from the touch driver's configured x/y range, i.e. exactly what the panel
    // had at boot before any stored calibration was applied.
    tft->panel()->touchCalibrate();
}

#endif // BASEUI_HAS_TOUCH_CALIBRATION

void TFTDisplay::setDetected(uint8_t detected)
{
    (void)detected;
}

// Connect to the display
bool TFTDisplay::connect()
{
    concurrency::LockGuard g(spiLock);
    LOG_INFO("Do TFT init");
    // connect() re-runs on every display wake on variants whose handleSetOn() re-inits
    // the UI (see the gates in Screen::handleSetOn); construct the driver exactly once.
    if (!tft) {
#if defined(RAK14014) || defined(HELTEC_MESH_NODE_T096) || defined(HELTEC_MESH_NODE_T1)
        tft = new TFT_eSPI;
#elif defined(USE_ARDUINO_GFX)
        Arduino_DataBus *bus =
            new Arduino_ESP32SPI(TFT_DC, TFT_CS, 38 /* SCK */, 21 /* MOSI */, GFX_NOT_DEFINED /* MISO */, HSPI /* spi_num */);
        tft = new Arduino_NV3007(bus, 40, 0 /* rotation */, false /* IPS */, 142 /* width */, 428 /* height */,
                                 12 /* col offset 1 */, 0 /* row offset 1 */, 14 /* col offset 2 */, 0 /* row offset 2 */,
                                 nv3007_279_init_operations, sizeof(nv3007_279_init_operations));
#elif defined(TFT_NV3001B)
        // The Heltec RC panels all use the same controller and differ only in how the bus is wired.
#if defined(HELTEC_RC52)
        // nRF52840: the panel sits on SPI1, clear of the LoRa radio on SPI0.
        Arduino_DataBus *bus = new Arduino_HWSPI(TFT_RS, TFT_CS, &SPI1, true /* is_shared_interface */);
#elif defined(HELTEC_RCC6)
        // ESP32-C6: the panel shares pins with the LoRa host, so bit-bang it rather than claim the peripheral.
        Arduino_DataBus *bus = new Arduino_SWSPI(TFT_RS, TFT_CS, TFT_SCL, TFT_SDA, GFX_NOT_DEFINED /* MISO */);
#else
        // ESP32-S3: keep the panel off the LoRa FSPI host, since Arduino_GFX reconfigures whichever bus it is handed.
        Arduino_DataBus *bus =
            new Arduino_ESP32SPI(TFT_RS, TFT_CS, TFT_SCL, TFT_SDA, GFX_NOT_DEFINED /* MISO */, HSPI /* spi_num */);
#endif
        tft = new Arduino_NV3001B(bus, TFT_RST, 3 /* rotation */, true /* IPS */, TFT_WIDTH, TFT_HEIGHT, 0 /* col offset 1 */,
                                  0 /* row offset 1 */, 0 /* col offset 2 */, 0 /* row offset 2 */);
#else
        tft = new LGFX;
#endif
    }

    LOG_INFO("Power to TFT Backlight");
    backlightEnable->set(true);

#ifdef UNPHONE
    unphone.backlight(true); // using unPhone library
#endif
#ifdef USE_ARDUINO_GFX
#if defined(TFT_NV3001B)
    // Arduino_SWSPI ignores the clock argument, so this only bites on the hardware-SPI variants.
    bool beginStatus = tft->begin(SPI_FREQUENCY);
#else
    bool beginStatus = tft->begin();
#endif
    if (beginStatus)
        LOG_DEBUG("TFT Success");
    else
        LOG_ERROR("TFT Fail");
#else
    tft->init();
#endif

#if defined(M5STACK)
    tft->setRotation(0);
#elif defined(RAK14014)
    tft->setRotation(1);
    tft->setSwapBytes(true);
    //    tft->fillScreen(TFT_BLACK);
    ft6336u.begin();
    pinMode(SCREEN_TOUCH_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(SCREEN_TOUCH_INT), rak14014_tpIntHandle, FALLING);
#elif defined(T_DECK) || defined(PICOMPUTER_S3) || defined(CHATTER_2) || defined(HELTEC_MESH_NODE_T096)
    tft->setRotation(1); // T-Deck has the TFT in landscape
#elif defined(T_WATCH_S3)
    tft->setRotation(2); // T-Watch S3 left-handed orientation
#elif ARCH_PORTDUINO || defined(SENSECAP_INDICATOR) || defined(T_LORA_PAGER) || defined(T_WATCH_ULTRA)
    tft->setRotation(0); // use config.yaml to set rotation
#else
    tft->setRotation(3); // Orient horizontal and wide underneath the silkscreen name label
#endif

#if BASEUI_HAS_TOUCH_CALIBRATION
    // Restore whatever was calibrated last, from the field device-ui also writes - so a calibration
    // done in either UI carries over to the other. NodeDB (and with it uiconfig) is loaded well
    // before Screen::setup() reaches us. An all-zero blob means "never calibrated"; the size check
    // and the first/last-parameter test mirror device-ui's own guard.
    if (uiconfig.calibration_data.size == sizeof(uint16_t) * 8) {
        uint16_t stored[8];
        memcpy(stored, uiconfig.calibration_data.bytes, sizeof(stored));
        if (stored[0] || stored[7]) {
            applyTouchCalibration(stored);
            LOG_INFO("Applied stored touch calibration");
        }
    }
#endif

    tft->fillScreen(getThemeDefaultOffColor());

    // Both buffers have to be DMA-reachable before display() can use the DMA push, so track the
    // two allocations together.
    bool allBuffersDmaCapable = true;
    bool thisBufferDmaCapable = false;

    if (this->linePixelBuffer == NULL) {
#if defined(CO5300_CS)
        // One width per row in a push: this panel stages every row of the pair back to back, and the
        // block starts at the span's x offset, so the staging can reach x + rowsPerPush * spanWidth.
        // At the worst case (x = 0, full-width span) that is exactly rowsPerPush * displayWidth.
        // Span widening can only grow a push rightwards to the panel edge, so x + rowsPerPush * span
        // still tops out at rowsPerPush * displayWidth.
        const size_t linePixels = (size_t)displayWidth * CO5300_ROWS_PER_PUSH;
#else
        const size_t linePixels = (size_t)displayWidth;
#endif
        this->linePixelBuffer = allocPixelBuffer(linePixels, &thisBufferDmaCapable);

        if (!this->linePixelBuffer) {
            LOG_ERROR("Not enough memory to create TFT line buffer");
            return false;
        }
        allBuffersDmaCapable &= thisBufferDmaCapable;
        memaudit::add("display", sizeof(uint16_t) * linePixels);
    }
    if (this->repaintChunkBuffer == NULL) {
        const size_t chunkPixels = (size_t)displayWidth * kFullRepaintChunkRows;

        // Two slots let a chunk's pixel conversion overlap the previous chunk's transfer, but that
        // is only worth having if the internal heap can spare it - so ask for the pair, and drop to
        // a single slot rather than starving whatever initialises after us.
        this->repaintChunkBuffer = tryAllocDmaPixelBuffer(chunkPixels * kFullRepaintChunkSlots);
        this->chunkBufferSlots = kFullRepaintChunkSlots;
        thisBufferDmaCapable = this->repaintChunkBuffer != nullptr;

        if (!this->repaintChunkBuffer) {
            this->chunkBufferSlots = 1;
            this->repaintChunkBuffer = allocPixelBuffer(chunkPixels, &thisBufferDmaCapable);
        }
        if (!this->repaintChunkBuffer) {
            LOG_ERROR("Not enough memory to create TFT repaint chunk buffer");
            return false;
        }
        allBuffersDmaCapable &= thisBufferDmaCapable;
        memaudit::add("display", sizeof(uint16_t) * chunkPixels * this->chunkBufferSlots);
    }
    LOG_DEBUG("TFT pixel buffers: dma=%d chunkSlots=%u", (int)allBuffersDmaCapable, (unsigned)this->chunkBufferSlots);

    this->pixelBuffersAreDmaCapable = allBuffersDmaCapable;
    return true;
}

#endif // USE_TFTDISPLAY
