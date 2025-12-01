#include "BHI260APSensor.h"
#include "NodeDB.h"
#include <Wire.h>

#if !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C

// BHI260AP I2C Register Definitions (from Bosch BHY2 API)
#define BHI260AP_REG_CHIP_ID 0x00
#define BHI260AP_REG_ERROR_VALUE 0x02
#define BHI260AP_REG_BOOT_STATUS 0x25
#define BHI260AP_REG_HOST_CTRL 0x28 // Reset control register
#define BHI260AP_REG_FEATURE_STATUS 0x35
#define BHI260AP_REG_CHAN_CMD 0x34
#define BHI260AP_REG_CHAN_FIFO 0x2E
#define BHI260AP_REG_FIFO_FLUSH 0x32
#define BHI260AP_REG_FIFO_CTRL 0x28

#define BHI260AP_CHIP_ID 0x89
#define BHI260AP_CMD_SOFT_RESET 0x01

// Boot status bits (from Bosch BHY2 API)
#define BHI260AP_BST_FLASH_DETECTED 0x01
#define BHI260AP_BST_FLASH_VERIFY_DONE 0x02
#define BHI260AP_BST_FLASH_VERIFY_ERROR 0x04
#define BHI260AP_BST_NO_FLASH 0x08 // Host download mode (no flash detected)
#define BHI260AP_BST_HOST_INTERFACE_READY 0x10
#define BHI260AP_BST_HOST_FW_VERIFY_DONE 0x80

// Virtual sensor IDs (from Bosch documentation)
#define BHI260AP_SENSOR_ID_STEP_COUNTER 52

// Minimal BHI260AP driver - basic functionality
// Global instance for renderer access
BHI260APSensor *g_bhi260ap_instance = nullptr;

// Simple I2C read/write helpers
static bool readRegister(TwoWire *wire, uint8_t addr, uint8_t reg, uint8_t *data, size_t len)
{
    wire->beginTransmission(addr);
    wire->write(reg);
    if (wire->endTransmission(false) != 0) {
        return false;
    }

    size_t received = wire->requestFrom(addr, len);
    if (received != len) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        data[i] = wire->read();
    }
    return true;
}

static bool writeRegister(TwoWire *wire, uint8_t addr, uint8_t reg, uint8_t value)
{
    wire->beginTransmission(addr);
    wire->write(reg);
    wire->write(value);
    return (wire->endTransmission() == 0);
}

BHI260APSensor::BHI260APSensor(ScanI2C::FoundDevice foundDevice) : MotionSensor::MotionSensor(foundDevice)
{
    LOG_INFO("BHI260AP: Sensor created (minimal driver)");
    g_bhi260ap_instance = this;
}

BHI260APSensor::~BHI260APSensor()
{
    g_bhi260ap_instance = nullptr;
}

