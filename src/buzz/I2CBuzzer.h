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
 * Supported addresses (auto-detected when HAS_I2C_BUZZER is defined):
 * - 0x1E: Modulino Buzzer default address (distinguished from HMC5883L via probe)
 * - 0x3C: Modulino Buzzer pinstrap address (distinguished from OLED via probe)
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
     * @brief Start playing an RTTTL melody (non-blocking)
     * @param rtttlSong RTTTL format string
     */
    void beginRtttl(const char *rtttlSong);

    /**
     * @brief Continue playing the current RTTTL melody
     * Call this frequently from the main loop
     */
    void playRtttl();

    /**
     * @brief Stop RTTTL playback
     */
    void stopRtttl();

    /**
     * @brief Check if RTTTL is currently playing
     */
    bool isRtttlPlaying() const { return rtttlPlaying; }

    /**
     * @brief Get the I2C address being used
     */
    uint8_t getAddress() const { return address; }

  private:
    TwoWire *wire;
    uint8_t address;
    bool available;

    // RTTTL playback state
    const char *rtttlBuffer;
    const char *rtttlFirstNote;
    bool rtttlPlaying;
    uint8_t rtttlDefaultDur;
    uint8_t rtttlDefaultOct;
    int rtttlBpm;
    long rtttlWholenote;
    unsigned long rtttlNoteEndTime;

    /**
     * @brief Write the 8-byte tone command to the buzzer
     */
    bool writeCommand(uint32_t frequency, uint32_t duration);

    /**
     * @brief Parse and play the next RTTTL note
     */
    void rtttlNextNote();
};

// Global instance (initialized in main.cpp after I2C scan)
extern I2CBuzzer *i2cBuzzer;

#endif // !MESHTASTIC_EXCLUDE_I2C
