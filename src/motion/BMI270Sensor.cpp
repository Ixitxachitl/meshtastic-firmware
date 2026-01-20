#include "BMI270Sensor.h"
#include "detect/ScanI2C.h"
#include "graphics/draw/Math3D.h"

// Global variables for magnetometer heading (defined here, used by CompassRenderer)
extern "C" {
volatile bool g_hasMagHeading = false;
volatile float g_magHeadingRad = 0.0f; // radians, 0 = North, +CW
}

// BMM150 magnetometer support for tilt-compensated compass
#if !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C && __has_include(<DFRobot_BMM150.h>)
#define HAS_BMM150_MAG 1
#include "BMM150Sensor.h"
#else
#define HAS_BMM150_MAG 0
#endif

#if HAS_BMM150_MAG
// Magnetometer calibration state
namespace
{
struct MagCal {
    float offset[3] = {0, 0, 0};
    float scale[3] = {1, 1, 1};
    bool valid = false;
};
MagCal s_magCal;
bool s_magCalActive = false;
uint32_t s_magCalEndMs = 0;
uint32_t s_magCalSamples = 0;
float s_magMin[3] = {1e9f, 1e9f, 1e9f};
float s_magMax[3] = {-1e9f, -1e9f, -1e9f};

void magCalReset()
{
    s_magCalActive = false;
    s_magCalEndMs = 0;
    s_magCalSamples = 0;
    s_magMin[0] = s_magMin[1] = s_magMin[2] = 1e9f;
    s_magMax[0] = s_magMax[1] = s_magMax[2] = -1e9f;
}

void magCalPush(float mx, float my, float mz)
{
    if (mx < s_magMin[0])
        s_magMin[0] = mx;
    if (mx > s_magMax[0])
        s_magMax[0] = mx;
    if (my < s_magMin[1])
        s_magMin[1] = my;
    if (my > s_magMax[1])
        s_magMax[1] = my;
    if (mz < s_magMin[2])
        s_magMin[2] = mz;
    if (mz > s_magMax[2])
        s_magMax[2] = mz;
    ++s_magCalSamples;
}

void magCalSolve()
{
    for (int i = 0; i < 3; ++i)
        s_magCal.offset[i] = 0.5f * (s_magMax[i] + s_magMin[i]);
    float r[3] = {0.5f * (s_magMax[0] - s_magMin[0]), 0.5f * (s_magMax[1] - s_magMin[1]), 0.5f * (s_magMax[2] - s_magMin[2])};
    float rmean = (r[0] + r[1] + r[2]) / 3.0f;
    for (int i = 0; i < 3; ++i)
        s_magCal.scale[i] = (r[i] > 1e-3f) ? (rmean / r[i]) : 1.0f;
    s_magCal.valid = true;
    s_magCalActive = false;
}
} // namespace
#endif // HAS_BMM150_MAG

#if !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C && __has_include(<bmi2.h>)

// Use tinyu-zhao BMI270 library exclusively
#include "buzz/buzz.h"
#include "graphics/Screen.h"
#include <Arduino.h>
#include <Wire.h>
#include <bmi2.h>
#include <bmi270.h>
#include <bmi2_defs.h>
#include <math.h>

// Screen instance from main.cpp
#if !defined(MESHTASTIC_EXCLUDE_SCREEN)
extern graphics::Screen *screen;
#endif

#define HAS_BMI270_TINYU 1

// Motion detection using BMI270 library API only

#if HAS_BMI270_TINYU

// ------------------------- Private impl (I2C bridge) -------------------------
struct BMI270Impl {
    bmi2_dev dev{};
    uint8_t addr = 0x00;   // 0x69 or 0x68
    TwoWire *wire = &Wire; // default bus
};

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

