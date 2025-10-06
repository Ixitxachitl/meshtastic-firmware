#include "BMI270Sensor.h"

#if !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C

// Only the .cpp depends on Bosch headers.
#if __has_include(<bmi2.h>) && __has_include(<bmi270.h>)
  #include <Arduino.h>
  #include <Wire.h>
  #include <math.h>
  #include <bmi2.h>
  #include <bmi270.h>
  #define HAS_BOSCH_BMI270 1
#else
  #define HAS_BOSCH_BMI270 0
#endif

#if HAS_BOSCH_BMI270

// ------------------------- Private impl (I2C bridge) -------------------------
struct BMI270Impl {
  bmi2_dev dev{};
  uint8_t  addr = 0x00;      // 0x69 or 0x68
  TwoWire* wire = &Wire;     // default bus
};

// I2C helpers
static bool i2cRead1(TwoWire& w, uint8_t addr, uint8_t reg, uint8_t& val) {
  w.beginTransmission(addr);
  w.write(reg);
  if (w.endTransmission(false) != 0) return false;
  if (w.requestFrom(addr, (uint8_t)1) != 1) return false;
  val = w.read();
  return true;
}

// Bosch callbacks
static BMI2_INTF_RETURN_TYPE boschI2CRead(uint8_t reg, uint8_t* buf, uint32_t len, void* intf_ptr) {
  auto* impl = static_cast<BMI270Impl*>(intf_ptr);
  TwoWire& w = *(impl->wire);
  w.beginTransmission(impl->addr);
  w.write(reg);
  if (w.endTransmission(false) != 0) return BMI2_E_COM_FAIL;
  uint32_t n = w.requestFrom(impl->addr, (uint8_t)len);
  if (n != len) return BMI2_E_COM_FAIL;
  for (uint32_t i = 0; i < len; ++i) buf[i] = w.read();
  return BMI2_OK;
}
static BMI2_INTF_RETURN_TYPE boschI2CWrite(uint8_t reg, const uint8_t* buf, uint32_t len, void* intf_ptr) {
  auto* impl = static_cast<BMI270Impl*>(intf_ptr);
  TwoWire& w = *(impl->wire);
  w.beginTransmission(impl->addr);
  w.write(reg);
  for (uint32_t i = 0; i < len; ++i) w.write(buf[i]);
  if (w.endTransmission() != 0) return BMI2_E_COM_FAIL;
  return BMI2_OK;
}
static void boschDelayUs(uint32_t us, void*) { delayMicroseconds(us); }

// Probe 0x69 then 0x68 (WHOAMI 0x24)
static bool selectAddress(BMI270Impl& impl) {
  uint8_t who = 0;
  for (uint8_t a : { (uint8_t)0x69, (uint8_t)0x68 }) {
    if (i2cRead1(*(impl.wire), a, 0x00, who)) {   // <-- impl.wire (not impl->wire)
      LOG_DEBUG("BMIx probe: addr 0x%02X WHOAMI 0x%02X", a, who);
      if (who == 0x24) { impl.addr = a; return true; }
    }
  }
  return false;
}

// ------------------- Fake compass (gyro-only) – file-scope state -------------------
namespace {
  // Tuning for software wake/tap
  constexpr float    MOTION_DELTA_G       = 0.15f;
  constexpr uint32_t MOTION_DEBOUNCE_MS   = 200;
  constexpr float    TAP_PEAK_G           = 1.25f;
  constexpr uint32_t TAP_WINDOW_MS        = 120;
  constexpr uint32_t DOUBLE_TAP_GAP_MS    = 300;
  constexpr float G_LPF_ALPHA     = 0.02f;   // accel LPF for gravity dir
  constexpr float G_VALID_TOL_G   = 0.20f;   // accept accel norm ~ 1g ± tol
  
  // Gravity (LPF) and gyro bias as full vectors
  float s_gxLP = 0.0f, s_gyLP = 0.0f, s_gzLP = 1.0f;  // init world-up ~ +Z
  float s_biasX = 0.0f, s_biasY = 0.0f, s_biasZ = 0.0f;

