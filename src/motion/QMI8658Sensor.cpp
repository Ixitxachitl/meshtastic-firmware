#include "QMI8658Sensor.h"

#if !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C && __has_include(<SensorQMI8658.hpp>)

#include "NodeDB.h"
#include "detect/ScanI2CTwoWire.h"
#include "mesh/Throttle.h"
#include <math.h>

#if defined(SHOW_STEP_COUNTER) && !defined(MESHTASTIC_EXCLUDE_SCREEN)
extern std::unique_ptr<graphics::Screen> screen;
#endif

// Accelerometer configuration. 2G full-scale gives the best gravity resolution for the tilt
// compensation that the (future) separate compass module will apply to these samples.
static constexpr SensorQMI8658::AccelRange QMI8658_ACCEL_RANGE = SensorQMI8658::ACC_RANGE_2G;
#ifdef SHOW_STEP_COUNTER
// Steps are detected from samples polled every 50ms: 62.5Hz with the ~8.4Hz filter below keeps step content
// intact and keeps faster noise from aliasing into that 20Hz poll.
static constexpr SensorQMI8658::AccelODR QMI8658_ACCEL_ODR = SensorQMI8658::ACC_ODR_62_5Hz;
#else
static constexpr SensorQMI8658::AccelODR QMI8658_ACCEL_ODR = SensorQMI8658::ACC_ODR_125Hz;
#endif

#ifdef QMI8658_SOFTWARE_MOTION_WAKE
// Deviation from averaged gravity that counts as motion, and how many samples in a row it must hold for.
// At rest the M9 shows ~110mg of variation, so a single sample over 150mg was waking it.
#ifndef QMI8658_SOFTWARE_MOTION_THRESHOLD_G
#define QMI8658_SOFTWARE_MOTION_THRESHOLD_G 0.35f
#endif
#ifndef QMI8658_SOFTWARE_MOTION_SAMPLES
#define QMI8658_SOFTWARE_MOTION_SAMPLES 2
#endif
static constexpr float QMI8658_GRAVITY_SMOOTHING = 0.1f;
#endif

#ifdef SHOW_STEP_COUNTER
// Counted in firmware: the chip's pedometer stayed at 0 on the M9 with ~2g walking peaks. A step is the
// magnitude rising this far above its slow average, at a cadence a person can actually walk.
#ifndef QMI8658_STEP_THRESHOLD_G
#define QMI8658_STEP_THRESHOLD_G 0.25f
#endif
#ifndef QMI8658_STEP_MIN_INTERVAL_MS
#define QMI8658_STEP_MIN_INTERVAL_MS 250 // quicker than 4 steps/s is a shake, not walking
#endif
#ifndef QMI8658_STEP_MAX_INTERVAL_MS
#define QMI8658_STEP_MAX_INTERVAL_MS 2000 // a longer gap ends the run
#endif
#ifndef QMI8658_STEP_RUN_TO_COUNT
#define QMI8658_STEP_RUN_TO_COUNT 4 // steps in a row before any count, so isolated jolts never add up
#endif
static constexpr float QMI8658_STEP_BASELINE_SMOOTHING = 0.05f;
#endif

// Any-motion slope threshold (in mg) used to wake the screen. Tunable: raise to reduce false wakes,
// lower to make it more sensitive. 200mg (~0.2g) requires a deliberate movement.
static constexpr float QMI8658_ANY_MOTION_THRESHOLD_MG = 200.0f;
static constexpr uint8_t QMI8658_ANY_MOTION_WINDOW = 1;

// Optional board-defined rotation (degrees) applied to the accel X/Y before publishing to the compass
// fusion path, mirroring the ICM42607P driver. Defaults to no rotation.
static constexpr float QMI8658_ACCEL_TO_COMPASS_ROTATION_DEG_VALUE =
#ifdef QMI8658_ACCEL_TO_COMPASS_ROTATION_DEG
    QMI8658_ACCEL_TO_COMPASS_ROTATION_DEG;
#else
    0.0f;
#endif

QMI8658Sensor::QMI8658Sensor(ScanI2C::FoundDevice foundDevice) : MotionSensor::MotionSensor(foundDevice) {}

bool QMI8658Sensor::init()
{
    LOG_DEBUG("QMI8658 begin on addr 0x%02X (port=%d)", deviceAddress(), devicePort());
    TwoWire *wire = ScanI2CTwoWire::fetchI2CBus(device.address);

    if (!sensor.begin(*wire, deviceAddress())) {
        LOG_DEBUG("QMI8658 init failed");
        return false;
    }

#ifdef SHOW_STEP_COUNTER
    // LPF_MODE_3 is 13.37% of ODR, ~8.4Hz: MODE_0 (~1.7Hz) sits on walking cadence, and OFF was noisy.
    sensor.configAccelerometer(QMI8658_ACCEL_RANGE, QMI8658_ACCEL_ODR, SensorQMI8658::LPF_MODE_3);
#else
    sensor.configAccelerometer(QMI8658_ACCEL_RANGE, QMI8658_ACCEL_ODR, SensorQMI8658::LPF_MODE_0);
#endif
    sensor.enableAccelerometer();

#ifndef QMI8658_SOFTWARE_MOTION_WAKE
    // Armed regardless of the setting: it can be switched on at runtime without init() running again,
    // and wakeScreen() checks it live. configMotion() keeps accel data flowing for the compass.
    const uint8_t modeCtrl = SensorQMI8658::ANY_MOTION_EN_X | SensorQMI8658::ANY_MOTION_EN_Y | SensorQMI8658::ANY_MOTION_EN_Z;
    // No-motion detection stays off (unreliable per the SensorLib example); its arguments are still required.
    sensor.configMotion(modeCtrl, QMI8658_ANY_MOTION_THRESHOLD_MG, QMI8658_ANY_MOTION_THRESHOLD_MG,
                        QMI8658_ANY_MOTION_THRESHOLD_MG, QMI8658_ANY_MOTION_WINDOW, /*NoMotion X/Y/Z*/ 0.1f, 0.1f, 0.1f,
                        /*NoMotionWindow*/ 1, /*SigMotionWaitWindow*/ 1, /*SigMotionConfirmWindow*/ 1);
#endif
#ifndef QMI8658_SOFTWARE_MOTION_WAKE
    // Enabled after configMotion(), which briefly stops the accelerometer that this needs running.
    sensor.enableMotionDetect();
#endif

    LOG_DEBUG("QMI8658 init ok");
    return true;
}

