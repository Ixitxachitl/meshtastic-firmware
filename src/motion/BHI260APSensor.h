#pragma once
#ifndef _BHI260AP_SENSOR_H_
#define _BHI260AP_SENSOR_H_

#include "MotionSensor.h"

#if !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C

// Threshold for motion detection (configurable)
#ifndef BHI260AP_WAKE_THRESH
#define BHI260AP_WAKE_THRESH 20
#endif

// Forward declare for external access
class BHI260APSensor;
extern BHI260APSensor *g_bhi260ap_instance;

class BHI260APSensor : public MotionSensor
{
  private:
    TwoWire *wire = nullptr;
    uint8_t i2cAddress = 0x28;
    bool initialized = false;
    float lastAccelX = 0;
    float lastAccelY = 0;
    float lastAccelZ = 0;
    uint32_t lastMotionTime = 0;
    uint32_t stepCount = 0;

    // Helper to read step counter from FIFO
    bool readStepCounterFromFifo();

  public:
    explicit BHI260APSensor(ScanI2C::FoundDevice foundDevice);
    virtual ~BHI260APSensor();
    virtual bool init() override;
    virtual int32_t runOnce() override;

    // Step counter accessor
    uint32_t getStepCount() const { return stepCount; }
};

#endif

#endif