  // Tuning for fake compass drift control
  constexpr float GYRO_DRIFT_TRIM = 0.0005f;   // small nudge while still
  constexpr float STILL_THR_DPS   = 2.0f;      // consider still if |g| sum < this

  // Persistent state across runOnce()
  float     s_lastAx = 0, s_lastAy = 0, s_lastAz = 0;
  uint32_t  s_lastMotionTs = 0;

  bool      s_tapArmed = false;
  uint32_t  s_tapArmTs = 0;
  uint32_t  s_lastTapTs = 0;

  // Fake compass state
  float     s_yawDeg = 0.0f;        // integrated yaw
  float     s_yawZeroDeg = 0.0f;    // user “north” anchor
  float     s_gyroBiasZ = 0.0f;     // bias from calibration
  uint32_t  s_lastMicros = 0;

  // Bias calibration accumulators
  bool      s_doingBiasCal = false;
  uint32_t  s_biasCalEndMs = 0;
  double    s_biasSumZ = 0.0;
  uint32_t  s_biasCount = 0;

  // Calibration UI guard: only open/close the alert once per session
  bool      s_showingCalUI = false;

  // Request to anchor "north" on next IMU sample (no countdown UI)
  static bool s_anchorRequested = false;
} // namespace

#endif // HAS_BOSCH_BMI270


// -----------------------------------------------------------------------------

BMI270Sensor::BMI270Sensor(ScanI2C::FoundDevice foundDevice)
  : MotionSensor::MotionSensor(foundDevice) {}

bool BMI270Sensor::init()
{
#if !HAS_BOSCH_BMI270
  LOG_DEBUG("BMI270: Bosch BMI2 headers not available in this build");
  return false;
#else
  auto* impl = new BMI270Impl();
  if (!impl) return false;

  // Use same bus as the rest of your firmware (logs show Wire). If you need Wire1, set impl->wire = &Wire1.
  impl->wire = &Wire;

  // Soft reset both addrs, then select
  uint8_t SOFT_RESET = 0xB6;
  (void)boschI2CWrite(0x7E, &SOFT_RESET, 1, impl); delay(10);
  (void)boschI2CWrite(0x7E, &SOFT_RESET, 1, impl); delay(10);

  if (!selectAddress(*impl)) {
    LOG_DEBUG("BMI270: WHOAMI 0x24 not found at 0x69/0x68");
    delete impl;
    return false;
  }

  // Hook bosch device
  impl->dev.intf = BMI2_I2C_INTF;
  impl->dev.read = &boschI2CRead;
  impl->dev.write = &boschI2CWrite;
  impl->dev.delay_us = &boschDelayUs;
  impl->dev.intf_ptr = impl;
  impl->dev.read_write_len = 16;
  impl->dev.config_file_ptr = NULL;

  (void)bmi2_soft_reset(&impl->dev);
  delay(10);

  int8_t err = bmi270_init(&impl->dev);
  if (err != BMI2_OK) {
    (void)bmi2_soft_reset(&impl->dev);
    delay(10);
    err = bmi270_init(&impl->dev);
  }
  if (err != BMI2_OK) {
    LOG_DEBUG("BMI270: bmi270_init failed (%d) addr 0x%02X", err, impl->addr);
    delete impl;
    return false;
  }

  // Enable accel + gyro
  uint8_t sensors[] = { BMI2_ACCEL, BMI2_GYRO };
  err = bmi270_sensor_enable(sensors, 2, &impl->dev);
  if (err != BMI2_OK) {
    LOG_DEBUG("BMI270: sensor_enable failed (%d)", err);
    delete impl;
    return false;
  }

  // Configure ODR / range / BW
  bmi2_sens_config cfgs[2];
  cfgs[0].type = BMI2_ACCEL;
  cfgs[1].type = BMI2_GYRO;
  if (bmi270_get_sensor_config(cfgs, 2, &impl->dev) != BMI2_OK) { delete impl; return false; }

  cfgs[0].cfg.acc.odr   = BMI2_ACC_ODR_100HZ;
  cfgs[0].cfg.acc.bwp   = BMI2_ACC_NORMAL_AVG4;
  cfgs[0].cfg.acc.range = BMI2_ACC_RANGE_4G;

  cfgs[1].cfg.gyr.odr   = BMI2_GYR_ODR_100HZ;
  cfgs[1].cfg.gyr.bwp   = BMI2_GYR_NORMAL_MODE;
  cfgs[1].cfg.gyr.range = BMI2_GYR_RANGE_2000;

  if (bmi270_set_sensor_config(cfgs, 2, &impl->dev) != BMI2_OK) { delete impl; return false; }

  // Reset fake compass state
  s_lastAx = s_lastAy = s_lastAz = 0;
  s_lastMotionTs = 0;
  s_tapArmed = false; s_tapArmTs = 0; s_lastTapTs = 0;

  s_yawDeg = 0.0f; s_yawZeroDeg = 0.0f; s_gyroBiasZ = 0.0f; s_lastMicros = 0;

  s_doingBiasCal = false; s_biasCalEndMs = 0; s_biasSumZ = 0.0; s_biasCount = 0;

  s_showingCalUI = false;

  impl_    = impl;
  inited_  = true;
  LOG_DEBUG("BMI270: init ok at 0x%02X", impl->addr);
  return true;
#endif
}

