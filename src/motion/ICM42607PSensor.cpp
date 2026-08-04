#include "ICM42607PSensor.h"

#if !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C && __has_include(<ICM42670P.h>)

#include "detect/ScanI2CTwoWire.h"
#include <ICM42670P.h>
#include <math.h>

// Boards with ICM_42607P_INT_PIN defined run at 12.5Hz instead - startWakeOnMotion() lowers the ODR.
// Boards that also define SHOW_STEP_COUNTER end up back at 50Hz, which the APEX pedometer requires.
static constexpr uint16_t ICM42607P_ACCEL_ODR_HZ = 50;
static constexpr uint16_t ICM42607P_ACCEL_FSR_G = 2;
static constexpr float ICM42607P_ACCEL_TO_COMPASS_ROTATION_DEG_VALUE =
#ifdef ICM42607P_ACCEL_TO_COMPASS_ROTATION_DEG
    ICM42607P_ACCEL_TO_COMPASS_ROTATION_DEG;
#else
    0.0f;
#endif

#ifdef SHOW_STEP_COUNTER
// startPedometer() requires a pin argument even when polling. It is never touched with a null handler.
static constexpr uint8_t ICM42607P_UNUSED_INT_PIN = 0;
#endif

#ifdef ICM_42607P_INT_PIN
volatile static bool ICM42607P_IRQ = false;

void ICM42607PSetInterrupt()
{
    ICM42607P_IRQ = true;
}
#endif

ICM42607PSensor::ICM42607PSensor(ScanI2C::FoundDevice foundDevice) : MotionSensor::MotionSensor(foundDevice) {}

ICM42607PSensor::~ICM42607PSensor() = default;

bool ICM42607PSensor::init()
{
    bool addressLsb = deviceAddress() == ICM42607P_ADDR_ALT;

    LOG_DEBUG("ICM-42607-P begin on addr 0x%02X (port=%d)", deviceAddress(), devicePort());
    TwoWire *wire = ScanI2CTwoWire::fetchI2CBus(device.address);
    sensor.reset();
    auto newSensor = std::make_unique<ICM42670>(*wire, addressLsb);

    int status = newSensor->begin();
    // ICM42670P library returns -3 for ICM42607P because WHO_AM_I differs; the register map is compatible.
    if (status != 0 && status != -3) {
        LOG_DEBUG("ICM-42607-P init error %d", status);
        return false;
    }

    status = newSensor->startAccel(ICM42607P_ACCEL_ODR_HZ, ICM42607P_ACCEL_FSR_G);
    if (status != 0) {
        LOG_DEBUG("ICM-42607-P accel start error %d", status);
        return false;
    }

#ifdef ICM_42607P_INT_PIN
    ICM42607P_IRQ = false;
    // startWakeOnMotion() leaves the accel running - it only drops it to low-power mode at
    // 12.5Hz - so the data registers keep updating for the compass tilt compensation below.
    status = newSensor->startWakeOnMotion(ICM_42607P_INT_PIN, ICM42607PSetInterrupt);
    if (status != 0) {
        LOG_DEBUG("ICM-42607-P wake-on-motion start error %d", status);
        return false;
    }
    LOG_DEBUG("ICM-42607-P wake-on-motion interrupt ok pin=%d", ICM_42607P_INT_PIN);
#endif

#ifdef SHOW_STEP_COUNTER
    // Must follow startWakeOnMotion(), which unconditionally disables the APEX pedometer. The reverse
    // order silently leaves the step counter off. WOM lives in its own register that the APEX path
    // never touches, so enabling the pedometer second keeps both alive on INT1. Passing a null handler
    // means we poll getPedometer() instead, so the pin argument is never used.
    status = newSensor->startPedometer(ICM42607P_UNUSED_INT_PIN, nullptr);
    if (status != 0) {
        LOG_DEBUG("ICM-42607-P pedometer start error %d", status);
        return false;
    }
    LOG_DEBUG("ICM-42607-P pedometer ok");
#endif

    sensor = std::move(newSensor);
    LOG_DEBUG("ICM-42607-P init ok");
    return true;
}

int32_t ICM42607PSensor::runOnce()
{
#ifdef ICM_42607P_INT_PIN
    if (ICM42607P_IRQ) {
        ICM42607P_IRQ = false;
        LOG_DEBUG("ICM-42607-P motion interrupt");
        wakeScreen();
    }
#endif

#if defined(SHOW_STEP_COUNTER) && !defined(MESHTASTIC_EXCLUDE_SCREEN) && HAS_SCREEN
    if (sensor != nullptr) {
        uint32_t stepCount = 0;
        float stepCadence = 0.0f;
        const char *activity = nullptr;
        // Returns non-zero when no step was detected since the last poll, leaving stepCount untouched.
        if (sensor->getPedometer(stepCount, stepCadence, activity) == 0 && stepCount != steps) {
            steps = stepCount;
            LOG_DEBUG("ICM-42607-P step count %u (%s)", steps, activity ? activity : "unknown");
            if (screen)
                screen->steps = steps;
        }
    }
#endif

    inv_imu_sensor_event_t event = {};

    if (sensor == nullptr || sensor->getDataFromRegisters(event) != 0) {
        return MOTION_SENSOR_CHECK_INTERVAL_MS;
    }

    // getDataFromRegisters() fills accel[] but does not set sensor_mask in this library version.
    if (event.accel[0] == 0 && event.accel[1] == 0 && event.accel[2] == 0) {
        return MOTION_SENSOR_CHECK_INTERVAL_MS;
    }

    float ax = static_cast<float>(event.accel[0]);
    float ay = static_cast<float>(event.accel[1]);
    const float az = static_cast<float>(event.accel[2]);

    if (ICM42607P_ACCEL_TO_COMPASS_ROTATION_DEG_VALUE != 0.0f) {
        static const float rotRad = ICM42607P_ACCEL_TO_COMPASS_ROTATION_DEG_VALUE * DEG_TO_RAD;
        static const float cosTheta = cosf(rotRad);
        static const float sinTheta = sinf(rotRad);
        const float rotatedX = (ax * cosTheta) - (ay * sinTheta);
        const float rotatedY = (ax * sinTheta) + (ay * cosTheta);
        ax = rotatedX;
        ay = rotatedY;
    }

    // Match the accel sign convention used by other FusionCompass sensor paths.
    publishCompassAccelSample(ax, -ay, -az);

    return MOTION_SENSOR_CHECK_INTERVAL_MS;
}

#endif