bool BHI260APSensor::init()
{
    // Get I2C bus and address
    if (devicePort() == ScanI2C::I2CPort::WIRE1) {
#ifdef I2C_SDA1
        wire = &Wire1;
#else
        LOG_WARN("BHI260AP: Wire1 requested but not available");
        return false;
#endif
    } else {
        wire = &Wire;
    }

    i2cAddress = deviceAddress();

    LOG_INFO("BHI260AP: Attempting to initialize sensor at address 0x%02x", i2cAddress);

    // Check if firmware is already running by reading chip ID first
    uint8_t chipId = 0;
    delay(10);
    if (readRegister(wire, i2cAddress, BHI260AP_REG_CHIP_ID, &chipId, 1)) {
        LOG_INFO("BHI260AP: Chip ID: 0x%02x (expected 0x%02x)", chipId, BHI260AP_CHIP_ID);
        if (chipId == BHI260AP_CHIP_ID) {
            LOG_INFO("BHI260AP: Firmware already running! Skipping boot sequence.");

            // Store global instance
            g_bhi260ap_instance = this;
            initialized = true;

            // TODO: Configure virtual sensors (accelerometer, step counter)
            // For now, just try to read any existing step data
            LOG_INFO("BHI260AP: Sensor initialized (firmware already loaded)");
            return true;
        }
    }

    // Firmware not running yet - check boot status
    uint8_t bootStatus = 0;
    if (!readRegister(wire, i2cAddress, BHI260AP_REG_BOOT_STATUS, &bootStatus, 1)) {
        LOG_ERROR("BHI260AP: Failed to read initial boot status");
        return false;
    }

    LOG_INFO("BHI260AP: Boot status: 0x%02x", bootStatus);
    LOG_INFO("BHI260AP: - Host interface ready: %s", (bootStatus & BHI260AP_BST_HOST_INTERFACE_READY) ? "YES" : "NO");
    LOG_INFO("BHI260AP: - Flash detected: %s", (bootStatus & BHI260AP_BST_FLASH_DETECTED) ? "YES" : "NO");
    LOG_INFO("BHI260AP: - Flash verify done: %s", (bootStatus & BHI260AP_BST_FLASH_VERIFY_DONE) ? "YES" : "NO");

    // Check if external flash is detected
    if (!(bootStatus & BHI260AP_BST_FLASH_DETECTED)) {
        LOG_ERROR("┌──────────────────────────────────────────────────────────────┐");
        LOG_ERROR("│ BHI260AP FIRMWARE NOT DETECTED                              │");
        LOG_ERROR("├──────────────────────────────────────────────────────────────┤");
        LOG_ERROR("│ Boot Status: 0x%02x (FLASH_DETECTED bit NOT set)            │", bootStatus);
        LOG_ERROR("│                                                              │");
        LOG_ERROR("│ This sensor requires firmware loaded into its external       │");
        LOG_ERROR("│ SPI flash chip. Meshtastic cannot include the ~109KB        │");
        LOG_ERROR("│ firmware due to nRF52840 flash space constraints.           │");
        LOG_ERROR("│                                                              │");
        LOG_ERROR("│ SOLUTION: Program sensor once using LilyGO's example:       │");
        LOG_ERROR("│   https://github.com/Xinyuan-LilyGO/T-Echo/blob/main/       │");
        LOG_ERROR("│   examples/SensorBHI260AP/SensorBHI260AP.ino                │");
        LOG_ERROR("│                                                              │");
        LOG_ERROR("│ See detailed instructions in:                                │");
        LOG_ERROR("│   variants/nrf52840/t-echo-plus/BHI260AP_FIRMWARE_REQUIRED.md│");
        LOG_ERROR("│                                                              │");
        LOG_ERROR("│ Step counter will display 0 until sensor is programmed.     │");
        LOG_ERROR("└──────────────────────────────────────────────────────────────┘");

        // Store instance anyway for UI (will show 0 steps)
        g_bhi260ap_instance = this;
        initialized = true;
        return true; // Return success so UI still shows (with 0 steps)
    }

    // If host interface is ready but firmware not running, try boot from flash
    if (bootStatus & BHI260AP_BST_HOST_INTERFACE_READY) {
        LOG_INFO("BHI260AP: Host interface ready, attempting boot from flash...");

        uint32_t startTime = millis();
        bool flashVerified = false;

        LOG_INFO("BHI260AP: Waiting for flash verification...");
        while ((millis() - startTime) < 3000) { // 3 second timeout
            if (!readRegister(wire, i2cAddress, BHI260AP_REG_BOOT_STATUS, &bootStatus, 1)) {
                LOG_ERROR("BHI260AP: Failed to read boot status");
                return false;
            }

            LOG_DEBUG("BHI260AP: Boot status during verification: 0x%02x", bootStatus);

            // Check if flash verification is done
            if (bootStatus & BHI260AP_BST_FLASH_VERIFY_DONE) {
                flashVerified = true;
                LOG_INFO("BHI260AP: Flash verification completed");
                break;
            }

            // Check for verification error
            if (bootStatus & BHI260AP_BST_FLASH_VERIFY_ERROR) {
                LOG_ERROR("BHI260AP: Flash verification error detected");
                return false;
            }

            delay(50);
        }

        if (!flashVerified) {
            LOG_ERROR("BHI260AP: Timeout waiting for flash verification");
            LOG_INFO("BHI260AP: Final boot status: 0x%02x", bootStatus);
            return false;
        }

        // Step 3: Check host interface is ready
        if (!(bootStatus & BHI260AP_BST_HOST_INTERFACE_READY)) {
            LOG_ERROR("BHI260AP: Host interface not ready after flash verification");
            return false;
        }

        // Step 4: Issue boot from flash command
        LOG_INFO("BHI260AP: Sending boot from flash command...");
        uint8_t bootCmd = 0x01; // CMD_BOOT_FLASH
        if (!writeRegister(wire, i2cAddress, BHI260AP_REG_CHAN_CMD, bootCmd)) {
            LOG_ERROR("BHI260AP: Failed to send boot command");
            return false;
        }

        // Step 4: Wait for boot to complete (check for both HOST_INTERFACE_READY and FLASH_VERIFY_DONE)
        startTime = millis();
        bool bootComplete = false;

        LOG_INFO("BHI260AP: Waiting for boot completion...");
        while ((millis() - startTime) < 5000) { // 5 second timeout
            if (!readRegister(wire, i2cAddress, BHI260AP_REG_BOOT_STATUS, &bootStatus, 1)) {
                LOG_ERROR("BHI260AP: Failed to read boot status");
                return false;
            }

            LOG_DEBUG("BHI260AP: Boot status during boot: 0x%02x", bootStatus);

            // Boot is complete when HOST_INTERFACE_READY is set and no errors
            if ((bootStatus & BHI260AP_BST_HOST_INTERFACE_READY) && !(bootStatus & BHI260AP_BST_FLASH_VERIFY_ERROR)) {
                bootComplete = true;
                LOG_INFO("BHI260AP: Boot completed successfully");
                break;
            }

            delay(50);
        }

        if (!bootComplete) {
            LOG_ERROR("BHI260AP: Timeout waiting for boot completion");
            LOG_INFO("BHI260AP: Final boot status: 0x%02x", bootStatus);
            return false;
        }

        // Verify chip ID after boot
        if (readRegister(wire, i2cAddress, BHI260AP_REG_CHIP_ID, &chipId, 1)) {
            LOG_INFO("BHI260AP: Chip ID after boot: 0x%02x (expected 0x%02x)", chipId, BHI260AP_CHIP_ID);
            if (chipId != BHI260AP_CHIP_ID) {
                LOG_WARN("BHI260AP: Unexpected chip ID - boot may have failed");
                return false;
            }
        }

        LOG_INFO("BHI260AP: Sensor initialized successfully after boot from flash");

        // Store global instance for renderer access
        g_bhi260ap_instance = this;

        initialized = true;
        return true;
    } else {
        LOG_ERROR("BHI260AP: Host interface not ready and firmware not running");
        LOG_ERROR("BHI260AP: Boot status: 0x%02x - sensor may need reset or power cycle", bootStatus);
        return false;
    }
}

