#pragma once
#ifndef _QMI8658_SENSOR_H_
#define _QMI8658_SENSOR_H_

#include "MotionSensor.h"

#if !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C && __has_include(<SensorQMI8658.hpp>)

#include <SensorQMI8658.hpp>

class QMI8658Sensor : public MotionSensor
{
  private:
    SensorQMI8658 sensor;
#ifdef QMI8658_SOFTWARE_MOTION_WAKE
    float gravityX = 0, gravityY = 0, gravityZ = 0; // slow average that each sample is compared against
    bool gravityPrimed = false;
    uint8_t motionSamples = 0; // consecutive samples over the wake threshold
#endif
#ifdef SHOW_STEP_COUNTER
    float stepBaselineG = 1.0f; // slow average of accel magnitude; a step is an excursion above it
    bool stepArmed = true;      // cleared on a peak, re-armed once the signal falls back through the average
    uint32_t lastStepMs = 0;
    uint8_t stepRun = 0; // plausible steps in the current run
    uint32_t stepCount = 0;

    void detectStep(float magnitudeG);
#endif

  public:
    explicit QMI8658Sensor(ScanI2C::FoundDevice foundDevice);
    virtual bool init() override;
    virtual int32_t runOnce() override;
};

#endif

#endif
