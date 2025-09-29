#pragma once
#ifndef _BMI270_SENSOR_H_
#define _BMI270_SENSOR_H_

#include "MotionSensor.h"

#if !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C

// We keep Bosch types out of the header (opaque pointer).
class BMI270Sensor : public MotionSensor
{
public:
    explicit BMI270Sensor(ScanI2C::FoundDevice foundDevice);

    bool init() override;
    int32_t runOnce() override;
    void calibrate(uint16_t forSeconds) override;

private:
    // Implementation owned by the .cpp (holds bmi2_dev, bus, addr…)
    void* impl_ = nullptr;
    bool inited_ = false;
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