bool BHI260APSensor::readStepCounterFromFifo()
{
    // Read FIFO to check for step counter data
    // This is a simplified approach - full implementation would need proper FIFO parsing

    // Check feature status to see if step counter is active
    uint8_t featureStatus = 0;
    if (!readRegister(wire, i2cAddress, BHI260AP_REG_FEATURE_STATUS, &featureStatus, 1)) {
        return false;
    }

    // Try to read from FIFO
    // FIFO format: [sensor_id, data_bytes...]
    // Step counter sends 4 bytes of data
    uint8_t fifoData[16];
    if (readRegister(wire, i2cAddress, BHI260AP_REG_CHAN_FIFO, fifoData, sizeof(fifoData))) {
        // Simple parsing - look for step counter sensor ID
        for (size_t i = 0; i < sizeof(fifoData) - 5; i++) {
            if (fifoData[i] == BHI260AP_SENSOR_ID_STEP_COUNTER) {
                // Next 4 bytes are step count (little-endian)
                uint32_t steps = fifoData[i + 1] | (fifoData[i + 2] << 8) | (fifoData[i + 3] << 16) | (fifoData[i + 4] << 24);

                if (steps != stepCount && steps < 1000000) { // Sanity check
                    stepCount = steps;
                    LOG_DEBUG("BHI260AP: Step count updated: %u", stepCount);
                    return true;
                }
            }
        }
    }

    return false;
}

int32_t BHI260APSensor::runOnce()
{
    if (!initialized || !wire) {
        return MOTION_SENSOR_CHECK_INTERVAL_MS;
    }

    // Try to read step counter from FIFO
    readStepCounterFromFifo();

    // Check for motion to wake screen
    // TODO: Could read accelerometer data for motion detection

    return MOTION_SENSOR_CHECK_INTERVAL_MS;
}

#endif
