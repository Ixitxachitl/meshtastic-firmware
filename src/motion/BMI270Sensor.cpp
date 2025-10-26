#include "BMI270Sensor.h"
#include "detect/ScanI2C.h"
#include "graphics/draw/Math3D.h"

// Global variables for magnetometer heading (shared with BMM150Sensor and other mag sensors)
extern "C" {
extern volatile bool g_hasMagHeading;
extern volatile float g_magHeadingRad; // radians, 0 = North, +CW
}

#if !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C && __has_include(<bmi2.h>)

// Use tinyu-zhao BMI270 library exclusively
#include <Arduino.h>
#include <Wire.h>
#include <bmi2.h>
#include <bmi270.h>
#include <bmi2_defs.h>
#include <math.h>

#define HAS_BMI270_TINYU 1

// Motion detection using BMI270 library API only

#if HAS_BMI270_TINYU

// ------------------------- Private impl (I2C bridge) -------------------------
struct BMI270Impl {
    bmi2_dev dev{};
    uint8_t addr = 0x00;   // 0x69 or 0x68
    TwoWire *wire = &Wire; // default bus
};

// Simplified I2C helper for device detection
static bool i2cRead1(TwoWire &w, uint8_t addr, uint8_t reg, uint8_t &val)
{
    w.beginTransmission(addr);
    w.write(reg);
    if (w.endTransmission(false) != 0)
        return false;
    if (w.requestFrom(addr, (uint8_t)1) != 1)
        return false;
    val = w.read();
    return true;
}

// Bosch BMI2 API callbacks - required by library
static BMI2_INTF_RETURN_TYPE boschI2CRead(uint8_t reg, uint8_t *buf, uint32_t len, void *intf_ptr)
{
    auto *impl = static_cast<BMI270Impl *>(intf_ptr);
    impl->wire->beginTransmission(impl->addr);
    impl->wire->write(reg);
    if (impl->wire->endTransmission(false) != 0)
        return BMI2_E_COM_FAIL;
    if (impl->wire->requestFrom(impl->addr, (uint8_t)len) != len)
        return BMI2_E_COM_FAIL;
    for (uint32_t i = 0; i < len; ++i)
        buf[i] = impl->wire->read();
    return BMI2_OK;
}

static BMI2_INTF_RETURN_TYPE boschI2CWrite(uint8_t reg, const uint8_t *buf, uint32_t len, void *intf_ptr)
{
    auto *impl = static_cast<BMI270Impl *>(intf_ptr);
    impl->wire->beginTransmission(impl->addr);
    impl->wire->write(reg);
    impl->wire->write(buf, len);
    return (impl->wire->endTransmission() == 0) ? BMI2_OK : BMI2_E_COM_FAIL;
}

static void boschDelayUs(uint32_t us, void *)
{
    delayMicroseconds(us);
}

// Probe 0x69 then 0x68 (WHOAMI 0x24)
static bool selectAddress(BMI270Impl &impl)
{
    uint8_t who = 0;
    for (uint8_t a : {(uint8_t)0x69, (uint8_t)0x68}) {
        if (i2cRead1(*(impl.wire), a, 0x00, who)) { // <-- impl.wire (not impl->wire)
            LOG_DEBUG("BMIx probe: addr 0x%02X WHOAMI 0x%02X", a, who);
            if (who == 0x24) {
                impl.addr = a;
                return true;
            }
        }
    }
    return false;
}

// ------------------- Fake compass (gyro-only) – file-scope state -------------------
namespace
{
constexpr float G_LPF_ALPHA = 0.1f;    // accel LPF for gravity dir
constexpr float G_VALID_TOL_G = 0.20f; // accept accel norm ~ 1g ± tol

// Gravity (LPF) and gyro bias as full vectors
float s_gxLP = 0.0f, s_gyLP = 0.0f, s_gzLP = 1.0f; // init world-up ~ +Z
float s_biasX = 0.0f, s_biasY = 0.0f, s_biasZ = 0.0f;

// Tuning for fake compass drift control
constexpr float GYRO_DRIFT_TRIM = 0.005f; // small nudge while still
constexpr float STILL_THR_DPS = 0.5f;     // consider still if |g| sum < this

// Persistent state across runOnce() (removed software motion detection variables)

// Compass state
float s_yawDeg = 0.0f;          // integrated yaw angle
float s_yawZeroDeg = 0.0f;      // user-defined "north" reference
uint32_t s_lastMicros = 0;      // timestamp for integration
bool s_showingCalUI = false;    // UI state guard
bool s_anchorRequested = false; // instant calibration flag
} // namespace

