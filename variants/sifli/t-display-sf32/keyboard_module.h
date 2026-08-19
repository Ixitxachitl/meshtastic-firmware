#pragma once

#include <stdbool.h>
#include <stdint.h>

// The optional T-SF32-Keyboard module. Everything on it hangs off the shared
// 3V3 I2C bus: an XL9555 expander at 0x22 that gates power and resets, a
// TCA8418 keypad at 0x34, an AW21009 backlight driver at 0x25, an AW86224
// haptic driver at 0x58 and a BME280 at 0x76. An L76K GNSS and an ESP32-C6
// share one UART, muxed by MOD_SEL on the expander, so only one is reachable.
//
// The module is hot-pluggable, so nothing here assumes it is attached: begin()
// probes for the expander and everything else is a no-op when it is absent.

class KeyboardModule
{
  public:
    enum class UartTarget { Gnss, Esp32C6 };

    // Probes the expander and drives the module into a known state: GNSS
    // powered, keypad out of reset, UART muxed to the GNSS.
    bool begin();
    bool present() const { return _present; }

    void setGnssPower(bool on);
    void setGnssReset(bool asserted);
    void setEspPower(bool on);
    void setBacklightPower(bool on);
    void setKeypadReset(bool asserted);
    void selectUart(UartTarget target);

  private:
    bool writeOutputs();
    bool writeReg(uint8_t reg, uint8_t value);

    bool _present = false;
    // Mirror of output port 0; the expander is write-only in practice here.
    uint8_t _out0 = 0;
};

extern KeyboardModule keyboardModule;
