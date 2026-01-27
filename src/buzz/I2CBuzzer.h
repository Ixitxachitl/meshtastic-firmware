#pragma once

#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_I2C

#include "detect/ScanI2C.h"
#include <Wire.h>
#include <cstdint>

/**
 * @brief I2C Buzzer driver compatible with Arduino Modulino protocol.
 *
 * The Modulino buzzer protocol is simple: write 8 bytes over I2C
 * - Bytes 0-3: Frequency in Hz (little-endian uint32_t)
 * - Bytes 4-7: Duration in ms (little-endian uint32_t)
 *
 * Default I2C address is 0x3C (same as Modulino Buzzer).
 * Note: This conflicts with SSD1306 displays, so only enable when
 * HAS_I2C_BUZZER is defined in variant.h and no OLED is on that address.
 */
class I2CBuzzer
{
  public:
    I2CBuzzer();

    /**
     * @brief Initialize the I2C buzzer with detected device info
     * @param device The found device from I2C scan
     * @return true if initialization successful
     */
    bool begin(const ScanI2C::FoundDevice &device);

    /**
     * @brief Check if buzzer is available
     */
    bool isAvailable() const { return available; }

    /**
     * @brief Play a tone at specified frequency and duration
     * @param frequency Frequency in Hz (e.g., 440 for A4)
     * @param duration Duration in milliseconds
     */
    void tone(uint32_t frequency, uint32_t duration);

    /**
     * @brief Stop any currently playing tone
     */
    void noTone();

    /**
     * @brief Get the I2C address being used
     */
    uint8_t getAddress() const { return address; }

  private:
    TwoWire *wire;
    uint8_t address;
    bool available;

    /**
     * @brief Write the 8-byte tone command to the buzzer
     */
    bool writeCommand(uint32_t frequency, uint32_t duration);
};

// Global instance (initialized in main.cpp after I2C scan)
extern I2CBuzzer *i2cBuzzer;

#endif // !MESHTASTIC_EXCLUDE_I2C