#endif // HAS_BMI270_TINYU

// -----------------------------------------------------------------------------

BMI270Sensor::BMI270Sensor(ScanI2C::FoundDevice foundDevice) : MotionSensor::MotionSensor(foundDevice) {}

// ---- 3D compass handoff for CompassRenderer (no UI plumbing needed) ----
static BMI270Sensor *g_bmi270_instance = nullptr;
extern "C" Quat GetAttitudeForRenderer()
{
    return (g_bmi270_instance) ? g_bmi270_instance->getAttitudeQuat() : Quat::identity();
}

// Return the current step count for UI display
extern "C" uint32_t GetStepCountForRenderer()
{
    return (g_bmi270_instance) ? g_bmi270_instance->getStepCount() : 0;
}

// Check if step counter hardware is available
extern "C" bool HasStepCounterForRenderer()
{
    return (g_bmi270_instance != nullptr);
}

// Return the current (unit) gravity vector in BODY frame from the LPF accel
extern "C" Vec3 GetGravityForRenderer()
{
#if HAS_BMI270_TINYU
    Vec3 g(s_gxLP, s_gyLP, s_gzLP);
    float n = g.norm();
    if (n > 1e-3f) {
        g = g * (1.0f / n);
    } else {
        // Fallback to default gravity down
        g = Vec3(0, 0, 1);
    }
    return g;
#else
    return Vec3(0, 0, 1); // Default gravity vector pointing down
#endif
}

extern "C" void GetGravityXYZ(float *gx, float *gy, float *gz)
{
    Vec3 g = GetGravityForRenderer(); // already normalized unit gravity
    if (gx)
        *gx = g.x;
    if (gy)
        *gy = g.y;
    if (gz)
        *gz = g.z;
}

// Return the current heading in radians, preferring magnetometer data if available.
// Heading increases clockwise, '0' = North at top.
extern "C" float GetHeadingRadiansForRenderer()
{
    // Prefer real magnetometer data if available from BMM150 or other mag sensors
    if (g_hasMagHeading) {
        return g_magHeadingRad;
    }

#if HAS_BMI270_TINYU
    // Fall back to gyro-based fake compass when no magnetometer is available
    float rel = s_yawDeg - s_yawZeroDeg;
    while (rel < 0.0f)
        rel += 360.0f;
    while (rel >= 360.0f)
        rel -= 360.0f;
    float headingDeg = 360.0f - rel; // matches your screen heading
    if (headingDeg >= 360.0f)
        headingDeg -= 360.0f;
    return headingDeg * (float)M_PI / 180.0f;
#else
    return 0.0f;
#endif
}

bool BMI270Sensor::setupMotionDetection()
{
#if HAS_BMI270_TINYU
    auto *impl = static_cast<BMI270Impl *>(impl_);
    if (!impl)
        return false;

    // Enable motion detection features
    uint8_t sens_list[] = {BMI2_ANY_MOTION, BMI2_STEP_COUNTER};
    if (bmi270_sensor_enable(sens_list, 2, &impl->dev) != BMI2_OK) {
        LOG_WARN("BMI270: Failed to enable motion features");
        return false;
    }

    // Configure any-motion with optimized settings
    bmi2_sens_config config;
    config.type = BMI2_ANY_MOTION;
    config.cfg.any_motion.threshold = 200; // Low sensitivity (0-1023, higher = less sensitive)
    config.cfg.any_motion.duration = 4;    // 80ms duration (4 * 20ms)
    config.cfg.any_motion.select_x = 1;    // Enable X-axis
    config.cfg.any_motion.select_y = 1;    // Enable Y-axis
    config.cfg.any_motion.select_z = 1;    // Enable Z-axis

    if (bmi270_set_sensor_config(&config, 1, &impl->dev) == BMI2_OK) {
        LOG_INFO("BMI270: Motion detection configured (threshold=200, duration=80ms)");
    }

    // Map features to INT1 for interrupt-driven operation
    bmi2_map_feat_int(BMI2_ANY_MOTION, BMI2_INT1, &impl->dev);
    bmi2_map_feat_int(BMI2_STEP_COUNTER, BMI2_INT1, &impl->dev);

    LOG_INFO("BMI270: Motion features mapped to INT1");
    return true;
#else
    return false;
#endif
}

