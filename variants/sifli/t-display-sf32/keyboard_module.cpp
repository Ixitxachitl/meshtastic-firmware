#include "keyboard_module.h"
#include "configuration.h"
#include <Wire.h>

KeyboardModule keyboardModule;

namespace
{
constexpr uint8_t XL9555_ADDR = 0x22;

// PCA9555-compatible register map.
constexpr uint8_t REG_OUTPUT_0 = 0x02;
constexpr uint8_t REG_CONFIG_0 = 0x06;
constexpr uint8_t REG_CONFIG_1 = 0x07;

// Port 0 assignments, from LilyGo's T-SF32-Keyboard V1.0 schematic.
constexpr uint8_t P_GPS_EN = 1 << 0;
constexpr uint8_t P_LED_EN = 1 << 1;
constexpr uint8_t P_WIFI_EN = 1 << 2;
constexpr uint8_t P_M_RST = 1 << 3;
constexpr uint8_t P_GPS_RST = 1 << 4;
constexpr uint8_t P_WIFI_RST = 1 << 5;
constexpr uint8_t P_MOD_SEL = 1 << 6;
constexpr uint8_t P_KEY_RST = 1 << 7;
} // namespace

bool KeyboardModule::writeReg(uint8_t reg, uint8_t value)
{
    Wire.beginTransmission(XL9555_ADDR);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

bool KeyboardModule::writeOutputs()
{
    if (!_present)
        return false;
    return writeReg(REG_OUTPUT_0, _out0);
}

bool KeyboardModule::begin()
{
    Wire.beginTransmission(XL9555_ADDR);
    if (Wire.endTransmission() != 0) {
        LOG_INFO("Keyboard module not attached");
        _present = false;
        return false;
    }
    _present = true;

    // Resets are active low and the enables active high, so start with
    // everything powered and held in reset, then release.
    _out0 = P_GPS_EN | P_LED_EN;
    writeReg(REG_OUTPUT_0, _out0);
    writeReg(REG_CONFIG_0, 0x00); // port 0 all outputs
    writeReg(REG_CONFIG_1, 0xFF); // port 1 unused, leave as inputs

    setGnssReset(false);
    setKeypadReset(false);
    selectUart(UartTarget::Gnss);

    LOG_INFO("Keyboard module found (XL9555 at 0x%02x)", XL9555_ADDR);
    return true;
}

void KeyboardModule::setGnssPower(bool on)
{
    _out0 = on ? (uint8_t)(_out0 | P_GPS_EN) : (uint8_t)(_out0 & ~P_GPS_EN);
    writeOutputs();
}

void KeyboardModule::setGnssReset(bool asserted)
{
    _out0 = asserted ? (uint8_t)(_out0 & ~P_GPS_RST) : (uint8_t)(_out0 | P_GPS_RST);
    writeOutputs();
}

void KeyboardModule::setEspPower(bool on)
{
    if (on) {
        _out0 |= P_WIFI_EN;
        _out0 |= P_WIFI_RST;
    } else {
        _out0 &= ~P_WIFI_EN;
        _out0 &= ~P_WIFI_RST;
    }
    writeOutputs();
}

void KeyboardModule::setBacklightPower(bool on)
{
    // Rail only. The AW21009's own current registers are not programmed yet.
    _out0 = on ? (uint8_t)(_out0 | P_LED_EN) : (uint8_t)(_out0 & ~P_LED_EN);
    writeOutputs();
}

void KeyboardModule::setKeypadReset(bool asserted)
{
    _out0 = asserted ? (uint8_t)(_out0 & ~P_KEY_RST) : (uint8_t)(_out0 | P_KEY_RST);
    writeOutputs();
}

void KeyboardModule::selectUart(UartTarget target)
{
    // The RS2257 muxes put the GNSS on their NC input and the ESP32-C6 on NO,
    // so a low select reaches the GNSS. Worth confirming on hardware.
    if (target == UartTarget::Gnss) {
        _out0 &= ~P_MOD_SEL;
    } else {
        _out0 |= P_MOD_SEL;
    }
    writeOutputs();
}
