#include "QMC6309Sensor.h"

#if !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C && __has_include(<SensorQMC6309.hpp>)

#include "Fusion/Fusion.h"
#include "detect/ScanI2CTwoWire.h"
#include "mesh/Throttle.h"
#include <math.h>
#if QMC6309_HARD_IRON_TRACKING
#include "NodeDB.h"
#endif

#if !defined(MESHTASTIC_EXCLUDE_SCREEN)
extern std::unique_ptr<graphics::Screen> screen;
#endif

static constexpr int32_t QMC6309_UPDATE_INTERVAL_MS = 20;
// Heading offset/flip below is a starting point copied from the MMC5983MA path; it is orientation-specific
// and must be verified/tuned against known North on real M9 hardware (see plan).
static constexpr float QMC6309_HEADING_OFFSET_DEG = 180.0f;
// Gravity barely moves in a hand, so an older sample still tilt-corrects; 300ms dropped it on every long UI redraw.
static constexpr uint32_t QMC6309_ACCEL_STALE_MS = 2000;
static constexpr float QMC6309_MIN_AXIS_RADIUS = 1e-4f;
static uint32_t qmc6309OverflowCount = 0;
#if QMC6309_HARD_IRON_TRACKING
static constexpr uint32_t QMC6309_HARD_IRON_ACCEL_MAX_AGE_MS = 250; // "down" has to match the magnetometer sample
static constexpr float QMC6309_HARD_IRON_REPORT_SHIFT = 0.05f;      // G
static constexpr uint32_t QMC6309_HARD_IRON_SAVE_INTERVAL_MS = 10UL * 60UL * 1000UL;
#endif

#ifdef COMPASS_SENSOR_DEBUG
// Averages a few fresh samples for the boot-time set/reset comparison below.
static bool averageQmc6309(SensorQMC6309 &sensor, float &x, float &y, float &z)
{
    MagnetometerData data;
    int got = 0;
    x = y = z = 0.0f;
    sensor.readData(data); // the first read after a mode change can still hold the old mode's sample
    for (int tries = 0; got < 8 && tries < 40; ++tries) {
        delay(12);
        if (sensor.readData(data) && !data.overflow) {
            x += data.magnetic_field.x;
            y += data.magnetic_field.y;
            z += data.magnetic_field.z;
            got++;
        }
    }
    if (got == 0)
        return false;
    x /= got;
    y /= got;
    z /= got;
    return true;
}

// Logs the field in each set/reset mode. Near Earth's 0.5 G with set+reset but many gauss set-only means the chip's
// own offset; many gauss in both means a magnet near the sensor. Keep the device still while it boots.
static void logSetResetModes(SensorQMC6309 &sensor)
{
    struct Step {
        SensorQMC6309::MagSetResetMode mode;
        const char *name;
    };
    const Step steps[] = {{SensorQMC6309::MagSetResetMode::SET_ONLY_ON, "set-only"},
                          {SensorQMC6309::MagSetResetMode::SET_AND_RESET_ON, "set+reset"},
                          {SensorQMC6309::MagSetResetMode::SET_ONLY_ON, "set-only again"}};
    for (const Step &step : steps) {
        sensor.setOperationMode(OperationMode::SUSPEND);
        sensor.setSetResetMode(step.mode);
        sensor.setOperationMode(OperationMode::CONTINUOUS_MEASUREMENT);
        delay(50);
        float x, y, z;
        if (averageQmc6309(sensor, x, y, z))
            LOG_INFO("QMC6309 mode test %s: (%.3f, %.3f, %.3f) |%.3f| G", step.name, x, y, z, sqrtf(x * x + y * y + z * z));
        else
            LOG_WARN("QMC6309 mode test %s: no data", step.name);
    }
}
#endif

QMC6309Sensor::QMC6309Sensor(ScanI2C::FoundDevice foundDevice) : MotionSensor::MotionSensor(foundDevice) {}