uint32_t BMI270Sensor::getStepCount() const
{
#if HAS_BMI270_TINYU
    auto *impl = static_cast<BMI270Impl *>(impl_);
    if (!impl)
        return 0;

    // Get step counter data
    struct bmi2_feat_sensor_data sensor_data;
    sensor_data.type = BMI2_STEP_COUNTER;

    int8_t result = bmi270_get_feature_data(&sensor_data, 1, &impl->dev);
    if (result == BMI2_OK) {
        return sensor_data.sens_data.step_counter_output;
    } else {
        LOG_WARN("BMI270: Failed to read step count (%d)", result);
        return 0;
    }
#else
    return 0;
#endif
}

bool BMI270Sensor::init()
{
#if !HAS_BMI270_TINYU
    LOG_DEBUG("BMI270: tinyu-zhao library not available in this build");
    return false;
#else
    auto *impl = new BMI270Impl();
    if (!impl)
        return false;

    impl->wire = &Wire;

    // Device detection and soft reset
    uint8_t SOFT_RESET = 0xB6;
    boschI2CWrite(0x7E, &SOFT_RESET, 1, impl);
    delay(10);

    if (!selectAddress(*impl)) {
        LOG_DEBUG("BMI270: Device not found (WHOAMI 0x24 not at 0x69/0x68)");
        delete impl;
        return false;
    }

    // Initialize BMI2 device structure
    impl->dev.intf = BMI2_I2C_INTF;
    impl->dev.read = boschI2CRead;
    impl->dev.write = boschI2CWrite;
    impl->dev.delay_us = boschDelayUs;
    impl->dev.intf_ptr = impl;
    impl->dev.read_write_len = 16;
    impl->dev.config_file_ptr = NULL;

    // Initialize BMI270 with retry
    int8_t err = bmi270_init(&impl->dev);
    if (err != BMI2_OK) {
        bmi2_soft_reset(&impl->dev);
        delay(10);
        err = bmi270_init(&impl->dev);
        if (err != BMI2_OK) {
            LOG_DEBUG("BMI270: Init failed (%d) at 0x%02X", err, impl->addr);
            delete impl;
            return false;
        }
    }

    // Enable accel + gyro
    uint8_t sensors[] = {BMI2_ACCEL, BMI2_GYRO};
    err = bmi270_sensor_enable(sensors, 2, &impl->dev);
    if (err != BMI2_OK) {
        LOG_DEBUG("BMI270: sensor_enable failed (%d)", err);
        delete impl;
        return false;
    }

    // Configure sensors with optimal settings for motion tracking
    bmi2_sens_config cfgs[2] = {
        {.type = BMI2_ACCEL, .cfg = {.acc = {BMI2_ACC_ODR_100HZ, BMI2_ACC_RANGE_4G, BMI2_ACC_NORMAL_AVG4}}},
        {.type = BMI2_GYRO, .cfg = {.gyr = {BMI2_GYR_ODR_100HZ, BMI2_GYR_RANGE_2000, BMI2_GYR_NORMAL_MODE}}}};

    if (bmi270_set_sensor_config(cfgs, 2, &impl->dev) != BMI2_OK) {
        delete impl;
        return false;
    }

    // Allow sensors to settle
    delay(50);

    // Initialize compass state and gravity vector
    s_yawDeg = 0.0f;
    s_yawZeroDeg = 0.0f;
    s_lastMicros = 0;
    s_showingCalUI = false;
    s_anchorRequested = false;

    // Initialize gravity vector by taking an immediate reading
    bmi2_sens_data initial_data{};
    if (bmi2_get_sensor_data(&initial_data, &impl->dev) == BMI2_OK) {
        const float aScale = 4.0f / 32768.0f; // ±4 g
        const float ax = -initial_data.acc.x * aScale;
        const float ay = initial_data.acc.y * aScale;
        const float az = initial_data.acc.z * aScale;

        // Initialize gravity LPF with current accelerometer reading
        float aMag = sqrtf(ax * ax + ay * ay + az * az);
        if (aMag > 0.1f) { // Valid acceleration reading
            s_gxLP = ax / aMag;
            s_gyLP = ay / aMag;
            s_gzLP = az / aMag;
            LOG_DEBUG("BMI270: Initialized gravity vector (%.3f, %.3f, %.3f)", s_gxLP, s_gyLP, s_gzLP);
        }
    }

    impl_ = impl;
    inited_ = true;
    g_bmi270_instance = this; // allow renderer to fetch attitude

    // Setup motion detection
    setupMotionDetection();

    LOG_DEBUG("BMI270: init ok at 0x%02X", impl->addr);
    return true;
#endif
}