// Probe 0x69 then 0x68 using library's read function
static bool selectAddress(BMI270Impl &impl)
{
    for (uint8_t addr : {(uint8_t)0x69, (uint8_t)0x68}) {
        impl.addr = addr;
        uint8_t chip_id = 0;
        if (bmi2_get_regs(BMI2_CHIP_ID_ADDR, &chip_id, 1, &impl.dev) == BMI2_OK) {
            LOG_DEBUG("BMI270: addr 0x%02X chip_id 0x%02X", addr, chip_id);
            if (chip_id == BMI270_CHIP_ID) {
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
bool s_anchorRequested = false; // instant calibration flag
} // namespace

#endif // HAS_BMI270_TINYU

// -----------------------------------------------------------------------------

BMI270Sensor::BMI270Sensor(ScanI2C::FoundDevice foundDevice) : MotionSensor::MotionSensor(foundDevice) {}

// ---- 3D compass handoff for CompassRenderer (no UI plumbing needed) ----
static BMI270Sensor *g_bmi270_instance = nullptr;

BMI270Sensor::~BMI270Sensor()
{
#if HAS_BMI270_TINYU
    if (impl_) {
        auto *impl = static_cast<BMI270Impl *>(impl_);
        delete impl;
        impl_ = nullptr;
    }
    if (g_bmi270_instance == this) {
        g_bmi270_instance = nullptr;
    }
#endif
}
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

uint32_t BMI270Sensor::getStepCount() const
{
#if HAS_BMI270_TINYU
    auto *impl = static_cast<BMI270Impl *>(impl_);
    if (!impl)
        return 0;

    bmi2_feat_sensor_data sensor_data = {.type = BMI2_STEP_COUNTER};
    return (bmi270_get_feature_data(&sensor_data, 1, &impl->dev) == BMI2_OK) ? sensor_data.sens_data.step_counter_output : 0;
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

    // Initialize BMI2 device structure
    impl->dev.intf = BMI2_I2C_INTF;
    impl->dev.read = boschI2CRead;
    impl->dev.write = boschI2CWrite;
    impl->dev.delay_us = boschDelayUs;
    impl->dev.intf_ptr = impl;
    impl->dev.read_write_len = 16;
    impl->dev.config_file_ptr = NULL;

    // Soft reset and device detection
    bmi2_soft_reset(&impl->dev);
    impl->dev.delay_us(2000, impl->dev.intf_ptr); // 2ms soft reset delay

    if (!selectAddress(*impl)) {
        LOG_DEBUG("BMI270: Device not found (chip_id 0x%02X not at 0x69/0x68)", BMI270_CHIP_ID);
        delete impl;
        return false;
    }

    // Initialize BMI270 (uploads config file internally)
    int8_t err = bmi270_init(&impl->dev);
    if (err != BMI2_OK) {
        LOG_DEBUG("BMI270: Init failed (%d) at 0x%02X", err, impl->addr);
        delete impl;
        return false;
    }

    // Enable accel + gyro + motion features
    uint8_t sensors[] = {BMI2_ACCEL, BMI2_GYRO, BMI2_ANY_MOTION, BMI2_STEP_COUNTER};
    if (bmi270_sensor_enable(sensors, 4, &impl->dev) != BMI2_OK) {
        LOG_DEBUG("BMI270: sensor_enable failed");
        delete impl;
        return false;
    }

    // Configure all sensors in one call
    bmi2_sens_config cfgs[3] = {};
    cfgs[0].type = BMI2_ACCEL;
    cfgs[0].cfg.acc.odr = BMI2_ACC_ODR_100HZ;
    cfgs[0].cfg.acc.range = BMI2_ACC_RANGE_4G;
    cfgs[0].cfg.acc.bwp = BMI2_ACC_NORMAL_AVG4;

    cfgs[1].type = BMI2_GYRO;
    cfgs[1].cfg.gyr.odr = BMI2_GYR_ODR_100HZ;
    cfgs[1].cfg.gyr.range = BMI2_GYR_RANGE_2000;
    cfgs[1].cfg.gyr.bwp = BMI2_GYR_NORMAL_MODE;

    cfgs[2].type = BMI2_ANY_MOTION;
    cfgs[2].cfg.any_motion.threshold = 200;
    cfgs[2].cfg.any_motion.duration = 4;
    cfgs[2].cfg.any_motion.select_x = 1;
    cfgs[2].cfg.any_motion.select_y = 1;
    cfgs[2].cfg.any_motion.select_z = 1;

    if (bmi270_set_sensor_config(cfgs, 3, &impl->dev) != BMI2_OK) {
        LOG_DEBUG("BMI270: config failed");
        delete impl;
        return false;
    }

    // Map motion features to INT1
    bmi2_map_feat_int(BMI2_ANY_MOTION, BMI2_INT1, &impl->dev);
    bmi2_map_feat_int(BMI2_STEP_COUNTER, BMI2_INT1, &impl->dev);

    impl->dev.delay_us(50000, impl->dev.intf_ptr); // 50ms settle

    // Initialize compass state
    s_yawDeg = s_yawZeroDeg = 0.0f;
    s_lastMicros = 0;
    s_anchorRequested = false;

    // Initialize gravity vector with first reading
    bmi2_sens_data initial_data{};
    if (bmi2_get_sensor_data(&initial_data, &impl->dev) == BMI2_OK) {
        const float aScale = 4.0f / 32768.0f; // ±4g
        float ax = -initial_data.acc.x * aScale;
        float ay = initial_data.acc.y * aScale;
        float az = initial_data.acc.z * aScale;
        float aMag = sqrtf(ax * ax + ay * ay + az * az);
        if (aMag > 0.1f) {
            s_gxLP = ax / aMag;
            s_gyLP = ay / aMag;
            s_gzLP = az / aMag;
        }
    }

    impl_ = impl;
    inited_ = true;
    g_bmi270_instance = this;

    LOG_DEBUG("BMI270: init ok at 0x%02X", impl->addr);
    return true;
#endif
}

int32_t BMI270Sensor::runOnce()
{
#if HAS_BMI270_TINYU
    if (inited_ && impl_) {

        auto *impl = static_cast<BMI270Impl *>(impl_);

        // Check motion and step interrupts using library
        uint16_t int_status = 0;
        if (bmi2_get_int_status(&int_status, &impl->dev) == BMI2_OK &&
            (int_status & (BMI270_ANY_MOT_STATUS_MASK | BMI270_STEP_CNT_STATUS_MASK))) {
            wakeScreen();
            powerFSM.trigger(EVENT_PRESS);
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
            // Skip fake compass integration entirely if real magnetometer is available

            if (!g_hasMagHeading) {
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

                    // 4) Clear request - calibration is instant, no UI needed
                    s_anchorRequested = false;
#if !defined(MESHTASTIC_EXCLUDE_SCREEN) && HAS_SCREEN
                    if (screen) {
                        screen->setHeading(0.0f); // force the snap visually this frame
                        screen->forceDisplay(true);
                    }
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

                // 7) Present heading
                float rel = s_yawDeg - s_yawZeroDeg;
                while (rel < 0.0f)
                    rel += 360.0f;
                while (rel >= 360.0f)
                    rel -= 360.0f;
                float heading = 360.0f - rel;
                if (heading >= 360.0f)
                    heading -= 360.0f;
#if !defined(MESHTASTIC_EXCLUDE_SCREEN) && HAS_SCREEN
                if (screen) {
                    screen->setHeading(heading);
                    screen->forceDisplay(true);
                }
#endif
            } // End fake compass integration (only when no real magnetometer)

            // Always update gravity vector (needed for tilt compensation)
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

#if HAS_BMM150_MAG
            // Get BMM150 singleton (already initialized by BMM150Sensor)
            static BMM150Singleton *bmm150 = nullptr;
            static uint32_t bmm150_retry_time = 0;

            // Retry getting the existing singleton periodically
            if (!bmm150 && millis() > bmm150_retry_time) {
                bmm150_retry_time = millis() + 500; // Retry every 500ms
                bmm150 = BMM150Singleton::GetExistingInstance();
                if (bmm150) {
                    LOG_INFO("BMI270: Got existing BMM150 singleton");
                }
            }

            if (bmm150) {
                // Read raw magnetometer data
                sBmm150MagData_t mag = bmm150->getGeomagneticData();

                float mx = mag.x;
                float my = mag.y;
                float mz = mag.z;

                // Debug: show raw mag readings periodically
                static uint32_t lastRawDebugMs = 0;
                if (millis() - lastRawDebugMs > 2000) {
                    LOG_DEBUG("BMM150 raw: [%d, %d, %d]", mag.x, mag.y, mag.z);
                    lastRawDebugMs = millis();
                }

                // Skip if data looks invalid (all zeros or very small)
                float magMag = mx * mx + my * my + mz * mz;
                if (magMag < 1.0f) {
                    // Data invalid, skip this sample
                    if (s_magCalActive) {
                        LOG_DEBUG("BMM150: Invalid data mx=%.1f my=%.1f mz=%.1f", mx, my, mz);
                    }
                } else {
                    // Handle calibration sampling
                    if (s_magCalActive) {
                        magCalPush(mx, my, mz);

                        // Update countdown banner every second
                        static uint32_t lastBannerUpdateMs = 0;
                        uint32_t remainingSec = (s_magCalEndMs > millis()) ? (s_magCalEndMs - millis()) / 1000 : 0;
                        if (millis() - lastBannerUpdateMs > 1000) {
#if !defined(MESHTASTIC_EXCLUDE_SCREEN) && HAS_SCREEN
                            if (screen && remainingSec > 0) {
                                char bannerMsg[64];
                                snprintf(bannerMsg, sizeof(bannerMsg),
                                         "Calibrating compass...\nRotate device in all\ndirections for %us", remainingSec);
                                screen->showSimpleBanner(bannerMsg, remainingSec * 1000);
                            }
#endif
                            lastBannerUpdateMs = millis();
                        }

                        if ((int32_t)(millis() - s_magCalEndMs) >= 0) {
                            // Calibration time ended - check if we got good data
                            float spanX = s_magMax[0] - s_magMin[0];
                            float spanY = s_magMax[1] - s_magMin[1];
                            float spanZ = s_magMax[2] - s_magMin[2];
                            if (s_magCalSamples >= 50 && spanX > 20 && spanY > 20 && spanZ > 20) {
                                magCalSolve();
                                LOG_INFO("BMM150 calibration complete: offset=[%.1f,%.1f,%.1f] scale=[%.2f,%.2f,%.2f]",
                                         s_magCal.offset[0], s_magCal.offset[1], s_magCal.offset[2], s_magCal.scale[0],
                                         s_magCal.scale[1], s_magCal.scale[2]);
#if !defined(MESHTASTIC_EXCLUDE_SCREEN) && HAS_SCREEN
                                if (screen) {
                                    screen->showSimpleBanner("Compass calibration\nSUCCESS!", 3000);
                                }
#endif
                                playBeep(); // Confirmation beep
                            } else {
                                LOG_WARN("BMM150 calibration failed: samples=%u spans=[%.1f,%.1f,%.1f]", s_magCalSamples, spanX,
                                         spanY, spanZ);
#if !defined(MESHTASTIC_EXCLUDE_SCREEN) && HAS_SCREEN
                                if (screen) {
                                    screen->showSimpleBanner("Compass calibration\nFAILED\nTry again", 3000);
                                }
#endif
                                s_magCalActive = false;
                            }
                        }
                    }

                    // Only compute heading if calibration is valid
                    if (s_magCal.valid) {
                        // Apply calibration
                        float mxc = (mx - s_magCal.offset[0]) * s_magCal.scale[0];
                        float myc = (my - s_magCal.offset[1]) * s_magCal.scale[1];
                        float mzc = (mz - s_magCal.offset[2]) * s_magCal.scale[2];

                        // Tilt-compensated compass using standard formula
                        // Device coordinate system (observed from data):
                        //   - X points backward, Y points left, Z points down (when flat)
                        //   - Forward = -X direction
                        //   - Gravity when flat: [0, 0, 1]
                        //   - Gravity when tilted back (screen facing user): [0, 1, 0]
                        //
                        // Standard tilt compensation formula:
                        //   pitch = atan2(gy, gz)  -- forward/back tilt
                        //   roll = atan2(-gx, sqrt(gy^2 + gz^2))  -- left/right tilt
                        //   Xh = -mx*cos(pitch) + my*sin(roll)*sin(pitch) + mz*cos(roll)*sin(pitch)
                        //   Yh = my*cos(roll) - mz*sin(roll)

                        // Pitch: forward/back tilt
                        float pitch = atan2f(s_gyLP, s_gzLP);
                        float cp = cosf(pitch);
                        float sp = sinf(pitch);

                        // Roll: left/right tilt
                        float roll = atan2f(-s_gxLP, sqrtf(s_gyLP * s_gyLP + s_gzLP * s_gzLP));
                        float cr = cosf(roll);
                        float sr = sinf(roll);

                        // Standard tilt compensation
                        float Xh = -mxc * cp + myc * sr * sp + mzc * cr * sp;
                        float Yh = myc * cr - mzc * sr;

                        // Heading from horizontal components (0=North, clockwise positive)
                        float headingDeg = atan2f(Yh, Xh) * 180.0f / (float)M_PI;
                        if (headingDeg < 0)
                            headingDeg += 360.0f;

                        // Debug: log computed values periodically
                        static uint32_t lastHeadingDebugMs = 0;
                        if (millis() - lastHeadingDebugMs > 2000) {
                            LOG_DEBUG("Tilt-comp: heading=%.1f° Xh=%.1f Yh=%.1f | pitch=%.0f° roll=%.0f° | grav=[%.2f,%.2f,%.2f]",
                                      headingDeg, Xh, Yh, pitch * 180.0f / M_PI, roll * 180.0f / M_PI, s_gxLP, s_gyLP, s_gzLP);
                            lastHeadingDebugMs = millis();
                        }

                        // Set global heading for CompassRenderer
                        g_magHeadingRad = headingDeg * (float)M_PI / 180.0f;
                        g_hasMagHeading = true;

                        // Update screen with calibrated magnetometer heading
#if !defined(MESHTASTIC_EXCLUDE_SCREEN) && HAS_SCREEN
                        if (screen) {
                            screen->setHeading(headingDeg);
                            screen->forceDisplay(true);
                        }
#endif
                    }
                } // end if magMag valid
            }
#endif // HAS_BMM150_MAG
        }
    }
#endif
    // Update rate: 50ms for fake gyro compass (smoother), 100ms for magnetometer or idle
    // Fake compass needs faster updates for smooth rotation tracking
    return g_hasMagHeading ? 100 : (s_magCalActive ? 100 : 50);
}

void BMI270Sensor::calibrate(uint16_t forSeconds)
{
#if HAS_BMM150_MAG
    // Start magnetometer calibration with countdown
    uint16_t calTime = (forSeconds > 0) ? forSeconds : 10;

    // Get BMM150 singleton (should be created by BMM150Sensor thread)
    BMM150Singleton *bmm150 = BMM150Singleton::GetExistingInstance();
    if (!bmm150) {
        LOG_WARN("BMM150 singleton not available for calibration");
    }

    if (bmm150) {
        bmm150->setOperationMode(BMM150_POWERMODE_NORMAL);
        bmm150->setPresetMode(BMM150_PRESETMODE_HIGHACCURACY);
        bmm150->setRate(BMM150_DATA_RATE_30HZ); // Fast rate for calibration
        bmm150->setMeasurementXYZ();            // REQUIRED: re-enable measurements after config change
        LOG_DEBUG("BMM150 configured for calibration: 30Hz, high accuracy");

        s_magCalActive = true;
        s_magCalEndMs = millis() + (uint32_t)calTime * 1000u;
        s_magCalSamples = 0;
        s_magMin[0] = s_magMin[1] = s_magMin[2] = 1e9f;
        s_magMax[0] = s_magMax[1] = s_magMax[2] = -1e9f;
        g_hasMagHeading = false; // Use fake compass during calibration

        // Show calibration banner
#if !defined(MESHTASTIC_EXCLUDE_SCREEN) && HAS_SCREEN
        if (screen) {
            char bannerMsg[64];
            snprintf(bannerMsg, sizeof(bannerMsg), "Calibrating compass...\nRotate device in all\ndirections for %us", calTime);
            screen->showSimpleBanner(bannerMsg, calTime * 1000);
        }
#endif
        playBeep(); // Start beep
        LOG_INFO("BMM150 calibration started for %u seconds - rotate device in all directions", calTime);
    } else {
        // No magnetometer available - fall back to fake compass calibration
        LOG_DEBUG("BMI270: No BMM150 available, using instant calibrate (anchor current facing as north)");
        s_anchorRequested = true;
    }
#else
    // No magnetometer - just anchor fake compass
    LOG_DEBUG("BMI270: instant calibrate requested (anchor current facing as north)");
    s_anchorRequested = true;
#endif
}

#endif // !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C && __has_include(<bmi2.h>)

// Fallback implementations when BMI270 is not available
#if !__has_include(<bmi2.h>) || defined(ARCH_STM32WL) || defined(MESHTASTIC_EXCLUDE_I2C)

#ifdef HAS_BHI260AP
#include "BHI260APSensor.h"
#endif

extern "C" Quat GetAttitudeForRenderer()
{
    return Quat::identity();
}

extern "C" uint32_t GetStepCountForRenderer()
{
#ifdef HAS_BHI260AP
    if (g_bhi260ap_instance) {
        return g_bhi260ap_instance->getStepCount();
    }
#endif
    return 0;
}

extern "C" bool HasStepCounterForRenderer()
{
#ifdef HAS_BHI260AP
    return (g_bhi260ap_instance != nullptr);
#else
    return false;
#endif
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