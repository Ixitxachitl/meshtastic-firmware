#include "variant.h"
#include "configuration.h"

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
}
