#include "BMM350Sensor.h"

#if !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C && !MESHTASTIC_EXCLUDE_MAGNETOMETER && __has_include(<DFRobot_BMM350.h>)

#include "detect/ScanI2CTwoWire.h"
#include <cmath>

#if !defined(MESHTASTIC_EXCLUDE_SCREEN)
extern std::unique_ptr<graphics::Screen> screen;
#endif

static constexpr float BMM350_MIN_AXIS_RADIUS = 1.0f;
static constexpr int32_t BMM350_POLL_INTERVAL_MS = 200;
// Gravity barely moves in a hand, so an older sample still tilt-corrects
static constexpr uint32_t BMM350_ACCEL_STALE_MS = 2000;
static constexpr float BMM350_MIN_GRAVITY_G = 0.5f;

TwoWire *BMM350Sensor::Sensor::bus = nullptr;
bool BMM350Sensor::Sensor::transferFailed = false;

BMM350Sensor::Sensor::Sensor(TwoWire *wire, uint8_t address) : DFRobot_BMM350(readRegisters, writeRegisters, delayUs, address)
{
    bus = wire;
}

int8_t BMM350Sensor::Sensor::readRegisters(uint8_t reg, uint8_t *data, uint32_t length, void *context)
{
    const uint8_t address = *static_cast<uint8_t *>(context);
    bus->beginTransmission(address);
    bus->write(reg);
    if (bus->endTransmission() != 0 || bus->requestFrom(address, static_cast<uint8_t>(length)) != length) {
        transferFailed = true;
        return -1;
    }
    for (uint32_t index = 0; index < length; ++index)
        data[index] = bus->read();
    return 0;
}

int8_t BMM350Sensor::Sensor::writeRegisters(uint8_t reg, const uint8_t *data, uint32_t length, void *context)
{
    const uint8_t address = *static_cast<uint8_t *>(context);
    bus->beginTransmission(address);
    const bool buffered = bus->write(reg) == 1 && bus->write(data, length) == length;
    const uint8_t status = bus->endTransmission();
    if (!buffered || status != 0) {
        transferFailed = true;
        return -1;
    }
    return 0;
}

void BMM350Sensor::Sensor::delayUs(uint32_t period, void *)
{
    if (period >= 1000)
        delay(period / 1000);
    if (period % 1000)
        delayMicroseconds(period % 1000);
}

uint8_t BMM350Sensor::Sensor::begin()
{
    transferFailed = false;
    return sensorInit() && !transferFailed ? 0 : 1;
}

bool BMM350Sensor::Sensor::configure()
{
    transferFailed = false;
    setPresetMode(BMM350_PRESETMODE_ENHANCED, BMM350_DATA_RATE_12_5HZ);
    setMeasurementXYZ();
    setOperationMode(eBmm350NormalMode);
    return !transferFailed;
}

bool BMM350Sensor::Sensor::readSample(sBmm350MagData_t &data)
{
    transferFailed = false;
    data = getGeomagneticData();
    return !transferFailed;
}

BMM350Sensor::BMM350Sensor(ScanI2C::FoundDevice foundDevice) : MotionSensor(foundDevice) {}

bool BMM350Sensor::init()
{
    if (!sensor)
        sensor = std::make_unique<Sensor>(ScanI2CTwoWire::fetchI2CBus(device.address), device.address.address);

    LOG_DEBUG("BMM350 begin on addr 0x%x (port=%d)", device.address.address, device.address.port);
    const uint8_t status = sensor->begin();
    if (status != 0) {
        LOG_WARN("BMM350 init error %u", status);
        return false;
    }

    if (!sensor->configure()) {
        LOG_WARN("BMM350 setup write failed");
        return false;
    }
    loadMagnetometerCalibration(compassCalibrationFileName, highestX, lowestX, highestY, lowestY, highestZ, lowestZ);
    return true;
}

bool BMM350Sensor::readMagnetometer(float &xMicroTesla, float &yMicroTesla, float &zMicroTesla)
{
    sBmm350MagData_t data = {};
    if (!sensor->readSample(data) || !std::isfinite(data.float_x) || !std::isfinite(data.float_y) ||
        !std::isfinite(data.float_z) || (data.float_x == 0.0f && data.float_y == 0.0f && data.float_z == 0.0f)) {
        return false;
    }

    xMicroTesla = data.float_x;
    yMicroTesla = data.float_y;
    zMicroTesla = data.float_z;
    return true;
}

int32_t BMM350Sensor::runOnce()
{
    float magX = 0, magY = 0, magZ = 0;
    if (!readMagnetometer(magX, magY, magZ)) {
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

    magX -= (highestX + lowestX) * 0.5f;
    magY -= (highestY + lowestY) * 0.5f;
    magZ -= (highestZ + lowestZ) * 0.5f;

    const float radiusX = (highestX - lowestX) * 0.5f;
    const float radiusY = (highestY - lowestY) * 0.5f;
    const float radiusZ = (highestZ - lowestZ) * 0.5f;
    const float avgRadius = (radiusX + radiusY + radiusZ) / 3.0f;
    magX *= (radiusX > BMM350_MIN_AXIS_RADIUS) ? (avgRadius / radiusX) : 1.0f;
    magY *= (radiusY > BMM350_MIN_AXIS_RADIUS) ? (avgRadius / radiusY) : 1.0f;
    magZ *= (radiusZ > BMM350_MIN_AXIS_RADIUS) ? (avgRadius / radiusZ) : 1.0f;

    // A Grove unit at 0x14 (M5Stack Unit DoF9) is levelled against the IMU's gravity. Its BMM350 is mounted rotated
    // 180 degrees about X, so bring it into the accelerometer's frame first.
    const bool groveUnit = device.address.address == BMM350_ADDR;
    if (groveUnit) {
        magY = -magY;
        magZ = -magZ;
    }

    publishCompassMagSample(magX, magY, magZ);

#if !defined(MESHTASTIC_EXCLUDE_SCREEN) && HAS_SCREEN
    float heading;
    bool tilt = false;
    if (groveUnit) {
        // Gravity from the IMU's thread, in g. Without a fresh sample, assume the unit lies flat, label up.
        float upX = 0.0f, upY = 0.0f, upZ = 1.0f;
        uint32_t accelAgeMs = 0;
        tilt = getLatestCompassAccelSample(upX, upY, upZ, accelAgeMs) && accelAgeMs <= BMM350_ACCEL_STALE_MS;
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
        // (away from the Grove connector).
        const float eastX = magY * upZ - magZ * upY;
        const float eastY = magZ * upX - magX * upZ;
        const float eastZ = magX * upY - magY * upX;
        const float northY = upZ * eastX - upX * eastZ;
        heading = atan2(double(eastY), double(northY)) * RAD_TO_DEG;
    } else {
        heading = atan2(double(magX), double(magY)) * RAD_TO_DEG;
    }
    if (heading < 0.0f)
        heading += 360.0f;
    // Levelled against gravity, a face-down mount turns the right way already; flat, only a mirror undoes it
    if (!tilt && config.display.compass_orientation > meshtastic_Config_DisplayConfig_CompassOrientation_DEGREES_270)
        heading = 360.0f - heading;
    if (screen)
        screen->setHeading(applyCompassOrientation(heading));
#endif
    return BMM350_POLL_INTERVAL_MS;
}

void BMM350Sensor::calibrate(uint16_t forSeconds)
{
    LOG_DEBUG("BMM350 calibration started for %us", forSeconds);
    seedPending = true;
    startCalibrationWindow(forSeconds);
}

#endif