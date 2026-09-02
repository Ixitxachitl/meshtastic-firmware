#pragma once
#ifndef _BHI260AP_SENSOR_H_
#define _BHI260AP_SENSOR_H_

#include "MotionSensor.h"

#if !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C && defined(HAS_BHI260AP) && __has_include(<SensorBHI260AP.hpp>)

// Sensor lib
#include "../detect/ReClockI2C.h"
#include <SensorBHI260AP.hpp>
#include <Wire.h>
#include <bosch/BoschSensorDataHelper.hpp>

// Opt-in per variant: wake the screen on a BHI260AP gesture (config.display.wake_on_tap_or_motion).
// Off by default - it needs BHI260AP_REMAP_AXES measured for the board, and which detectors suit
// the device depends on how it is carried. See BHI260APSensor.cpp.
#ifndef BHI260AP_WAKE_ON_MOTION
#define BHI260AP_WAKE_ON_MOTION 0
#endif

class BHI260APSensor : public MotionSensor
{
  private:
    SensorBHI260AP sensor;
    volatile bool BHI_IRQ = false;
    SensorStepCounter *stepCounter;
    SensorStepDetector *stepDetector;
    uint32_t steps = 0;

    // Present when the variant sets BHI260AP_I2C_CLOCK_SPEED, to reclock the bus for the firmware
    // upload - most of a boot's stall is I2C bit time at the 100kHz default. See BHI260APSensor.cpp.
#ifdef BHI260AP_I2C_CLOCK_SPEED
    ReClockI2C reClockI2C;
#endif

#if BHI260AP_WAKE_ON_MOTION
    // Virtual sensor driving wake-on-motion, or 0 if the loaded firmware image offers none. Plain
    // uint8_t rather than BoschSensorID: the BHI3 gesture IDs are #defines outside that enum.
    uint8_t wakeGesture = 0;
    float wakeGestureRate = 0;
    bool wakeGestureFired = false;

    // Pick the best wake trigger the running firmware image actually reports as available
    uint8_t selectWakeGesture(const BoschSensorInfo &info) const;
    static void onWakeGesture(uint8_t sensor_id, uint8_t *data, uint32_t size, uint64_t *timestamp, void *user_data);
#endif

  public:
    explicit BHI260APSensor(ScanI2C::FoundDevice foundDevice);
    virtual bool init() override;
    virtual int32_t runOnce() override;
};

#endif

#endif