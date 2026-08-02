#pragma once
#ifndef _BHI260AP_SENSOR_H_
#define _BHI260AP_SENSOR_H_

#include "MotionSensor.h"

#if !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C && defined(HAS_BHI260AP) && __has_include(<SensorBHI260AP.hpp>)

// Sensor lib
#include <SensorBHI260AP.hpp>
#include <Wire.h>
#include <bosch/BoschSensorDataHelper.hpp>

class BHI260APSensor : public MotionSensor
{
  private:
    SensorBHI260AP sensor;
    volatile bool BHI_IRQ = false;
    SensorStepCounter *stepCounter;
    SensorStepDetector *stepDetector;
    uint32_t steps = 0;

    // Virtual sensor driving wake-on-motion, or 0 if the loaded firmware image offers none. Plain
    // uint8_t rather than BoschSensorID: the BHI3 gesture IDs are #defines outside that enum.
    uint8_t wakeGesture = 0;
    float wakeGestureRate = 0;
    bool wakeGestureFired = false;

    // Pick the best wake trigger the running firmware image actually reports as available
    uint8_t selectWakeGesture(const BoschSensorInfo &info) const;
    static void onWakeGesture(uint8_t sensor_id, uint8_t *data, uint32_t size, uint64_t *timestamp, void *user_data);

  public:
    explicit BHI260APSensor(ScanI2C::FoundDevice foundDevice);
    virtual bool init() override;
    virtual int32_t runOnce() override;
};

#endif

#endif