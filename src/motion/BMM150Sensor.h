#pragma once
#ifndef _BMM_150_SENSOR_H_
#define _BMM_150_SENSOR_H_

#include "MotionSensor.h"

#if !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C && __has_include(<DFRobot_BMM150.h>)

#include "Fusion/Fusion.h"
#include <DFRobot_BMM150.h>

// The I2C address of the Accelerometer (if found) from main.cpp
extern ScanI2C::DeviceAddress accelerometer_found;

// Singleton wrapper
class BMM150Singleton : public DFRobot_BMM150_I2C
{
  private:
    static BMM150Singleton *pinstance;

  protected:
    BMM150Singleton(TwoWire *tw, uint8_t addr) : DFRobot_BMM150_I2C(tw, addr) {}
    ~BMM150Singleton();

  public:
    // Create a singleton instance (not thread safe)
    static BMM150Singleton *GetInstance(ScanI2C::FoundDevice device);

    // Singletons should not be cloneable.
    BMM150Singleton(BMM150Singleton &other) = delete;

    // Singletons should not be assignable.
    void operator=(const BMM150Singleton &) = delete;

    // Initialise the motion sensor singleton for normal operation
    bool init(ScanI2C::FoundDevice device);
};

class BMM150Sensor : public MotionSensor
{
  private:
    BMM150Singleton *sensor = nullptr;
    bool showingScreen = false;
    
    // --- Calibration state (background) ---
    struct MagCal {
      float offset[3] = {0,0,0};   // hard-iron offsets (uT)
      float scale[3]  = {1,1,1};   // per-axis scale (soft-iron, simple diag)
      bool  valid     = false;
    } cal_;

    bool     calActive_ = false;
    uint32_t calEndMs_  = 0;
    float    min_[3] = {+1e9f,+1e9f,+1e9f};
    float    max_[3] = {-1e9f,-1e9f,-1e9f};
    uint32_t calSamples_ = 0;

    void     calReset_();
    void     calPush_(float mx, float my, float mz);
    void     calSolve_();

  public:
    explicit BMM150Sensor(ScanI2C::FoundDevice foundDevice);

    // Initialise the motion sensor
    virtual bool init() override;

    // Called each time our sensor gets a chance to run
    virtual int32_t runOnce() override;
    
    // Trigger background auto-cal for N seconds (e.g., 10)
    void calibrate(uint16_t seconds) override;
};

#endif

#endif