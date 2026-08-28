#pragma once

#include <stdbool.h>
#include <stdint.h>

// AW86224 LRA haptic driver at 0x58, on the optional T-SF32-Keyboard module.
// Driven in CONT mode, which synthesises the drive waveform on-chip, so no
// waveform library has to be uploaded to its SRAM first.
//
// The chip is held in reset by the module's XL9555 until KeyboardModule
// releases it, so begin() must run after that. Like the rest of the module,
// nothing here assumes the board is attached.

class HapticsAW86224
{
  public:
    bool begin();
    bool present() const { return _present; }

    // CONT playback runs until stop(); the caller owns the duration, which is
    // how LilyGo's own driver does it.
    void play();
    void stop();

    // Drive level, 0..127. Applied to both CONT drive phases.
    void setStrength(uint8_t strength);

  private:
    bool writeReg(uint8_t reg, uint8_t value);
    bool updateReg(uint8_t reg, uint8_t mask, uint8_t value);

    bool _present = false;
    uint8_t _strength = 0x7F;
};

extern HapticsAW86224 hapticsAW86224;
