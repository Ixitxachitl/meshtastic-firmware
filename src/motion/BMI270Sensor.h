#pragma once
#ifndef _BMI270_SENSOR_H_
#define _BMI270_SENSOR_H_

#include "MotionSensor.h"

struct Quat; // forward declaration for getAttitudeQuat()

#if !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C

// We keep Bosch types out of the header (opaque pointer).
class BMI270Sensor : public MotionSensor
{
  public:
    explicit BMI270Sensor(ScanI2C::FoundDevice foundDevice);

    bool init() override;
    int32_t runOnce() override;
    void calibrate(uint16_t forSeconds) override;

    // 3D compass: get current orientation as quaternion
    Quat getAttitudeQuat() const;

    // New: request visual re-anchor (optional)
    void anchorHeading3D();

    // Step counter (simplified implementation)
    uint32_t getStepCount() const;

  private:
    // Implementation owned by the .cpp (holds bmi2_dev, bus, addr…)
    void *impl_ = nullptr;
    bool inited_ = false;

    // Motion detection setup
    bool setupMotionDetection();
};

#else

// Stub for unsupported targets
class BMI270Sensor : public MotionSensor
{
  public:
    explicit BMI270Sensor(ScanI2C::FoundDevice fd) : MotionSensor(fd) {}
    bool init() override { return false; }
    int32_t runOnce() override { return MOTION_SENSOR_CHECK_INTERVAL_MS; }
    void calibrate(uint16_t) override {}
};

#endif // !ARCH_STM32WL && !MESHTASTIC_EXCLUDE_I2C
#endif // _BMI270_SENSOR_H_