bool QMC6309Sensor::init()
{
    LOG_DEBUG("QMC6309 begin on addr 0x%02X (port=%d)", device.address.address, device.address.port);
    TwoWire *wire = ScanI2CTwoWire::fetchI2CBus(device.address);

    if (!sensor.begin(*wire, deviceAddress())) {
        LOG_DEBUG("QMC6309 init error");
        return false;
    }

    sensor.reset();

    // Force a SET pulse before every measurement. Left at the default, polarity came up differently on
    // successive boots (offset flipped by ~6G), silently invalidating the saved calibration each time.
    if (!sensor.setSetResetMode(SensorQMC6309::MagSetResetMode::SET_ONLY_ON))
        LOG_WARN("QMC6309 set/reset mode not applied; calibration may not survive a reboot");

    // 32 Gauss: the M9 reads ~18 G at rest in either set/reset mode, and one axis swung 20 G across a reboot.
    if (!sensor.configMagnetometer(OperationMode::CONTINUOUS_MEASUREMENT, MagFullScaleRange::FS_32G, 100.0f,
                                   MagOverSampleRatio::OSR_8)) {
        LOG_DEBUG("QMC6309 config failed");
        return false;
    }

#ifdef COMPASS_SENSOR_DEBUG
    logSetResetModes(sensor); // ends back in SET_ONLY_ON, continuous
#endif

    loadMagnetometerCalibration(compassCalibrationFileName, highestX, lowestX, highestY, lowestY, highestZ, lowestZ);
#if QMC6309_HARD_IRON_TRACKING
    resetHardIron();
#endif
    LOG_DEBUG("QMC6309 init ok");
    LOG_DEBUG("QMC6309 calibration extrema: X=(%.3f, %.3f), Y=(%.3f, %.3f), Z=(%.3f, %.3f)", lowestX, highestX, lowestY, highestY,
              lowestZ, highestZ);
    return true;
}

bool QMC6309Sensor::readMagnetometer(float &xGauss, float &yGauss, float &zGauss)
{
    MagnetometerData data;
    if (!sensor.readData(data)) {
        return false;
    }
    // A clipped axis would skew the heading and, during calibration, the saved extrema for good.
    if (data.overflow) {
        if (qmc6309OverflowCount++ == 0)
            LOG_WARN("QMC6309 overflow - dropping clipped samples");
        return false;
    }

    // magnetic_field is already scaled to Gauss by the driver.
    xGauss = data.magnetic_field.x;
    yGauss = data.magnetic_field.y;
    zGauss = data.magnetic_field.z;
    return true;
}