int32_t BMI270Sensor::runOnce()
{
#if HAS_BMI270_TINYU
    if (inited_ && impl_) {

        // --- Calibration UI handling (same pattern as BMX160) ---
#if !defined(MESHTASTIC_EXCLUDE_SCREEN) && HAS_SCREEN
        if (doCalibration) {
            if (!s_showingCalUI) {
                powerFSM.trigger(EVENT_PRESS); // keep screen awake during calibration
                s_showingCalUI = true;
                if (screen)
                    screen->startAlert((FrameCallback)drawFrameCalibration);
            }
        } else {
            if (s_showingCalUI) {
                if (screen)
                    screen->endAlert();
                s_showingCalUI = false;
            }
        }
#endif

        auto *impl = static_cast<BMI270Impl *>(impl_);

        // Check BMI270 motion detection and step counter status using the library
        uint16_t int_status = 0;
        int8_t status_result = bmi2_get_int_status(&int_status, &impl->dev);

        if (status_result == BMI2_OK) {
            if (int_status & BMI270_ANY_MOT_STATUS_MASK) {
                LOG_INFO("BMI270: Motion detected (status=0x%04X)", int_status);
                wakeScreen();
                powerFSM.trigger(EVENT_PRESS);
            }

            if (int_status & BMI270_STEP_CNT_STATUS_MASK) {
                LOG_INFO("BMI270: Step counter interrupt (status=0x%04X)", int_status);
            }
        }

        // Log each individual step as it happens
        static uint32_t last_step_count = 0;
        uint32_t current_steps = getStepCount();
        if (current_steps > last_step_count) {
            uint32_t new_steps = current_steps - last_step_count;
            for (uint32_t i = 0; i < new_steps; i++) {
                LOG_INFO("BMI270: Step detected! Total steps: %u", last_step_count + i + 1);
            }
            last_step_count = current_steps;
        }
        bmi2_sens_data raw{};
        if (bmi2_get_sensor_data(&raw, &impl->dev) == BMI2_OK) {

            // Convert according to ranges set in init()
            const float aScale = 4.0f / 32768.0f;    // ±4 g
            const float gScale = 2000.0f / 32768.0f; // 2000 dps

            const float ax = -raw.acc.x * aScale;
            const float ay = raw.acc.y * aScale;
            const float az = raw.acc.z * aScale;

            const float gx = raw.gyr.x * gScale;
            const float gy = raw.gyr.y * gScale;
            const float gz = raw.gyr.z * gScale;

            // Hardware interrupts only - no software motion detection

            // ------------------------ Fake compass (gyro-only yaw) ------------------------
            // ---- Instant anchor: lock current facing as "north" (no countdown) ----
            if (s_anchorRequested) {
                // 1) Freeze gyro bias to current sample (assumes user is holding still)
                s_biasX = gx;
                s_biasY = gy;
                s_biasZ = gz;

                // 2) Make "north" *immediately* equal to what we're facing now
                s_yawZeroDeg = s_yawDeg;
                s_yawDeg = s_yawZeroDeg; // ensures rel = 0 this frame (instant snap)

                // 3) Guard against a big first dt after anchoring
                s_lastMicros = micros();

                // 4) Clear request and close any lingering UI
                s_anchorRequested = false;
#if !defined(MESHTASTIC_EXCLUDE_SCREEN) && HAS_SCREEN
                if (screen && !ScanI2C::hasMagnetometer()) {
                    screen->setHeading(0.0f); // force the snap visually this frame
                    screen->forceDisplay(true);
                }
                if (screen)
                    screen->endAlert();
#endif

                LOG_DEBUG("BMI270: anchored & snapped (bias=%.3f, %.3f, %.3f dps, zero=%.1f deg)", s_biasX, s_biasY, s_biasZ,
                          s_yawZeroDeg);
            }

            // --- Tilt-compensated yaw integration ---
            uint32_t nowUs = micros();
            if (s_lastMicros == 0)
                s_lastMicros = nowUs;
            float dt = (nowUs - s_lastMicros) * 1e-6f;
            s_lastMicros = nowUs;

            // 1) Always update gravity estimate with low-pass filter (like before)
            float aMag = sqrtf(ax * ax + ay * ay + az * az); // g
            bool aLooksLikeGravity = fabsf(aMag - 1.0f) < G_VALID_TOL_G;

            // Always update gravity vector (not tied to calibration)
            s_gxLP = s_gxLP + G_LPF_ALPHA * (ax - s_gxLP);
            s_gyLP = s_gyLP + G_LPF_ALPHA * (ay - s_gyLP);
            s_gzLP = s_gzLP + G_LPF_ALPHA * (az - s_gzLP);
            // normalize to unit
            float gn = sqrtf(s_gxLP * s_gxLP + s_gyLP * s_gyLP + s_gzLP * s_gzLP);
            if (gn > 1e-3f) {
                s_gxLP /= gn;
                s_gyLP /= gn;
                s_gzLP /= gn;
            }

            // 2) Project gyro onto gravity to get world-yaw rate
            float gxUnb = gx - s_biasX;
            float gyUnb = gy - s_biasY;
            float gzUnb = gz - s_biasZ;
            float yawRateDegPerSec = (gxUnb * s_gxLP + gyUnb * s_gyLP + gzUnb * s_gzLP);

            // 3) Integrate
            s_yawDeg += yawRateDegPerSec * dt;

            // 4) Leaky Integrator: Apply a weak pull-to-zero to counteract long-term drift.
            // This acts as a final software correction for any residual gyro bias.
            constexpr float YAW_DRIFT_CORRECTION_STRENGTH = 0.002f; // Corrects 0.2% of the drift error per second
            float yaw_error_deg = s_yawDeg - s_yawZeroDeg;
            s_yawDeg -= yaw_error_deg * YAW_DRIFT_CORRECTION_STRENGTH * dt;

            // 5) Keep angle in [-180, 180)
            if (s_yawDeg > 180.0f)
                s_yawDeg -= 360.0f;
            if (s_yawDeg <= -180.0f)
                s_yawDeg += 360.0f;

            // 6) Bias self-trim while still (use original still threshold)
            if (fabsf(gx) + fabsf(gy) + fabsf(gz) < STILL_THR_DPS && aLooksLikeGravity) {
                // Nudge bias toward current gyro (like your existing single-axis trim)
                s_biasX += (gx - s_biasX) * GYRO_DRIFT_TRIM;
                s_biasY += (gy - s_biasY) * GYRO_DRIFT_TRIM;
                s_biasZ += (gz - s_biasZ) * GYRO_DRIFT_TRIM;
            }

            // 6) Present heading (unchanged)
            float rel = s_yawDeg - s_yawZeroDeg;
            while (rel < 0.0f)
                rel += 360.0f;
            while (rel >= 360.0f)
                rel -= 360.0f;
            float heading = 360.0f - rel;
            if (heading >= 360.0f)
                heading -= 360.0f;
#if !defined(MESHTASTIC_EXCLUDE_SCREEN) && HAS_SCREEN
            if (screen && !ScanI2C::hasMagnetometer()) {
                screen->setHeading(heading);
                screen->forceDisplay(true);
            }
#endif
        }
    }
#endif
    return MOTION_SENSOR_CHECK_INTERVAL_MS;
}

