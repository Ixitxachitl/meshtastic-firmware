#include "variant.h"
#include "configuration.h"
#include "keyboard_module.h"
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
}
