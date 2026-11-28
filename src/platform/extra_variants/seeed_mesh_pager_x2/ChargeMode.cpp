#if defined(SEEED_MESHPAGER_X2) && defined(X2_POWER_KEY_UI)

#include "Arduino.h"
#include "mesh/Throttle.h"
#include "variant.h"
#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <initializer_list>

#if HAS_TFT
#include "graphics/LGFX/LGFX_GENERIC.h"
using ChargeLcd = LGFX_GENERIC;
#else
#include <LovyanGFX.hpp>
// BaseUI builds carry no device-ui, so no LGFX_GENERIC: the same panel, configured from variant.h as TFTDisplay does.
class ChargeLcd : public lgfx::LGFX_Device
{
    lgfx::Panel_ST7789P3 panel;
    lgfx::Bus_SPI bus;
    lgfx::Light_PWM light;

  public:
    ChargeLcd()
    {
        {
            auto cfg = bus.config();
            cfg.spi_host = ST7789_SPI_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = SPI_FREQUENCY;
            cfg.freq_read = SPI_READ_FREQUENCY;
            cfg.spi_3wire = true;
            cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = ST7789_SCK;
            cfg.pin_mosi = ST7789_SDA;
            cfg.pin_miso = ST7789_MISO;
            cfg.pin_dc = ST7789_RS;
            bus.config(cfg);
            panel.setBus(&bus);
        }
        {
            auto cfg = panel.config();
            cfg.pin_cs = ST7789_CS;
            cfg.pin_rst = ST7789_RESET;
            cfg.pin_busy = ST7789_BUSY;
            cfg.memory_width = TFT_WIDTH;
            cfg.memory_height = TFT_HEIGHT;
            cfg.panel_width = TFT_WIDTH;
            cfg.panel_height = TFT_HEIGHT;
            cfg.offset_x = TFT_OFFSET_X;
            cfg.offset_y = TFT_OFFSET_Y;
            cfg.offset_rotation = TFT_OFFSET_ROTATION;
            cfg.readable = false;
            cfg.invert = true;
            cfg.rgb_order = false;
            cfg.bus_shared = false;
            panel.config(cfg);
        }
        {
            auto cfg = light.config();
            cfg.pin_bl = ST7789_BL;
            cfg.invert = false;
            light.config(cfg);
            panel.setLight(&light);
        }
        setPanel(&panel);
    }
};
#endif

using namespace meshpager_x2;

namespace
{
constexpr uint32_t CHARGE_REQUEST = 0x58324348;
constexpr uint32_t SHOW_MS = 10000;
constexpr uint32_t BOOT_HOLD_MS = 5000;
constexpr uint8_t CHARGER_STAT = EXP_PIN_BUTTON_PWR;
RTC_NOINIT_ATTR uint32_t chargeRequest;

int batterySegments()
{
    uint32_t totalMv = 0;
    for (int sample = 0; sample < 8; ++sample)
        totalMv += analogReadMilliVolts(BATTERY_PIN);
    const uint32_t batteryMv = (totalMv / 8) * ADC_MULTIPLIER;
    constexpr uint16_t levels[] = {4308, 4141, 4026, 3916, 3821, 3751, 3703, 3672, 3629, 3548, 3300};
    if (batteryMv < 2500 || batteryMv > 4600)
        return 2;
    for (int index = 0; index < 11; ++index) {
        if (batteryMv >= levels[index]) {
            if (index == 0)
                return 3;
            const float percent = 10.0f * (10 - index + float(batteryMv - levels[index]) / (levels[index - 1] - levels[index]));
            const int segments = (percent * 4 + 50) / 100;
            return segments < 4 ? segments : 3;
        }
    }
    return 0;
}

void drawBatteryFrame(ChargeLcd &lcd)
{
    lcd.fillRoundRect(100, 30, 120, 180, 8, 0xFFFF);
    lcd.fillRoundRect(104, 34, 112, 172, 4, 0);
    lcd.fillRect(138, 20, 44, 12, 0xFFFF);
}

// Only repaint segments whose color changed; repainting the whole battery flickers.
void drawBatterySegments(ChargeLcd &lcd, int segments, bool pulse, int32_t (&shown)[4])
{
    for (int segment = 0; segment < 4; ++segment) {
        const uint16_t color = segment < segments ? 0x07E0 : (segment == segments && pulse ? 0x03E0 : 0);
        if (shown[segment] == color)
            continue;
        lcd.fillRoundRect(108, 167 - 43 * segment, 104, 35, 4, color);
        shown[segment] = color;
    }
}
} // namespace