void BMI270Sensor::calibrate(uint16_t /*forSeconds*/)
{
    // Instant "face north and calibrate": just anchor on next IMU sample.
    LOG_DEBUG("BMI270: instant calibrate requested (anchor current facing as north)");
    s_anchorRequested = true;

    // Ensure no legacy countdown/alerts remain active
    doCalibration = false;
#if !defined(MESHTASTIC_EXCLUDE_SCREEN) && HAS_SCREEN
    if (screen)
        screen->endAlert();
#endif
}

#endif // !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C && __has_include(<bmi2.h>)

// Fallback implementations when BMI270 is not available
#if !__has_include(<bmi2.h>) || defined(ARCH_STM32WL) || defined(MESHTASTIC_EXCLUDE_I2C)
extern "C" Quat GetAttitudeForRenderer()
{
    return Quat::identity();
}

extern "C" uint32_t GetStepCountForRenderer()
{
    return 0;
}

extern "C" bool HasStepCounterForRenderer()
{
    return false;
}

extern "C" Vec3 GetGravityForRenderer()
{
    return Vec3(0, 0, 1); // Default gravity vector pointing down
}

extern "C" void GetGravityXYZ(float *gx, float *gy, float *gz)
{
    if (gx)
        *gx = 0.0f;
    if (gy)
        *gy = 0.0f;
    if (gz)
        *gz = 1.0f; // Default gravity vector pointing down
}

