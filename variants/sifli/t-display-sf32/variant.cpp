#include "variant.h"
#include "configuration.h"
#include "haptics_aw86224.h"
#include "input/TouchScreenImpl1.h"
#include "keyboard_module.h"
#include "touch_cst9220.h"
#include <Wire.h>

void initVariant()
{
    // Bring up the switched 3V3 rail and the LoRa module supply before
    // anything probes SPI1. Everything else is configured from the board DTS.
    pinMode(PIN_3V3_EN, OUTPUT);
    digitalWrite(PIN_3V3_EN, HIGH);

    pinMode(SX126X_POWER_EN, OUTPUT);
    digitalWrite(SX126X_POWER_EN, HIGH);

    // AMOLED VCI rail, needed before the panel's reset pulse.
    pinMode(LCD_VCI_EN, OUTPUT);
    digitalWrite(LCD_VCI_EN, HIGH);
    delay(10);

    // Powers up the keyboard module if one is attached, so the I2C scan that
    // follows sees its devices and the GNSS is already out of reset.
    Wire.begin();
    keyboardModule.begin();
    hapticsAW86224.begin();
}

// Runs late in setup(), once the screen exists. LovyanGFX has no CST9220
// driver, so the touchscreen is built here from the variant's own reader
// rather than from the panel - see VARIANT_TOUCHSCREEN in the build flags.
void lateInitVariant()
{
    touchInit();
    touchScreenImpl1 = new TouchScreenImpl1(TFT_WIDTH, TFT_HEIGHT, readTouch);
    touchScreenImpl1->init();
}
