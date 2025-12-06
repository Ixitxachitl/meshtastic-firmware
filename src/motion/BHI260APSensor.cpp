#include "BHI260APSensor.h"
#include "NodeDB.h"
#include "input/InputBroker.h"
#include <Wire.h>

// SensorLib is optional - enables RAM-based firmware upload for BHI260AP
// boards without external SPI flash (boot status 0x18)
#ifdef HAS_BHI260AP_SENSORLIB
#include <SensorBHI260AP.hpp>

// Optional: Embedded firmware for automatic programming
#ifdef HAS_BHI260AP_FIRMWARE_EMBEDDED
#include "motion/bhi260ap_firmware.h"
#endif
#endif

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

    // Read boot status first to understand sensor state
    uint8_t bootStatus = 0;
    delay(50); // Give sensor time to stabilize after power-on

    if (!readRegister(wire, i2cAddress, BHI260AP_REG_BOOT_STATUS, &bootStatus, 1)) {
        LOG_ERROR("BHI260AP: Failed to read boot status - I2C communication error");
        return false;
    }

    LOG_INFO("BHI260AP: Boot status: 0x%02x", bootStatus);
    LOG_INFO("BHI260AP: - Flash detected: %s", (bootStatus & BHI260AP_BST_FLASH_DETECTED) ? "YES" : "NO");
    LOG_INFO("BHI260AP: - Flash verify done: %s", (bootStatus & BHI260AP_BST_FLASH_VERIFY_DONE) ? "YES" : "NO");
    LOG_INFO("BHI260AP: - Flash verify error: %s", (bootStatus & BHI260AP_BST_FLASH_VERIFY_ERROR) ? "YES" : "NO");
    LOG_INFO("BHI260AP: - Host interface ready: %s", (bootStatus & BHI260AP_BST_HOST_INTERFACE_READY) ? "YES" : "NO");
    LOG_INFO("BHI260AP: - Host FW verify done: %s", (bootStatus & BHI260AP_BST_HOST_FW_VERIFY_DONE) ? "YES" : "NO");

    // Check if firmware is already running by reading chip ID
    uint8_t chipId = 0;
    if (readRegister(wire, i2cAddress, BHI260AP_REG_CHIP_ID, &chipId, 1)) {
        LOG_INFO("BHI260AP: Chip ID: 0x%02x (expected 0x%02x)", chipId, BHI260AP_CHIP_ID);
        if (chipId == BHI260AP_CHIP_ID) {
            LOG_INFO("BHI260AP: ✓ Firmware already running! Sensor ready.");

            // Store global instance
            g_bhi260ap_instance = this;
            initialized = true;

            // TODO: Configure virtual sensors (accelerometer, step counter)
            // For now, just try to read any existing step data
            LOG_INFO("BHI260AP: Sensor initialized successfully");
            return true;
        }
    } else {
        LOG_WARN("BHI260AP: Failed to read chip ID register");
    }

    // If chip ID doesn't match, firmware is not running yet
    // Check if external flash is detected
    if (!(bootStatus & BHI260AP_BST_FLASH_DETECTED)) {
        LOG_WARN("BHI260AP: No external flash detected (boot status 0x%02x)", bootStatus);
        LOG_WARN("BHI260AP: Sensor is in HOST DOWNLOAD MODE - requires RAM firmware upload");

#ifdef HAS_BHI260AP_SENSORLIB
        // Attempt RAM-based firmware upload using SensorLib
        LOG_INFO("BHI260AP: SensorLib available - attempting RAM firmware upload...");
        LOG_WARN("BHI260AP: This will take ~30 seconds and must repeat on every boot");

        if (uploadFirmwareToRAM()) {
            LOG_INFO("BHI260AP: ✓ RAM firmware upload successful!");
            g_bhi260ap_instance = this;
            initialized = true;
            return true;
        } else {
            LOG_ERROR("BHI260AP: RAM firmware upload failed");
            g_bhi260ap_instance = this;
            initialized = true;
            return true; // Still return success for UI (will show 0 steps)
        }
#else
        // SensorLib not included - provide helpful error message
        LOG_ERROR("┌──────────────────────────────────────────────────────────────┐");
        LOG_ERROR("│ BHI260AP: NO EXTERNAL FLASH DETECTED                        │");
        LOG_ERROR("├──────────────────────────────────────────────────────────────┤");
        LOG_ERROR("│ Boot Status: 0x%02x (HOST DOWNLOAD MODE)                    │", bootStatus);
        LOG_ERROR("│                                                              │");
        LOG_ERROR("│ This board requires firmware uploaded to RAM on every boot.  │");
        LOG_ERROR("│ Two solutions available:                                     │");
        LOG_ERROR("│                                                              │");
        LOG_ERROR("│ RECOMMENDED: Embed firmware in build (AUTOMATIC)             │");
        LOG_ERROR("│   1. Download BHI260AP firmware file (~111KB):               │");
        LOG_ERROR("│      github.com/Xinyuan-LilyGO/T-Echo/raw/main/lib/         │");
        LOG_ERROR("│      SensorLib/examples/BHI260AP_UpdateFirmware/             │");
        LOG_ERROR("│      BHI260AP_aux_BMM150_BME280_GPIO-flash.fw               │");
        LOG_ERROR("│   2. Convert to header:                                      │");
        LOG_ERROR("│      cd variants/nrf52840/t-echo-plus                        │");
        LOG_ERROR("│      python3 firmware_to_header.py <firmware.fw>            │");
        LOG_ERROR("│      cp bhi260ap_firmware.h ../../../src/motion/            │");
        LOG_ERROR("│   3. Add to platformio.ini build_flags:                      │");
        LOG_ERROR("│      -DHAS_BHI260AP_FIRMWARE_EMBEDDED                        │");
        LOG_ERROR("│      -DHAS_BHI260AP_SENSORLIB                                │");
        LOG_ERROR("│   4. Add to lib_deps: lewisxhe/SensorLib@^0.2.0             │");
        LOG_ERROR("│   5. Patch SensorLib (see BHI260AP_RAM_UPLOAD_STATUS.md)    │");
        LOG_ERROR("│   6. Build & flash - sensor auto-programs on first boot!    │");
        LOG_ERROR("│                                                              │");
        LOG_ERROR("│ See: variants/nrf52840/t-echo-plus/firmware_to_header.py    │");
        LOG_ERROR("│                                                              │");
        LOG_ERROR("│ Step counter will display 0 until one solution is used.     │");
        LOG_ERROR("└──────────────────────────────────────────────────────────────┘");

        // Store instance anyway for UI (will show 0 steps)
        g_bhi260ap_instance = this;
        initialized = true;
        return true; // Return success so UI still shows (with 0 steps)
#endif
    }

    // If host interface is ready and flash detected, attempt boot
    if ((bootStatus & BHI260AP_BST_HOST_INTERFACE_READY) && (bootStatus & BHI260AP_BST_FLASH_DETECTED)) {
        LOG_INFO("BHI260AP: Host interface ready and flash detected, attempting boot...");

        // If flash verification is already done, proceed to boot
        if (!(bootStatus & BHI260AP_BST_FLASH_VERIFY_DONE)) {
            LOG_INFO("BHI260AP: Waiting for flash verification...");
            uint32_t startTime = millis();
            bool flashVerified = false;

            while ((millis() - startTime) < 3000) { // 3 second timeout
                if (!readRegister(wire, i2cAddress, BHI260AP_REG_BOOT_STATUS, &bootStatus, 1)) {
                    LOG_ERROR("BHI260AP: Failed to read boot status during verification");
                    return false;
                }

                LOG_DEBUG("BHI260AP: Boot status: 0x%02x", bootStatus);

                // Check if flash verification is done
                if (bootStatus & BHI260AP_BST_FLASH_VERIFY_DONE) {
                    flashVerified = true;
                    LOG_INFO("BHI260AP: Flash verification completed");
                    break;
                }

                // Check for verification error
                if (bootStatus & BHI260AP_BST_FLASH_VERIFY_ERROR) {
                    LOG_ERROR("BHI260AP: Flash verification error detected!");
                    return false;
                }

                delay(50);
            }

            if (!flashVerified) {
                LOG_ERROR("BHI260AP: Timeout waiting for flash verification");
                LOG_INFO("BHI260AP: Final boot status: 0x%02x", bootStatus);
                return false;
            }
        } else {
            LOG_INFO("BHI260AP: Flash already verified (status: 0x%02x)", bootStatus);
        }

        // Wait a moment for the sensor to stabilize after verification
        delay(100);

        // Read boot status again to check if firmware is now running
        if (!readRegister(wire, i2cAddress, BHI260AP_REG_BOOT_STATUS, &bootStatus, 1)) {
            LOG_ERROR("BHI260AP: Failed to read boot status after verification");
            return false;
        }

        LOG_INFO("BHI260AP: Post-verification boot status: 0x%02x", bootStatus);

        // Check if firmware is now running by reading chip ID
        if (readRegister(wire, i2cAddress, BHI260AP_REG_CHIP_ID, &chipId, 1)) {
            LOG_INFO("BHI260AP: Chip ID after verification: 0x%02x (expected 0x%02x)", chipId, BHI260AP_CHIP_ID);
            if (chipId == BHI260AP_CHIP_ID) {
                LOG_INFO("BHI260AP: ✓ Firmware booted successfully!");

                // Store global instance for renderer access
                g_bhi260ap_instance = this;
                initialized = true;
                return true;
            } else {
                LOG_ERROR("BHI260AP: Chip ID mismatch after boot - got 0x%02x", chipId);
            }
        } else {
            LOG_ERROR("BHI260AP: Failed to read chip ID after boot");
        }

        // If we get here, firmware didn't boot properly despite flash being present
        LOG_ERROR("BHI260AP: Firmware in flash but failed to boot - may need power cycle");
        LOG_ERROR("BHI260AP: Try disconnecting and reconnecting the device");
        return false;
    } else {
        LOG_ERROR("BHI260AP: Host interface not ready or flash not detected");
        LOG_ERROR("BHI260AP: Boot status: 0x%02x", bootStatus);
        LOG_ERROR("BHI260AP: Possible causes:");
        LOG_ERROR("BHI260AP:   1. Sensor needs power cycle (disconnect/reconnect USB)");
        LOG_ERROR("BHI260AP:   2. Flash firmware not uploaded (check PROGRAMMING_GUIDE.md)");
        LOG_ERROR("BHI260AP:   3. I2C communication issue");
        return false;
    }
}

