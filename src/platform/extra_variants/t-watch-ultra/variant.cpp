#include "configuration.h"

#ifdef T_WATCH_ULTRA

// Lives here, not under variants/, so PlatformIO's LDF resolves the Thread.h that
// input/TouchScreenImpl1.h pulls in transitively. See extra_variants/README.md.

#include "input/TouchScreenImpl1.h"
#include "touch/TouchDrvCST92xx.h"
#include <IoExpanderXL9555.hpp>
#include <Wire.h>

static IoExpanderXL9555 io;
static TouchDrvCST92xx touchDrv;

void earlyInitVariant()
{
    pinMode(LORA_CS, OUTPUT);
    digitalWrite(LORA_CS, HIGH);
    pinMode(DISP_CS, OUTPUT);
    digitalWrite(DISP_CS, HIGH);
    pinMode(SDCARD_CS, OUTPUT);
    digitalWrite(SDCARD_CS, HIGH);
    pinMode(NFC_CS, OUTPUT);
    digitalWrite(NFC_CS, HIGH);
    pinMode(I2C_SDA, INPUT_PULLUP);
    pinMode(I2C_SCL, INPUT_PULLUP);

    if (io.begin(Wire, XL9555_SLAVE_ADDRESS0)) {
        io.pinMode(EXPANDS_DRV_EN, OUTPUT);
        io.digitalWrite(EXPANDS_DRV_EN, HIGH);
        delay(1);
        io.pinMode(EXPANDS_DISP_EN, OUTPUT);
        io.digitalWrite(EXPANDS_DISP_EN, HIGH);
        delay(1);
        io.pinMode(EXPANDS_TOUCH_RST, OUTPUT);
        io.digitalWrite(EXPANDS_TOUCH_RST, LOW);
        delay(20);
        io.digitalWrite(EXPANDS_TOUCH_RST, HIGH);
        delay(60);
        io.pinMode(EXPANDS_LORA_RF_SW, OUTPUT);
        io.digitalWrite(EXPANDS_LORA_RF_SW, HIGH); // set RF switch to built-in LoRa antenna
        // io.pinMode(EXPANDS_SD_DET, INPUT);
    }
    // NOTE: deliberately no LOG_* on the io.begin() failure path. earlyInitVariant() runs
    // before consoleInit(), where calling a LOG_* macro crashes the device (see
    // extra_variants/README.md). On failure the EXPANDS_* pins stay on their defaults.
}

static bool readTouch(int16_t *x, int16_t *y)
{
    int16_t x_array[1], y_array[1];
    uint8_t touched = touchDrv.getPoint(x_array, y_array, 1);
    if (touched > 0) {
        *x = (x_array[0]);
        *y = (y_array[0]);
        // Check bounds
        if (*x < 0 || *x >= TFT_WIDTH || *y < 0 || *y >= TFT_HEIGHT) {
            return false;
        }
        return true; // Valid touch detected
    }
    return false; // No valid touch data
}

void lateInitVariant()
{
    if (config.display.displaymode != meshtastic_Config_DisplayConfig_DisplayMode_COLOR) {
        pinMode(SCREEN_TOUCH_INT, INPUT_PULLUP);
        touchDrv.setPins(-1, SCREEN_TOUCH_INT);
        // The CST92xx shares this I2C bus with the BHI260AP, whose thread is already polling by the time
        // we get here, and nothing serialises the two. A collision leaves i2c_master_transmit returning
        // ESP_ERR_INVALID_STATE, which the driver reports as a firmware read error - indistinguishable
        // from an absent controller, so touch was never created and stayed dead until the next reboot.
        // The window is short, so retry rather than lose the whole session to it.
        constexpr int kTouchInitAttempts = 5;
        bool touchReady = false;
        for (int attempt = 1; attempt <= kTouchInitAttempts && !touchReady; attempt++) {
            if (attempt > 1)
                delay(50);
            touchReady = touchDrv.begin(Wire, TOUCH_SLAVE_ADDRESS, -1, -1);
            if (!touchReady)
                LOG_WARN("CST92xx init attempt %d/%d failed", attempt, kTouchInitAttempts);
        }
        if (touchReady) {
            touchScreenImpl1 = new TouchScreenImpl1(TFT_WIDTH, TFT_HEIGHT, readTouch);
            touchScreenImpl1->init();
        } else {
            LOG_ERROR("failed to initialize CST92xx after %d attempts", kTouchInitAttempts);
        }
    }
}
#endif
