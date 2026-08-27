#include "haptics_aw86224.h"
#include "configuration.h"
#include <Wire.h>

HapticsAW86224 hapticsAW86224;

namespace
{
constexpr uint8_t AW86224_ADDR = 0x58;

// Register map from the AW86224A/B datasheet V1.7.
constexpr uint8_t REG_SRST = 0x00;
constexpr uint8_t REG_PLAYCFG2 = 0x07; // GAIN
constexpr uint8_t REG_PLAYCFG3 = 0x08; // STOP_MODE, BRK_EN, PLAY_MODE
constexpr uint8_t REG_PLAYCFG4 = 0x09; // STOP, GO
constexpr uint8_t REG_CONTCFG6 = 0x1D; // TRACK_EN, DRV1_LVL
constexpr uint8_t REG_CONTCFG7 = 0x1E; // DRV2_LVL
constexpr uint8_t REG_CONTCFG8 = 0x1F; // DRV1_TIME
constexpr uint8_t REG_CONTCFG9 = 0x20; // DRV2_TIME

constexpr uint8_t SRST_RESET = 0xAA;

constexpr uint8_t PLAY_MODE_CONT = 0x02;
constexpr uint8_t PLAYCFG3_BRK_EN = 1 << 2;

constexpr uint8_t PLAYCFG4_GO = 1 << 0;
constexpr uint8_t PLAYCFG4_STOP = 1 << 1;

// DRV1 is the ramp-up burst; DRV2 carries the body of the buzz. Both are
// counted in half cycles of the motor's resonance, not milliseconds.
constexpr uint8_t DRV1_HALF_CYCLES = 4;

// Resonant frequency of the module's LRA. LilyGo does not publish it, so this
// is the common 170 Hz and only affects how play()'s duration is scaled.
constexpr uint32_t LRA_F0_HZ = 170;
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

bool HapticsAW86224::begin()
{
    Wire.beginTransmission(AW86224_ADDR);
    if (Wire.endTransmission() != 0) {
        _present = false;
        return false;
    }
    _present = true;

    // Software reset clears the configuration registers but not SRAM, so the
    // chip lands in a known state without disturbing anything else on the bus.
    writeReg(REG_SRST, SRST_RESET);
    delay(2);

    // Brake at the end of a burst so the mass stops with the drive rather than
    // ringing on, and keep the datasheet's default drive levels.
    writeReg(REG_PLAYCFG3, PLAYCFG3_BRK_EN | PLAY_MODE_CONT);
    writeReg(REG_CONTCFG8, DRV1_HALF_CYCLES);

    LOG_INFO("AW86224 haptics found at 0x%02x", AW86224_ADDR);
    return true;
}

void HapticsAW86224::setGain(uint8_t gain)
{
    writeReg(REG_PLAYCFG2, gain);
}

void HapticsAW86224::play(uint16_t durationMs)
{
    if (!_present)
        return;

    // One half cycle is 1/(2*F0) seconds, so a duration in milliseconds needs
    // durationMs * 2 * F0 / 1000 of them. The field is eight bits wide.
    uint32_t halfCycles = ((uint32_t)durationMs * 2u * LRA_F0_HZ) / 1000u;
    if (halfCycles > DRV1_HALF_CYCLES)
        halfCycles -= DRV1_HALF_CYCLES;
    else
        halfCycles = 1;
    if (halfCycles > 255)
        halfCycles = 255;

    writeReg(REG_CONTCFG9, (uint8_t)halfCycles);
    writeReg(REG_PLAYCFG4, PLAYCFG4_GO);
}

void HapticsAW86224::stop()
{
    writeReg(REG_PLAYCFG4, PLAYCFG4_STOP);
}