bool x2ChargeModeRequested()
{
    const bool redirected = chargeRequest == CHARGE_REQUEST;
    chargeRequest = 0;
    if (redirected)
        return true;
    if (esp_reset_reason() != ESP_RST_POWERON)
        return false;
    for (int sample = 0; sample < 3; ++sample) {
        if (digitalRead(BUTTON_PIN) == LOW)
            return true;
        delay(5);
    }
    return false;
}

void x2RunChargeMode(bool (*readPort)(uint8_t, uint8_t &), bool (*drivePin)(uint8_t, bool))
{
    bool ready = true;
    for (uint8_t pin : {EXP_PIN_GNSS_SLEEP_INT, EXP_PIN_SEN_EN, EXP_PIN_SD_PWR_EN, EXP_PIN_GNSS_VRTC_EN, EXP_PIN_GNSS_PWR_EN,
                        EXP_PIN_PA_PWR_EN, EXP_PIN_LORA_RESET})
        ready &= drivePin(pin, false);
    ready &= drivePin(EXP_PIN_BUTTON_RETURN, false);
    ready &= drivePin(EXP_PIN_BAT_ADC_EN, true);
    ready &= drivePin(EXP_PIN_LCD_PWR_EN, true);
    ready &= drivePin(EXP_PIN_LCD_RST, false);
    delay(20);
    ready &= drivePin(EXP_PIN_LCD_RST, true);
    delay(120);
    if (!ready)
        ESP.restart();

    ChargeLcd lcd;
    if (!lcd.init())
        ESP.restart();
    lcd.setBrightness(0);
    lcd.setRotation(0);
    lcd.fillScreen(0);
    drawBatteryFrame(lcd);
    int32_t shownColors[4] = {-1, -1, -1, -1};
    if (!drivePin(EXP_PIN_PWR_HOLD, false))
        ESP.restart();

    gpio_wakeup_enable((gpio_num_t)BUTTON_PIN, GPIO_INTR_HIGH_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    bool full = false;
    int fullSamples = 0;
    bool armed = digitalRead(BUTTON_PIN) == LOW;
    bool held = false;
    bool bootRequested = false;
    uint32_t heldSince = 0;
    uint32_t lastStatMs = 0;
    while (true) {
        const int frozenSegments = batterySegments();
        const uint32_t shownAt = millis();
        int previousSegments = -1;
        bool previousPulse = false;
        bool visible = true;
        while (visible || digitalRead(BUTTON_PIN) == HIGH) {
            const bool pressed = digitalRead(BUTTON_PIN) == HIGH;
            if (!pressed)
                armed = true;
            if (armed && pressed && !held) {
                heldSince = millis();
                held = true;
            }
            if (held && pressed && Throttle::hasElapsed(heldSince, BOOT_HOLD_MS))
                bootRequested = true;
            if (!pressed) {
                held = false;
                if (bootRequested) {
                    if (drivePin(EXP_PIN_PWR_HOLD, true))
                        ESP.restart();
                    bootRequested = false;
                }
            }

            if (!full && Throttle::hasElapsed(lastStatMs, 1000)) {
                lastStatMs = millis();
                uint8_t status = 0;
                if (readPort(0, status) && (status & (1U << CHARGER_STAT)))
                    full = ++fullSamples >= 2;
                else
                    fullSamples = 0;
            }
            const uint32_t elapsedMs = millis() - shownAt;
            const int frame = elapsedMs / 450;
            const bool animating = !full && frame < 10;
            const int segments = full ? 4 : (animating ? (frame % 5 + 1) % 5 : frozenSegments);
            const bool pulse = !full && !animating && (elapsedMs / 600) % 2;
            if (segments != previousSegments || pulse != previousPulse) {
                drawBatterySegments(lcd, segments, pulse, shownColors);
                previousSegments = segments;
                previousPulse = pulse;
                lcd.setBrightness(visible ? BRIGHTNESS_DEFAULT : 0);
            }
            if (visible && Throttle::hasElapsed(shownAt, SHOW_MS)) {
                visible = false;
                lcd.setBrightness(0);
            }
            delay(10);
        }
        uint8_t ignored = 0;
        for (uint8_t port = 0; port < 3; ++port)
            readPort(port, ignored);
        esp_light_sleep_start();
    }
}

void x2RequestChargeMode()
{
    chargeRequest = CHARGE_REQUEST;
    ESP.restart();
}

#endif