int32_t QMI8658Sensor::runOnce()
{
    float ax, ay, az;
    if (sensor.getAccelerometer(ax, ay, az)) {
        if (QMI8658_ACCEL_TO_COMPASS_ROTATION_DEG_VALUE != 0.0f) {
            static const float rotRad = QMI8658_ACCEL_TO_COMPASS_ROTATION_DEG_VALUE * DEG_TO_RAD;
            static const float cosTheta = cosf(rotRad);
            static const float sinTheta = sinf(rotRad);
            const float rotatedX = (ax * cosTheta) - (ay * sinTheta);
            const float rotatedY = (ax * sinTheta) + (ay * cosTheta);
            ax = rotatedX;
            ay = rotatedY;
        }

        // Match the accel sign convention used by the other FusionCompass sensor paths (e.g. ICM42607P).
        // The final handedness must be verified against the QMI8658 datasheet and real calibration once the
        // separate compass module is wired up; do not hand-tune the signs before then.
        publishCompassAccelSample(ax, ay, az);

#ifdef SHOW_STEP_COUNTER
        detectStep(sqrtf(ax * ax + ay * ay + az * az));
#endif

#ifdef QMI8658_SOFTWARE_MOTION_WAKE
        // Motion is a sample straying from averaged gravity. Magnitude only, so the rotation above is irrelevant.
        if (!gravityPrimed) {
            gravityX = ax, gravityY = ay, gravityZ = az;
            gravityPrimed = true;
        }
        const float dx = ax - gravityX, dy = ay - gravityY, dz = az - gravityZ;
        if (dx * dx + dy * dy + dz * dz > QMI8658_SOFTWARE_MOTION_THRESHOLD_G * QMI8658_SOFTWARE_MOTION_THRESHOLD_G) {
            // Held across consecutive samples, so a single bump or tap on the table doesn't count.
            if (++motionSamples >= QMI8658_SOFTWARE_MOTION_SAMPLES) {
                motionSamples = 0;
                wakeScreen(); // no-op unless the screen is dark and wake-on-motion is set
            }
        } else {
            motionSamples = 0;
        }
        gravityX += (ax - gravityX) * QMI8658_GRAVITY_SMOOTHING;
        gravityY += (ay - gravityY) * QMI8658_GRAVITY_SMOOTHING;
        gravityZ += (az - gravityZ) * QMI8658_GRAVITY_SMOOTHING;
#endif
    }

#ifndef QMI8658_SOFTWARE_MOTION_WAKE
    if (sensor.getStatusRegister() & SensorQMI8658::EVENT_ANY_MOTION)
        wakeScreen(); // checks wake_on_tap_or_motion itself
#endif

#if defined(SHOW_STEP_COUNTER) && !defined(MESHTASTIC_EXCLUDE_SCREEN) && HAS_SCREEN
    if (screen)
        screen->steps = stepCount;
#endif

    return MOTION_SENSOR_CHECK_INTERVAL_MS;
}

#ifdef SHOW_STEP_COUNTER
void QMI8658Sensor::detectStep(float magnitudeG)
{
    const float excursion = magnitudeG - stepBaselineG;
    stepBaselineG += excursion * QMI8658_STEP_BASELINE_SMOOTHING;

    if (!stepArmed) {
        // Re-armed only once the signal falls back through its average, so one impact counts once.
        if (excursion < 0.0f)
            stepArmed = true;
        return;
    }
    if (excursion < QMI8658_STEP_THRESHOLD_G)
        return;
    stepArmed = false;

    if (stepRun > 0 && Throttle::isWithinTimespanMs(lastStepMs, QMI8658_STEP_MIN_INTERVAL_MS))
        return; // too soon after the last peak: the same impact, or shaking
    if (stepRun > 0 && Throttle::hasElapsed(lastStepMs, QMI8658_STEP_MAX_INTERVAL_MS))
        stepRun = 0; // the previous run ended
    lastStepMs = millis();

    if (stepRun < QMI8658_STEP_RUN_TO_COUNT) {
        if (++stepRun == QMI8658_STEP_RUN_TO_COUNT)
            stepCount += QMI8658_STEP_RUN_TO_COUNT; // the run proved real: credit the steps that proved it
        return;
    }
    stepCount++;
}
#endif

#endif