int32_t QMC6309Sensor::runOnce()
{
    float magX = 0, magY = 0, magZ = 0;
    if (!readMagnetometer(magX, magY, magZ)) {
        return QMC6309_UPDATE_INTERVAL_MS;
    }

#if !defined(MESHTASTIC_EXCLUDE_SCREEN)
    if (doCalibration) {
        beginCalibrationDisplay(showingScreen);
        updateCalibrationExtrema(magX, magY, magZ, highestX, lowestX, highestY, lowestY, highestZ, lowestZ);
        finishCalibrationIfExpired(showingScreen, compassCalibrationFileName, highestX, lowestX, highestY, lowestY, highestZ,
                                   lowestZ);
    }
#endif

#if QMC6309_HARD_IRON_TRACKING
    // During a calibration the extrema belong to it; a finished one restarts tracking from its own centre.
    if (doCalibration) {
        wasCalibrating = true;
    } else {
        if (wasCalibrating) {
            wasCalibrating = false;
            resetHardIron();
        }
        trackHardIron(magX, magY, magZ);
    }
#endif

#ifdef COMPASS_SENSOR_DEBUG
    const float rawX = magX, rawY = magY, rawZ = magZ;
#endif

    // Hard-iron bias removal.
    magX -= (highestX + lowestX) * 0.5f;
    magY -= (highestY + lowestY) * 0.5f;
    magZ -= (highestZ + lowestZ) * 0.5f;
#if QMC6309_HARD_IRON_TRACKING
    // Undo any axis the tracker found reversed this boot.
    magX *= hardIron.signX();
    magY *= hardIron.signY();
    magZ *= hardIron.signZ();
#endif
    // LOG_WARN("QMC6309 extrema=(%.3f, %.3f, %.3f) to (%.3f, %.3f, %.3f)",
    //          lowestX, lowestY, lowestZ, highestX, highestY, highestZ);

    // Soft-iron diagonal scaling from calibration extrema.
    const float radiusX = (highestX - lowestX) * 0.5f;
    const float radiusY = (highestY - lowestY) * 0.5f;
    const float radiusZ = (highestZ - lowestZ) * 0.5f;
    const float avgRadius = (radiusX + radiusY + radiusZ) / 3.0f;
    // magX *= (radiusX > QMC6309_MIN_AXIS_RADIUS) ? (avgRadius / radiusX) : 1.0f;
    // magY *= (radiusY > QMC6309_MIN_AXIS_RADIUS) ? (avgRadius / radiusY) : 1.0f;
    // magZ *= (radiusZ > QMC6309_MIN_AXIS_RADIUS) ? (avgRadius / radiusZ) : 1.0f;

    // Publish the calibrated magnetometer values (hard/soft-iron applied) for the optional on-screen debug readout.
    publishCompassMagSample(magX, magY, magZ);

#ifdef COMPASS_SENSOR_DEBUG
    // Once a second: |cal| should hold at the calibration radius; drift away from it with temp means the offset moved.
    static uint32_t lastDebugLogMs = 0;
    if (Throttle::hasElapsed(lastDebugLogMs, 1000)) {
        lastDebugLogMs = millis();
        float ax = 0.0f, ay = 0.0f, az = 0.0f;
        uint32_t ageMs = 0;
        const bool accelOk = getLatestCompassAccelSample(ax, ay, az, ageMs);
#ifdef ARCH_ESP32
        const float tempC = temperatureRead();
#else
        const float tempC = 0.0f;
#endif
#if QMC6309_HARD_IRON_TRACKING
        const unsigned fitCount = hardIron.fitCount();
#else
        constexpr unsigned fitCount = 0;
#endif
        LOG_INFO("QMC6309 dbg acc=(%.3f,%.3f,%.3f)%s raw=(%.3f,%.3f,%.3f) cal=(%.3f,%.3f,%.3f) |cal|=%.3f r=%.3f "
                 "c=(%.3f,%.3f,%.3f) T=%.1fC ovf=%u fits=%u",
                 ax, ay, az, accelOk && ageMs <= QMC6309_ACCEL_STALE_MS ? "" : "(stale)", rawX, rawY, rawZ, magX, magY, magZ,
                 sqrtf(magX * magX + magY * magY + magZ * magZ), avgRadius, (highestX + lowestX) * 0.5f,
                 (highestY + lowestY) * 0.5f, (highestZ + lowestZ) * 0.5f, tempC, (unsigned)qmc6309OverflowCount, fitCount);
    }
#endif

#if !defined(MESHTASTIC_EXCLUDE_SCREEN) && HAS_SCREEN
    float heading;
    float accelX = 0.0f;
    float accelY = 0.0f;
    float accelZ = 0.0f;
    uint32_t accelAgeMs = 0;
    static bool warnedStale = false;

    // Fuse with the latest accelerometer sample (published by the QMI8658 driver) for tilt compensation.
    const bool haveAccel = getLatestCompassAccelSample(accelX, accelY, accelZ, accelAgeMs);
    if (haveAccel && accelAgeMs <= QMC6309_ACCEL_STALE_MS) {
        FusionVector ga = {.axis = {accelX, accelY, accelZ}};
        FusionVector ma = {.axis = {magX, magY, magZ}};
        // if (config.display.compass_orientation > meshtastic_Config_DisplayConfig_CompassOrientation_DEGREES_270) {
        // ma = FusionAxesSwap(ma, FusionAxesAlignmentNXNYPZ);
        // ga = FusionAxesSwap(ga, FusionAxesAlignmentNXNYPZ);
        //}
        // LOG_WARN("QMC6309 accel age %ums, ga=(%.3f, %.3f, %.3f), ma=(%.3f, %.3f, %.3f)", accelAgeMs, ga.axis.x, ga.axis.y,
        //          ga.axis.z, ma.axis.x, ma.axis.y, ma.axis.z);
        heading = FusionCompass(ga, ma, FusionConventionNed);
        if (ga.axis.z > 0.0f)
            heading = 360.0f - heading;
        warnedStale = false;

    } else {
        // No usable accel sample: this is a flat heading, wrong at any angle but level. Silent
        // before, which made a stopped accelerometer thread look like a compass that lost calibration.
        if (!warnedStale) {
            warnedStale = true;
            if (haveAccel)
                LOG_WARN("QMC6309 accel sample %ums old - compass running without tilt compensation", accelAgeMs);
            else
                LOG_WARN("QMC6309 has no accel sample - compass running without tilt compensation");
        }
        heading = atan2f(-magY, magX) * RAD_TO_DEG;
    }

    if (heading >= 360.0f)
        heading -= 360.0f;
    else if (heading < 0.0f)
        heading += 360.0f;

    heading = applyCompassOrientation(heading);
    if (screen)
        screen->setHeading(heading);
#endif

    return QMC6309_UPDATE_INTERVAL_MS;
}