int32_t BMI270Sensor::runOnce()
{
#if HAS_BOSCH_BMI270
  if (inited_ && impl_) {

    // --- Calibration UI handling (same pattern as BMX160) ---
#if !defined(MESHTASTIC_EXCLUDE_SCREEN) && HAS_SCREEN
    if (doCalibration) {
      if (!s_showingCalUI) {
        powerFSM.trigger(EVENT_PRESS);                 // keep screen awake during calibration
        s_showingCalUI = true;
        if (screen) screen->startAlert((FrameCallback)drawFrameCalibration);
      }
    } else {
      if (s_showingCalUI) {
        if (screen) screen->endAlert();
        s_showingCalUI = false;
      }
    }
#endif

    auto* impl = static_cast<BMI270Impl*>(impl_);
    bmi2_sens_data raw{};
    if (bmi2_get_sensor_data(&raw, &impl->dev) == BMI2_OK) {

      // Convert according to ranges set in init()
      const float aScale = 4.0f    / 32768.0f;   // ±4 g
      const float gScale = 2000.0f / 32768.0f;   // 2000 dps

      const float ax = raw.acc.x * aScale;
      const float ay = raw.acc.y * aScale;
      const float az = raw.acc.z * aScale;

      const float gx = raw.gyr.x * gScale;
      const float gy = raw.gyr.y * gScale;
      const float gz = raw.gyr.z * gScale;

      // ------------------ Software wake-on-motion/tap (single setting) ------------------
      if (config.display.wake_on_tap_or_motion) {

        // Motion via accel deltas
        float dax = fabsf(ax - s_lastAx);
        float day = fabsf(ay - s_lastAy);
        float daz = fabsf(az - s_lastAz);
        if ((dax + day + daz) > MOTION_DELTA_G) {
          const uint32_t now = millis();
          if (now - s_lastMotionTs > MOTION_DEBOUNCE_MS) {
            s_lastMotionTs = now;
            wakeScreen(); // Meshtastic: wake
            powerFSM.trigger(EVENT_PRESS);
          }
        }
        s_lastAx = ax; s_lastAy = ay; s_lastAz = az;

        // Tap / Double-tap via accel magnitude impulse
        const float aMag    = sqrtf(ax*ax + ay*ay + az*az); // g
        const float aExcess = aMag - 1.0f;
        const uint32_t now  = millis();

        if (!s_tapArmed) {
          if (aExcess > 0.10f) { s_tapArmed = true; s_tapArmTs = now; }
        } else {
          if (now - s_tapArmTs <= TAP_WINDOW_MS) {
            if (aExcess > TAP_PEAK_G) {
              if ((now - s_lastTapTs) <= DOUBLE_TAP_GAP_MS) {
                buttonPress();    // treat double-tap like a button press
                s_lastTapTs = 0;
              } else {
                s_lastTapTs = now;
                wakeScreen();     // single tap wakes
                powerFSM.trigger(EVENT_PRESS);
              }
              s_tapArmed = false;
            }
          } else {
            s_tapArmed = false;
          }
        }
      } else {
        // Setting disabled: keep last accel for future deltas but don’t wake
        s_lastAx = ax; s_lastAy = ay; s_lastAz = az;
        s_tapArmed = false;
      }

      // ------------------------ Fake compass (gyro-only yaw) ------------------------
      // ---- Instant anchor: lock current facing as "north" (no countdown) ----
      if (s_anchorRequested) {
        // Snap current Z bias (reduces immediate drift feeling)
        s_gyroBiasZ = gz;          // quick bias estimate from the current sample
        s_yawZeroDeg = s_yawDeg;   // anchor "north" to current yaw
        s_anchorRequested = false;

      #if !defined(MESHTASTIC_EXCLUDE_SCREEN) && HAS_SCREEN
        // Make sure no alert is left open (in case of older behavior)
        if (screen) screen->endAlert();
      #endif

        LOG_DEBUG("BMI270: anchored north (biasZ=%.3f dps, zero=%.1f deg)", s_gyroBiasZ, s_yawZeroDeg);
      }

      // --- Tilt-compensated yaw integration ---
      uint32_t nowUs = micros();
      if (s_lastMicros == 0) s_lastMicros = nowUs;
      float dt = (nowUs - s_lastMicros) * 1e-6f;
      s_lastMicros = nowUs;

      // 1) Update gravity estimate when accel looks like gravity (not linear motion)
      float aMag = sqrtf(ax*ax + ay*ay + az*az); // g
      bool aLooksLikeGravity = fabsf(aMag - 1.0f) < G_VALID_TOL_G;
      if (aLooksLikeGravity) {
        // simple first-order LPF
        s_gxLP = s_gxLP + G_LPF_ALPHA * (ax - s_gxLP);
        s_gyLP = s_gyLP + G_LPF_ALPHA * (ay - s_gyLP);
        s_gzLP = s_gzLP + G_LPF_ALPHA * (az - s_gzLP);
        // normalize to unit
        float gn = sqrtf(s_gxLP*s_gxLP + s_gyLP*s_gyLP + s_gzLP*s_gzLP);
        if (gn > 1e-3f) { s_gxLP/=gn; s_gyLP/=gn; s_gzLP/=gn; }
      }

      // 2) Project gyro onto gravity to get world-yaw rate
      float gxUnb = gx - s_biasX;
      float gyUnb = gy - s_biasY;
      float gzUnb = gz - s_biasZ;
      float yawRateDegPerSec = (gxUnb * s_gxLP + gyUnb * s_gyLP + gzUnb * s_gzLP);

      // 3) Integrate
      s_yawDeg += yawRateDegPerSec * dt;

      // 4) Keep angle in [-180, 180)
      if (s_yawDeg > 180.0f)  s_yawDeg -= 360.0f;
      if (s_yawDeg <= -180.0f) s_yawDeg += 360.0f;

      // 5) Bias self-trim while still (use original still threshold)
      if (fabsf(gx) + fabsf(gy) + fabsf(gz) < STILL_THR_DPS && aLooksLikeGravity) {
        // Nudge bias toward current gyro (like your existing single-axis trim)
        s_biasX += (gx - s_biasX) * GYRO_DRIFT_TRIM;
        s_biasY += (gy - s_biasY) * GYRO_DRIFT_TRIM;
        s_biasZ += (gz - s_biasZ) * GYRO_DRIFT_TRIM;
      }

      // 6) Present heading (unchanged)
      float rel = s_yawDeg - s_yawZeroDeg;
      while (rel < 0.0f)   rel += 360.0f;
      while (rel >= 360.0f) rel -= 360.0f;
      float heading = 360.0f - rel;
      if (heading >= 360.0f) heading -= 360.0f;
#if !defined(MESHTASTIC_EXCLUDE_SCREEN) && HAS_SCREEN
      if (screen) { screen->setHeading(heading); screen->forceDisplay(true); }
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
  if (screen) screen->endAlert();
#endif
}

#endif // build guard
