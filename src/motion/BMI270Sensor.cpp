#include "BMI270Sensor.h"

#if !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C

// Only the .cpp depends on Bosch headers.
#if __has_include(<bmi2.h>) && __has_include(<bmi270.h>)
  #include <Arduino.h>
  #include <Wire.h>
  #include <bmi2.h>
  #include <bmi270.h>
  #define HAS_BOSCH_BMI270 1
#else
  #define HAS_BOSCH_BMI270 0
#endif

#if HAS_BOSCH_BMI270

// Small private impl that keeps Bosch state
struct BMI270Impl {
  bmi2_dev dev{};
  uint8_t addr = 0x00;   // chosen address (0x69 or 0x68)
  TwoWire* wire = &Wire; // default bus
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

// Bosch driver callbacks (bridge to Arduino Wire)
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

// Try to detect BMI270 at 0x69 then 0x68 (WHOAMI 0x24)
static bool selectAddress(BMI270Impl& impl) {
  uint8_t who = 0;
  for (uint8_t a : { (uint8_t)0x69, (uint8_t)0x68 }) {
    if (i2cRead1(*(impl.wire), a, 0x00, who)) {
      LOG_DEBUG("BMIx probe: addr 0x%02X WHOAMI 0x%02X", a, who);
      if (who == 0x24) { impl.addr = a; return true; }
    }
  }
  return false;
}

#endif // HAS_BOSCH_BMI270

BMI270Sensor::BMI270Sensor(ScanI2C::FoundDevice foundDevice)
  : MotionSensor::MotionSensor(foundDevice) {}

bool BMI270Sensor::init()
{
#if !HAS_BOSCH_BMI270
  LOG_DEBUG("BMI270: Bosch BMI2 headers not available in this build");
  return false;
#else
  // Use the same bus your firmware already uses (your logs showed "port: Wire").
  // Do not call Wire.begin() again if your platform dislikes re-begin.
  // If you actually need Wire1 on some target, change impl->wire here.
  auto* impl = new BMI270Impl();
  if (!impl) return false;
  impl->wire = &Wire;

  // Optional: slow I2C during bring-up on picky boards
  // Wire.setClock(100000);

  // Soft reset both candidate addresses, then pick the one that answers 0x24.
  // (If the device isn’t on the bus yet, these writes are harmless.)
  uint8_t SOFT_RESET = 0xB6;
  (void)boschI2CWrite(0x7E, &SOFT_RESET, 1, impl); delay(10);
  (void)boschI2CWrite(0x7E, &SOFT_RESET, 1, impl); delay(10);

  if (!selectAddress(*impl)) {
    LOG_DEBUG("BMI270: WHOAMI 0x24 not found at 0x69/0x68");
    delete impl;
    return false;
  }

  // Hook up Bosch device
  impl->dev.intf = BMI2_I2C_INTF;
  impl->dev.read = &boschI2CRead;
  impl->dev.write = &boschI2CWrite;
  impl->dev.delay_us = &boschDelayUs;
  impl->dev.intf_ptr = impl;
  impl->dev.read_write_len = 16; // safer on some cores
  impl->dev.config_file_ptr = NULL;

  // Extra soft reset via API then init
  (void)bmi2_soft_reset(&impl->dev);
  delay(10);

  int8_t err = bmi270_init(&impl->dev);
  if (err != BMI2_OK) {
    // One retry after reset
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

  // Configure ODR/range/bandwidth
  bmi2_sens_config cfgs[2];
  cfgs[0].type = BMI2_ACCEL;
  cfgs[1].type = BMI2_GYRO;

  if (bmi270_get_sensor_config(cfgs, 2, &impl->dev) != BMI2_OK) {
    delete impl; return false;
  }

  // Accel: 100Hz, normal bandwidth, ±4g
  cfgs[0].cfg.acc.odr   = BMI2_ACC_ODR_100HZ;
  cfgs[0].cfg.acc.bwp   = BMI2_ACC_NORMAL_AVG4;
  cfgs[0].cfg.acc.range = BMI2_ACC_RANGE_4G;

  // Gyro: 100Hz, normal bandwidth, 2000 dps
  cfgs[1].cfg.gyr.odr   = BMI2_GYR_ODR_100HZ;
  cfgs[1].cfg.gyr.bwp   = BMI2_GYR_NORMAL_MODE;
  cfgs[1].cfg.gyr.range = BMI2_GYR_RANGE_2000;

  if (bmi270_set_sensor_config(cfgs, 2, &impl->dev) != BMI2_OK) {
    delete impl; return false;
  }

  // Optional: restore fast I2C
  // Wire.setClock(400000);

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
    auto* impl = static_cast<BMI270Impl*>(impl_);
    bmi2_sens_data raw{};
    if (bmi2_get_sensor_data(&raw, &impl->dev) == BMI2_OK) {
      // Convert to g / dps (matching the ranges we set)
      const float aScale = 4.0f / 32768.0f;
      const float gScale = 2000.0f / 32768.0f;

      const float ax = raw.acc.x * aScale;
      const float ay = raw.acc.y * aScale;
      const float az = raw.acc.z * aScale;

      const float gx = raw.gyr.x * gScale;
      const float gy = raw.gyr.y * gScale;
      const float gz = raw.gyr.z * gScale;

      (void)ax; (void)ay; (void)az; (void)gx; (void)gy; (void)gz;
      
      if (config.display.wake_on_tap_or_motion) {
        // ---- Software motion / tap detector (Meshtastic style) ----
        // Motion -> wakeScreen()
        // Double-tap -> buttonPress()

        // Tunables
        const float MOTION_DELTA_G       = 0.15f;   // motion sensitivity (sum of delta |a|)
        const uint32_t MOTION_DEBOUNCE   = 200;     // ms between motion wakes
        const float TAP_PEAK_G           = 1.1f;    // g peak above 1g (tap impulse ~1.1–1.6g)
        const uint32_t TAP_WINDOW        = 120;     // ms: window to detect a single tap impulse
        const uint32_t DOUBLE_TAP_GAP_MS = 300;     // ms between two taps for double-tap

        // State
        static float lastAx = ax, lastAy = ay, lastAz = az;
        static uint32_t lastMotionTs = 0;
        static uint32_t tapStartTs   = 0;
        static bool     tapArmed     = false;
        static uint32_t lastTapTs    = 0;

        // ---- motion (delta accel) ----
        float dax = fabsf(ax - lastAx);
        float day = fabsf(ay - lastAy);
        float daz = fabsf(az - lastAz);
        if ((dax + day + daz) > MOTION_DELTA_G) {
            const uint32_t now = millis();
            if (now - lastMotionTs > MOTION_DEBOUNCE) {
                lastMotionTs = now;
                wakeScreen();        // <— Meshtastic way
            }
        }
        lastAx = ax; lastAy = ay; lastAz = az;

        // ---- single/double tap (impulse on |a|-1g) ----
        const float aMag = sqrtf(ax*ax + ay*ay + az*az);   // g units
        const float aExcess = aMag - 1.0f;

        const uint32_t now = millis();
        if (!tapArmed) {
            // arm when we see a rising edge
            if (aExcess > 0.10f) { tapArmed = true; tapStartTs = now; }
        } else {
            // look for a peak quickly after arm
            if (now - tapStartTs <= TAP_WINDOW) {
                if (aExcess > TAP_PEAK_G) {
                    // tap detected
                    if ((now - lastTapTs) <= DOUBLE_TAP_GAP_MS) {
                        // double-tap -> button press
                        buttonPress();               // <— Meshtastic way
                        lastTapTs = 0;               // consume pair
                    } else {
                        lastTapTs = now;             // first tap; wait for second
                        wakeScreen();                // wake on single tap too
                    }
                    tapArmed = false;
                }
            } else {
                // window expired
                tapArmed = false;
            }
        }
      }
    }
  }

#endif
  return MOTION_SENSOR_CHECK_INTERVAL_MS;
}

void BMI270Sensor::calibrate(uint16_t /*forSeconds*/)
{
  // Optional: add FOC/CRT using bmi2_perform_* APIs if you want bias calibration.
}

#endif // build guard