extern "C" float GetHeadingRadiansForRenderer()
{
    // Prefer real magnetometer data if available from BMM150 or other mag sensors
    if (g_hasMagHeading) {
        return g_magHeadingRad;
    }
    return 0.0f; // Default heading (north)
}
#endif

// ------------------------------ 3D attitude export ------------------------------
// Build a quaternion from the current low-pass gravity vector (tilt) and the integrated
// yaw-about-gravity (relative “north”). This avoids changing your sampling pipeline.
//  - Tilt comes from s_gxLP/s_gyLP/s_gzLP (unit gravity vector in body frame)
//  - Yaw is the same angle you already present to the screen
//
// Conventions:
//  * World up = +Y. We rotate model points by this quaternion before projecting.
//
static Quat quatBetweenUnit(const Vec3 &a, const Vec3 &b)
{
    // Assumes a,b are unit. Handles 180° by choosing an arbitrary ortho axis.
    float d = a.dot(b);
    Vec3 v = a.cross(b);
    float w = 1.0f + d;
    if (w < 1e-6f) {
        // a ≈ -b: pick an orthogonal axis
        Vec3 axis = (std::fabs(a.x) < 0.5f) ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
        v = a.cross(axis).normalized();
        return Quat(0, v.x, v.y, v.z); // 180°
    }
    Quat q(w, v.x, v.y, v.z);
    q.normalize();
    return q;
}

Quat BMI270Sensor::getAttitudeQuat() const
{
#if HAS_BMI270_TINYU
    // 1) Tilt: Define the world's "up" as -Y to match screen coordinates,
    //    then align it with the device's physical UP vector (anti_ghat).
    //    This corrects the pole orientation while keeping the tilt direction intuitive.
    Vec3 ghat(s_gxLP, s_gyLP, s_gzLP); // Gravity vector (points DOWN) in BODY frame
    if (ghat.norm() < 1e-3f)
        ghat = Vec3(0, 1, 0);      // Fallback
    Vec3 anti_ghat = ghat * -1.0f; // Anti-Gravity / Body UP axis

    // Align World DOWN (0,-1,0) with the Body UP vector. This is the key change.
    Quat qTilt = quatBetweenUnit(Vec3(0, -1, 0), anti_ghat);

    // 2) Yaw: Rotate around the device's physical UP axis.
    float rel = s_yawDeg - s_yawZeroDeg;
    while (rel < 0.0f)
        rel += 360.0f;
    while (rel >= 360.0f)
        rel -= 360.0f;
    float yawRad = rel * (float)M_PI / 180.0f;

    // Rotate around the physical UP axis with the corrected yaw direction.
    Quat qYaw = Quat::fromAxisAngle(anti_ghat, -yawRad);

    // Full attitude quaternion: q = qYaw * qTilt
    Quat q = qYaw * qTilt;
    q.normalize();
    return q;
#else
    return Quat::identity();
#endif
}