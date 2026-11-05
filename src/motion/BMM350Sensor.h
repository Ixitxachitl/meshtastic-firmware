#pragma once
#ifndef _BMM_350_SENSOR_H_
#define _BMM_350_SENSOR_H_

#include "MotionSensor.h"

#if !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C && defined(HAS_BMM350) && __has_include(<DFRobot_BMM350.h>)

#include <DFRobot_BMM350.h>

// The library keeps its device state in file-scope globals, so only one BMM350 can be driven
class BMM350Sensor : public MotionSensor
{
  private:
    DFRobot_BMM350_I2C *sensor = nullptr;
    bool showingScreen = false;
    bool seedPending = false; // seed the calibration extrema from the first valid sample of the window
    bool warnedNoTilt = false;
    static constexpr const char *compassCalibrationFileName = "/prefs/compass_bmm350.dat";
    float highestX = 0, lowestX = 0, highestY = 0, lowestY = 0, highestZ = 0, lowestZ = 0;

    bool readMagnetometer(float &xMicroTesla, float &yMicroTesla, float &zMicroTesla);

  public:
    explicit BMM350Sensor(ScanI2C::FoundDevice foundDevice);

    virtual bool init() override;
    virtual int32_t runOnce() override;
    virtual void calibrate(uint16_t forSeconds) override;
    virtual bool providesHeading() const override { return true; }
};

#endif

#endif