// Add helper to decode boot status for debugging
static const char *decodeBootStatus(uint8_t status)
{
    static char buffer[128];
    snprintf(buffer, sizeof(buffer), "0x%02X [%s%s%s%s%s]", status, (status & 0x01) ? "FLASH_DET " : "",
             (status & 0x02) ? "VERIFY_DONE " : "", (status & 0x04) ? "VERIFY_ERR " : "", (status & 0x10) ? "HOST_RDY " : "",
             (status & 0x80) ? "FW_VERIFY_DONE" : "");
    return buffer;
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

#ifdef HAS_BHI260AP_SENSORLIB

// Minimal callback to capture step counter data
static void step_callback(uint8_t sensor_id, uint8_t *data_ptr, uint32_t len, uint64_t *timestamp)
{
    if (!g_bhi260ap_instance || len < 4)
        return;

    // Parse 32-bit step count
    uint32_t steps = data_ptr[0] | (data_ptr[1] << 8) | (data_ptr[2] << 16) | (data_ptr[3] << 24);
    if (steps < 1000000) { // Sanity check
        g_bhi260ap_instance->stepCount = steps;
    }
}

static void accel_callback(uint8_t sensor_id, uint8_t *data_ptr, uint32_t len, uint64_t *timestamp)
{
    if (!g_bhi260ap_instance)
        return;

    struct bhy2_data_xyz data;
    float scale = get_sensor_default_scaling(sensor_id);
    bhy2_parse_xyz(data_ptr, &data);

    g_bhi260ap_instance->accelX = data.x * scale;
    g_bhi260ap_instance->accelY = data.y * scale;
    g_bhi260ap_instance->accelZ = data.z * scale;
    g_bhi260ap_instance->hasAccelData = true;

    // Tilt-to-scroll detection using Y-axis (left/right tilt when upright)
    // When upright: X≈1.0 (gravity), Y≈0.0, Z≈0.0 at rest
    // Tilt left: Y increases positive (scroll UP)
    // Tilt right: Y goes negative (scroll DOWN)
    uint32_t now = millis();

    if (now - g_bhi260ap_instance->lastTiltScrollTime > 800) { // 800ms debounce
        float yAccel = data.y * scale;

        // Higher threshold to reduce sensitivity (±1.0 m/s²)
        if (yAccel > 1.0) {
            LOG_INFO("TILT LEFT Y=%.2f -> scroll UP", yAccel);
            if (inputBroker) {
                InputEvent e;
                e.inputEvent = INPUT_BROKER_UP;
                e.source = "tilt";
                inputBroker->queueInputEvent(&e);
            }
            g_bhi260ap_instance->lastTiltScrollTime = now;
        } else if (yAccel < -1.0) {
            LOG_INFO("TILT RIGHT Y=%.2f -> scroll DOWN", yAccel);
            if (inputBroker) {
                InputEvent e;
                e.inputEvent = INPUT_BROKER_DOWN;
                e.source = "tilt";
                inputBroker->queueInputEvent(&e);
            }
            g_bhi260ap_instance->lastTiltScrollTime = now;
        }
    }
}

static void gyro_callback(uint8_t sensor_id, uint8_t *data_ptr, uint32_t len, uint64_t *timestamp)
{
    if (!g_bhi260ap_instance)
        return;

    struct bhy2_data_xyz data;
    float scale = get_sensor_default_scaling(sensor_id);
    bhy2_parse_xyz(data_ptr, &data);

    g_bhi260ap_instance->gyroX = data.x * scale;
    g_bhi260ap_instance->gyroY = data.y * scale;
    g_bhi260ap_instance->gyroZ = data.z * scale;
    g_bhi260ap_instance->hasGyroData = true;
}

bool BHI260APSensor::uploadFirmwareToRAM()
{
    LOG_INFO("BHI260AP: Initializing SensorLib for firmware upload...");

    // Firmware source priority:
    // 1. Embedded firmware (if HAS_BHI260AP_FIRMWARE_EMBEDDED defined)
    // 2. Built-in SensorLib minimal firmware
    const uint8_t *firmwareData = nullptr;
    size_t firmwareSize = 0;
    bool writeToFlash = false;

#ifdef HAS_BHI260AP_FIRMWARE_EMBEDDED
    // Use embedded firmware - will program to external flash permanently
    firmwareData = bhi260ap_firmware_data;
    firmwareSize = bhi260ap_firmware_size;
    writeToFlash = true;
    LOG_INFO("BHI260AP: Using embedded firmware (%u bytes)", firmwareSize);
    LOG_INFO("BHI260AP: Will program to external flash chip (one-time operation)");
#else
    // Use built-in SensorLib firmware - RAM only
    firmwareData = bhy2_firmware_image;
    firmwareSize = sizeof(bhy2_firmware_image);
    writeToFlash = false;
    LOG_INFO("BHI260AP: Using built-in SensorLib firmware (%u bytes)", firmwareSize);
    LOG_WARN("BHI260AP: Firmware will be lost on power cycle (RAM only)");
#endif

    // Create or reuse persistent SensorLib instance
    SensorBHI260AP *bhi = nullptr;
    if (bhiInstance) {
        bhi = static_cast<SensorBHI260AP *>(bhiInstance);
    } else {
        bhi = new SensorBHI260AP();
        bhiInstance = bhi;
    }

    // Set reset pin to -1 (not used on T-Echo Plus)
    bhi->setPins(-1, -1);

    // Configure firmware
    bhi->setFirmware(firmwareData, firmwareSize, writeToFlash, true); // force_update=true
    bhi->setBootFromFlash(writeToFlash);

    LOG_INFO("BHI260AP: Uploading firmware (this takes ~30 seconds)...");
    uint32_t startTime = millis();

    // Upload firmware - init() triggers the upload when firmware is configured
    if (!bhi->init(*wire, PIN_WIRE_SDA, PIN_WIRE_SCL, BHI260AP_SLAVE_ADDRESS_L)) {
        LOG_ERROR("BHI260AP: Firmware upload failed after %lu ms", millis() - startTime);
        LOG_ERROR("BHI260AP: Error code: %d", bhi->getError());
        if (firmwareData)
            free(const_cast<void *>(static_cast<const void *>(firmwareData)));
        return false;
    }

    LOG_INFO("BHI260AP: Firmware uploaded in %lu ms", millis() - startTime);
    if (writeToFlash)
        LOG_INFO("BHI260AP: External flash programmed");

    delay(500);

    uint8_t bootStatus = 0;
    if (!readRegister(wire, i2cAddress, BHI260AP_REG_BOOT_STATUS, &bootStatus, 1)) {
        LOG_ERROR("BHI260AP: Boot status read failed");
        return false;
    }

    if (!(bootStatus & BHI260AP_BST_HOST_INTERFACE_READY)) {
        LOG_ERROR("BHI260AP: Not ready (0x%02x)", bootStatus);
        return false;
    }

    uint8_t chipId = 0;
    if (readRegister(wire, i2cAddress, BHI260AP_REG_CHIP_ID, &chipId, 1) && chipId == BHI260AP_CHIP_ID) {
        LOG_INFO("BHI260AP: Chip ID OK");
    } else {
        LOG_WARN("BHI260AP: Chip ID 0x%02x (expected 0x%02x)", chipId, BHI260AP_CHIP_ID);
    }

    // Enable accelerometer (required for step counter)
    if (!bhi->configure(SENSOR_ID_ACC_PASS, 10.0, 0)) {
        LOG_WARN("BHI260AP: Accel config failed");
    } else {
        bhi->onResultEvent(SENSOR_ID_ACC_PASS, accel_callback);
        LOG_INFO("BHI260AP: Accel enabled");
    }

    // Enable gyroscope
    if (!bhi->configure(SENSOR_ID_GYRO_PASS, 10.0, 0)) {
        LOG_WARN("BHI260AP: Gyro config failed");
    } else {
        bhi->onResultEvent(SENSOR_ID_GYRO_PASS, gyro_callback);
        LOG_INFO("BHI260AP: Gyro enabled");
    }

    if (!bhi->configure(BHI260AP_SENSOR_ID_STEP_COUNTER, 1.0, 0)) {
        LOG_WARN("BHI260AP: Step counter config failed");
    } else {
        bhi->onResultEvent((BhySensorID)BHI260AP_SENSOR_ID_STEP_COUNTER, step_callback);
        LOG_INFO("BHI260AP: Step counter OK");
    }

    return true;
}
#endif

int32_t BHI260APSensor::runOnce()
{
    if (!initialized || !wire) {
        return MOTION_SENSOR_CHECK_INTERVAL_MS;
    }

#ifdef HAS_BHI260AP_SENSORLIB
    // Update FIFO via SensorLib (processes callbacks)
    if (bhiInstance) {
        static_cast<SensorBHI260AP *>(bhiInstance)->update();
    }
#else
    // Fallback: direct FIFO read
    readStepCounterFromFifo();
#endif

    // Log sensor data every 5 seconds (faster updates for display)
    uint32_t now = millis();
    if (lastLogTime == 0 || (now - lastLogTime) >= 5000) {
#ifdef HAS_BHI260AP_SENSORLIB
        if (hasAccelData && hasGyroData) {
            LOG_INFO("BHI260AP: Accel[%.2f,%.2f,%.2f] Gyro[%.2f,%.2f,%.2f] Steps=%u", accelX, accelY, accelZ, gyroX, gyroY, gyroZ,
                     stepCount);
        } else if (hasAccelData) {
            LOG_INFO("BHI260AP: Accel[%.2f,%.2f,%.2f] Steps=%u", accelX, accelY, accelZ, stepCount);
        } else {
            LOG_INFO("BHI260AP: Steps=%u (waiting for sensor data)", stepCount);
        }
#else
        LOG_INFO("BHI260AP: Steps=%u", stepCount);
#endif
        lastLogTime = now;
    }

    return MOTION_SENSOR_CHECK_INTERVAL_MS;
}

#ifdef HAS_BHI260AP_SENSORLIB
// Getter function for UI renderer to access IMU data
extern "C" bool GetBHI260APDataForRenderer(float *accelX, float *accelY, float *accelZ, float *gyroX, float *gyroY, float *gyroZ)
{
    if (!g_bhi260ap_instance) {
        return false;
    }

    if (!g_bhi260ap_instance->hasAccelData || !g_bhi260ap_instance->hasGyroData) {
        return false;
    }

    *accelX = g_bhi260ap_instance->accelX;
    *accelY = g_bhi260ap_instance->accelY;
    *accelZ = g_bhi260ap_instance->accelZ;
    *gyroX = g_bhi260ap_instance->gyroX;
    *gyroY = g_bhi260ap_instance->gyroY;
    *gyroZ = g_bhi260ap_instance->gyroZ;

    return true;
}
#endif

#endif
