#include "I2CBuzzer.h"

#if !MESHTASTIC_EXCLUDE_I2C

#include "detect/ScanI2CTwoWire.h"
#include "main.h"
#include <cstring>

// Global instance
I2CBuzzer *i2cBuzzer = nullptr;

I2CBuzzer::I2CBuzzer() : wire(nullptr), address(0), available(false) {}

bool I2CBuzzer::begin(const ScanI2C::FoundDevice &device)
{
    if (device.type != ScanI2C::DeviceType::I2C_BUZZER) {
        LOG_WARN("I2CBuzzer: Invalid device type");
        return false;
    }

    if (device.address.address == 0 || device.address.port == ScanI2C::I2CPort::NO_I2C) {
        LOG_WARN("I2CBuzzer: Invalid address or port");
        return false;
    }

    address = device.address.address;
    wire = ScanI2CTwoWire::fetchI2CBus(device.address);

    if (!wire) {
        LOG_WARN("I2CBuzzer: Could not get I2C bus");
        return false;
    }

    available = true;
    LOG_INFO("I2CBuzzer initialized at address 0x%02X", address);
    return true;
}

void I2CBuzzer::tone(uint32_t frequency, uint32_t duration)
{
    if (!available) {
        return;
    }

    if (!writeCommand(frequency, duration)) {
        LOG_WARN("I2CBuzzer: Failed to send tone command");
    }
}

void I2CBuzzer::noTone()
{
    if (!available) {
        return;
    }

    // Send zeros to stop any playing tone
    writeCommand(0, 0);
}

bool I2CBuzzer::writeCommand(uint32_t frequency, uint32_t duration)
{
    if (!wire || !available) {
        return false;
    }

    // Modulino protocol: 8 bytes
    // - Bytes 0-3: frequency (little-endian)
    // - Bytes 4-7: duration in ms (little-endian)
    uint8_t buf[8];
    memcpy(&buf[0], &frequency, 4);
    memcpy(&buf[4], &duration, 4);

    wire->beginTransmission(address);
    wire->write(buf, 8);
    uint8_t result = wire->endTransmission();

    if (result != 0) {
        LOG_DEBUG("I2CBuzzer: I2C transmission error %d", result);
        return false;
    }

    return true;
}

#endif // !MESHTASTIC_EXCLUDE_I2C
