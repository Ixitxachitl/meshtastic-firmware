#include "BMM350Sensor.h"

#if !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C && defined(HAS_BMM350) && __has_include(<DFRobot_BMM350.h>)
#include "detect/ScanI2CTwoWire.h"
#if !defined(MESHTASTIC_EXCLUDE_SCREEN)

// screen is defined in main.cpp
extern std::unique_ptr<graphics::Screen> screen;
#endif

static constexpr float BMM350_MIN_AXIS_RADIUS = 1.0f; // uT
static constexpr int32_t BMM350_POLL_INTERVAL_MS = 200;
// Gravity barely moves in a hand, so an older sample still tilt-corrects
static constexpr uint32_t BMM350_ACCEL_STALE_MS = 2000;
static constexpr float BMM350_MIN_GRAVITY_G = 0.5f;

BMM350Sensor::BMM350Sensor(ScanI2C::FoundDevice foundDevice) : MotionSensor::MotionSensor(foundDevice) {}

bool BMM350Sensor::init()
{
    // resolved via the scanner: WIRE1 may be a bridged bus rather than the local Wire1 (e.g. SenseCAP Indicator)
    TwoWire *bus = ScanI2CTwoWire::fetchI2CBus(device.address);
    sensor = new DFRobot_BMM350_I2C(bus, device.address.address);

    // begin() soft-resets the part and loads its OTP compensation coefficients
    LOG_DEBUG("BMM350 begin on addr 0x%02X (port=%d)", device.address.address, device.address.port);
    uint8_t status = sensor->begin();
    if (status != 0) {
        LOG_DEBUG("BMM350 init error %u", status);
        delete sensor;
        sensor = nullptr;
        return false;
    }

    // Continuous 12.5 Hz, 4x averaging
    sensor->setOperationMode(eBmm350NormalMode);
    sensor->setPresetMode(BMM350_PRESETMODE_ENHANCED, BMM350_DATA_RATE_12_5HZ);
    sensor->setMeasurementXYZ();

    loadMagnetometerCalibration(compassCalibrationFileName, highestX, lowestX, highestY, lowestY, highestZ, lowestZ);
    LOG_DEBUG("BMM350 calibration extrema: X=(%.0f, %.0f), Y=(%.0f, %.0f), Z=(%.0f, %.0f)", lowestX, highestX, lowestY, highestY,
              lowestZ, highestZ);
    return true;
}

bool BMM350Sensor::readMagnetometer(float &xMicroTesla, float &yMicroTesla, float &zMicroTesla)
{
    const sBmm350MagData_t data = sensor->getGeomagneticData();
    // The library swallows bus errors and returns a zeroed sample; one would wreck the calibration extrema
    if (data.float_x == 0.0f && data.float_y == 0.0f && data.float_z == 0.0f) {
        return false;
    }

    xMicroTesla = data.float_x;
    yMicroTesla = data.float_y;
    zMicroTesla = data.float_z;
    return true;
}

int32_t BMM350Sensor::runOnce()
{
#if !defined(MESHTASTIC_EXCLUDE_SCREEN) && HAS_SCREEN
    float magX = 0, magY = 0, magZ = 0;
    if (!readMagnetometer(magX, magY, magZ)) {
        // A run of rejected samples must not hold calibration open past its deadline
        if (doCalibration)
            finishCalibrationIfExpired(showingScreen, compassCalibrationFileName, highestX, lowestX, highestY, lowestY, highestZ,
                                       lowestZ);
        return BMM350_POLL_INTERVAL_MS;
    }

    if (doCalibration) {
        beginCalibrationDisplay(showingScreen);
        if (seedPending) {
            seedCalibrationExtrema(magX, magY, magZ, highestX, lowestX, highestY, lowestY, highestZ, lowestZ);
            seedPending = false;
        }
        updateCalibrationExtrema(magX, magY, magZ, highestX, lowestX, highestY, lowestY, highestZ, lowestZ);
        finishCalibrationIfExpired(showingScreen, compassCalibrationFileName, highestX, lowestX, highestY, lowestY, highestZ,
                                   lowestZ);
    }

    // Hard-iron bias removal.
    magX -= (highestX + lowestX) * 0.5f;
    magY -= (highestY + lowestY) * 0.5f;
    magZ -= (highestZ + lowestZ) * 0.5f;

    // Soft-iron diagonal scaling from calibration extrema.
    const float radiusX = (highestX - lowestX) * 0.5f;
    const float radiusY = (highestY - lowestY) * 0.5f;
    const float radiusZ = (highestZ - lowestZ) * 0.5f;
    const float avgRadius = (radiusX + radiusY + radiusZ) / 3.0f;
    magX *= (radiusX > BMM350_MIN_AXIS_RADIUS) ? (avgRadius / radiusX) : 1.0f;
    magY *= (radiusY > BMM350_MIN_AXIS_RADIUS) ? (avgRadius / radiusY) : 1.0f;
    magZ *= (radiusZ > BMM350_MIN_AXIS_RADIUS) ? (avgRadius / radiusZ) : 1.0f;

    // Into the accelerometer's frame: M5Stack's Grove BMM350 units mount it rotated 180 degrees about X
    magY = -magY;
    magZ = -magZ;

    // Publish the calibrated magnetometer values for the optional on-screen debug readout.
    publishCompassMagSample(magX, magY, magZ);

    // Gravity from the IMU's thread, in g. Without a fresh sample, assume the unit lies flat, label up.
    float upX = 0.0f, upY = 0.0f, upZ = 1.0f;
    uint32_t accelAgeMs = 0;
    bool tilt = getLatestCompassAccelSample(upX, upY, upZ, accelAgeMs) && accelAgeMs <= BMM350_ACCEL_STALE_MS;
    const float upNorm = sqrtf(upX * upX + upY * upY + upZ * upZ);
    tilt = tilt && upNorm > BMM350_MIN_GRAVITY_G; // free fall or a dead read has no "down" to level against
    if (tilt) {
        upX /= upNorm, upY /= upNorm, upZ /= upNorm;
        warnedNoTilt = false;
    } else {
        upX = 0.0f, upY = 0.0f, upZ = 1.0f;
        if (!warnedNoTilt) {
            warnedNoTilt = true;
            LOG_WARN("BMM350 has no fresh accel sample - compass running without tilt compensation");
        }
    }

    // East = field x up, north = up x east: both horizontal whatever the tilt. Heading is the bearing of +Y
    // (away from the Grove connector). Double atan2 on purpose: already linked.
    const float eastX = magY * upZ - magZ * upY;
    const float eastY = magZ * upX - magX * upZ;
    const float eastZ = magX * upY - magY * upX;
    const float northY = upZ * eastX - upX * eastZ;
    float heading = atan2(double(eastY), double(northY)) * RAD_TO_DEG;
    if (heading < 0.0f)
        heading += 360.0f;
    // Levelled against gravity, a face-down mount turns the right way already; flat, only a mirror undoes it
    if (!tilt && config.display.compass_orientation > meshtastic_Config_DisplayConfig_CompassOrientation_DEGREES_270)
        heading = 360.0f - heading;

    heading = applyCompassOrientation(heading);
    if (screen)
        screen->setHeading(heading);
#endif
    return BMM350_POLL_INTERVAL_MS;
}

void BMM350Sensor::calibrate(uint16_t forSeconds)
{
#if !defined(MESHTASTIC_EXCLUDE_SCREEN) && HAS_SCREEN
    LOG_DEBUG("BMM350 calibration started for %is", forSeconds);
    seedPending = true;
    startCalibrationWindow(forSeconds);
#endif
}

#endif
