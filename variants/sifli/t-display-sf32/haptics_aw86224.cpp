#include "haptics_aw86224.h"
#include "configuration.h"
#include <Wire.h>

HapticsAW86224 hapticsAW86224;

namespace
{
constexpr uint8_t AW86224_ADDR = 0x58;

// Register map from the AW86224A/B datasheet V1.7; the CONT-mode sequence
// follows LilyGo's aw86224_play_cont() in their SiFli SDK fork.
constexpr uint8_t REG_SRST = 0x00;
constexpr uint8_t REG_PLAYCFG2 = 0x07; // GAIN
constexpr uint8_t REG_PLAYCFG3 = 0x08; // BRK_EN, PLAY_MODE
constexpr uint8_t REG_PLAYCFG4 = 0x09; // STOP, GO
constexpr uint8_t REG_CONTCFG1 = 0x18; // EN_F0_DET
constexpr uint8_t REG_CONTCFG3 = 0x1A; // DRV_WIDTH
constexpr uint8_t REG_CONTCFG6 = 0x1D; // TRACK_EN, DRV1_LVL
constexpr uint8_t REG_CONTCFG7 = 0x1E; // DRV2_LVL
constexpr uint8_t REG_CONTCFG8 = 0x1F; // DRV1_TIME
constexpr uint8_t REG_CONTCFG9 = 0x20; // DRV2_TIME
constexpr uint8_t REG_SYSCRTL2 = 0x44; // WAVDAT_MODE
constexpr uint8_t REG_SYSCRTL7 = 0x49; // GAIN_BYPASS
constexpr uint8_t REG_PWMCFG3 = 0x4E;  // DC protection

constexpr uint8_t SRST_RESET = 0xAA;
constexpr uint8_t PLAY_MODE_CONT = 0x02;
constexpr uint8_t PLAYCFG3_BRK_EN = 1 << 2;
constexpr uint8_t PLAYCFG4_GO = 1 << 0;
constexpr uint8_t PLAYCFG4_STOP = 1 << 1;
constexpr uint8_t CONTCFG1_EN_F0_DET = 1 << 3;
constexpr uint8_t CONTCFG6_TRACK_EN = 1 << 7;
constexpr uint8_t SRATE_12K = 2;

// The module's LRA resonates at 170.0 Hz - LilyGo's driver falls back to this
// when the chip has not been calibrated, and names its waveform blob for it.
constexpr uint32_t LRA_F0_X10 = 1700;

// Half cycles of the two CONT drive phases, both fixed by LilyGo's driver.
constexpr uint8_t DRV1_HALF_CYCLES = 0x04;
constexpr uint8_t DRV2_HALF_CYCLES = 0x06;
} // namespace

bool HapticsAW86224::writeReg(uint8_t reg, uint8_t value)
{
    if (!_present)
        return false;
    Wire.beginTransmission(AW86224_ADDR);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

bool HapticsAW86224::updateReg(uint8_t reg, uint8_t mask, uint8_t value)
{
    if (!_present)
        return false;
    Wire.beginTransmission(AW86224_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0)
        return false;
    if (Wire.requestFrom(AW86224_ADDR, (uint8_t)1) != 1)
        return false;
    uint8_t current = Wire.read();
    return writeReg(reg, (uint8_t)((current & ~mask) | (value & mask)));
}

bool HapticsAW86224::begin()
{
    Wire.beginTransmission(AW86224_ADDR);
    if (Wire.endTransmission() != 0) {
        _present = false;
        return false;
    }
    _present = true;

    // Clears the configuration registers but not SRAM, so the chip lands in a
    // known state without disturbing anything else on the bus.
    writeReg(REG_SRST, SRST_RESET);
    delay(2);

    updateReg(REG_SYSCRTL2, 0x03, SRATE_12K); // 12 kHz waveform sample rate
    updateReg(REG_PWMCFG3, 1 << 7, 1 << 7);   // DC protection
    updateReg(REG_SYSCRTL7, 1 << 6, 1 << 6);  // gain changes during playback
    writeReg(REG_PLAYCFG2, 0x80);             // unity gain

    LOG_INFO("AW86224 haptics found at 0x%02x", AW86224_ADDR);
    return true;
}

void HapticsAW86224::setStrength(uint8_t strength)
{
    _strength = strength > 0x7F ? 0x7F : strength;
}

void HapticsAW86224::play()
{
    if (!_present)
        return;

    writeReg(REG_PLAYCFG4, PLAYCFG4_STOP);

    // Plain CONT drive: no F0 detection, no resonance tracking and no auto
    // brake, matching LilyGo's simple playback path.
    updateReg(REG_PLAYCFG3, 0x03 | PLAYCFG3_BRK_EN, PLAY_MODE_CONT);
    updateReg(REG_CONTCFG1, CONTCFG1_EN_F0_DET, 0);
    updateReg(REG_CONTCFG6, CONTCFG6_TRACK_EN, 0);

    updateReg(REG_CONTCFG6, 0x7F, _strength); // DRV1_LVL
    updateReg(REG_CONTCFG7, 0x7F, _strength); // DRV2_LVL

    // Drive pulse width in units of the chip's timebase, derived from the
    // motor's resonance; the trailing terms are LilyGo's fixed corrections.
    int32_t drvWidth = (int32_t)(240000u / LRA_F0_X10) - 8 - 8 - 15;
    if (drvWidth < 0)
        drvWidth = 0;
    else if (drvWidth > 255)
        drvWidth = 255;
    writeReg(REG_CONTCFG3, (uint8_t)drvWidth);

    writeReg(REG_CONTCFG8, DRV1_HALF_CYCLES);
    writeReg(REG_CONTCFG9, DRV2_HALF_CYCLES);

    writeReg(REG_PLAYCFG4, PLAYCFG4_GO);
}

void HapticsAW86224::stop()
{
    writeReg(REG_PLAYCFG4, PLAYCFG4_STOP);
}
