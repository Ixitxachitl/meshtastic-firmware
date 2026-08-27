#pragma once

#include <stdbool.h>
#include <stdint.h>

// AW86224 LRA haptic driver at 0x58, on the optional T-SF32-Keyboard module.
// Driven in CONT mode, which synthesises the drive waveform on-chip, so no
// waveform library has to be uploaded to its SRAM first.
//
// Like the rest of the module, nothing here assumes the board is attached:
// begin() probes for the chip and every other call is a no-op when absent.

class HapticsAW86224
{
  public:
    bool begin();
    bool present() const { return _present; }

    // Buzzes for roughly durationMs. Resolution is one half cycle of the
    // motor's resonance, so short pulses quantise coarsely.
    void play(uint16_t durationMs = 30);
    void stop();

    // 0x80 is the chip default and corresponds to unity drive.
    void setGain(uint8_t gain);

  private:
    bool writeReg(uint8_t reg, uint8_t value);

    bool _present = false;
};

extern HapticsAW86224 hapticsAW86224;