#if QMC6309_HARD_IRON_TRACKING
void QMC6309Sensor::resetHardIron()
{
    reportedCentre[0] = (highestX + lowestX) * 0.5f;
    reportedCentre[1] = (highestY + lowestY) * 0.5f;
    reportedCentre[2] = (highestZ + lowestZ) * 0.5f;
    const float radius = ((highestX - lowestX) + (highestY - lowestY) + (highestZ - lowestZ)) / 6.0f;
    hardIron.reset(reportedCentre[0], reportedCentre[1], reportedCentre[2], radius);
}

void QMC6309Sensor::trackHardIron(float magX, float magY, float magZ)
{
    float ax = 0.0f, ay = 0.0f, az = 0.0f;
    uint32_t ageMs = 0;
    if (getLatestCompassAccelSample(ax, ay, az, ageMs) && ageMs <= QMC6309_HARD_IRON_ACCEL_MAX_AGE_MS) {
        // Which way the field dips picks between mirrored fits; 0 until there is a position.
        const int8_t hemisphere = localPosition.latitude_i > 0 ? 1 : (localPosition.latitude_i < 0 ? -1 : 0);
        if (hardIron.addSample(magX, magY, magZ, ax, ay, az, millis(), hemisphere)) {
            // The extrema follow the centre, each axis keeping its calibrated radius.
            const float rx = (highestX - lowestX) * 0.5f, ry = (highestY - lowestY) * 0.5f, rz = (highestZ - lowestZ) * 0.5f;
            highestX = hardIron.centreX() + rx;
            lowestX = hardIron.centreX() - rx;
            highestY = hardIron.centreY() + ry;
            lowestY = hardIron.centreY() - ry;
            highestZ = hardIron.centreZ() + rz;
            lowestZ = hardIron.centreZ() - rz;

            if (hardIron.signChangeCount() != reportedSignChanges) {
                reportedSignChanges = hardIron.signChangeCount();
                LOG_WARN("QMC6309 axis polarity differs from calibration - using signs (%+d, %+d, %+d)", (int)hardIron.signX(),
                         (int)hardIron.signY(), (int)hardIron.signZ());
            }

            const float dx = hardIron.centreX() - reportedCentre[0];
            const float dy = hardIron.centreY() - reportedCentre[1];
            const float dz = hardIron.centreZ() - reportedCentre[2];
            if (dx * dx + dy * dy + dz * dz >= QMC6309_HARD_IRON_REPORT_SHIFT * QMC6309_HARD_IRON_REPORT_SHIFT) {
                LOG_INFO("QMC6309 hard-iron centre (%.3f, %.3f, %.3f) -> (%.3f, %.3f, %.3f), rms %.3f G over %u samples",
                         reportedCentre[0], reportedCentre[1], reportedCentre[2], hardIron.centreX(), hardIron.centreY(),
                         hardIron.centreZ(), hardIron.lastRms(), (unsigned)hardIron.sampleCount());
                reportedCentre[0] = hardIron.centreX();
                reportedCentre[1] = hardIron.centreY();
                reportedCentre[2] = hardIron.centreZ();
                hardIronSavePending = true;
            }
        }
    }

    // The first shift after boot is saved straight away; later ones at most every few minutes, to spare the flash.
    if (hardIronSavePending && (!hardIronSaved || Throttle::hasElapsed(lastHardIronSaveMs, QMC6309_HARD_IRON_SAVE_INTERVAL_MS))) {
        hardIronSavePending = false;
        hardIronSaved = true;
        lastHardIronSaveMs = millis();
        saveMagnetometerCalibration(compassCalibrationFileName, highestX, lowestX, highestY, lowestY, highestZ, lowestZ);
    }
}
#endif

void QMC6309Sensor::calibrate(uint16_t forSeconds)
{
#if !defined(MESHTASTIC_EXCLUDE_SCREEN)
    float xGauss = 0.0f;
    float yGauss = 0.0f;
    float zGauss = 0.0f;

    LOG_DEBUG("QMC6309 calibration started for %is", forSeconds);
    if (readMagnetometer(xGauss, yGauss, zGauss)) {
        seedCalibrationExtrema(xGauss, yGauss, zGauss, highestX, lowestX, highestY, lowestY, highestZ, lowestZ);
    } else {
        seedCalibrationExtrema(0.0f, 0.0f, 0.0f, highestX, lowestX, highestY, lowestY, highestZ, lowestZ);
    }
    startCalibrationWindow(forSeconds);
#endif
}

#endif
