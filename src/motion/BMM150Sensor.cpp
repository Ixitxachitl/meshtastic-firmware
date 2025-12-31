#include "BMM150Sensor.h"

#if !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C && __has_include(<DFRobot_BMM150.h>)

extern "C" {
volatile bool g_hasMagHeading = false;
volatile float g_magHeadingRad = 0.0f; // radians, 0 = North, +CW
#if !MESHTASTIC_EXCLUDE_BMI270
void GetGravityXYZ(float *gx, float *gy, float *gz);
#endif
}

#if !defined(MESHTASTIC_EXCLUDE_SCREEN)

// screen is defined in main.cpp
extern graphics::Screen *screen;
#endif

// Flag when an interrupt has been detected
volatile static bool BMM150_IRQ = false;

BMM150Sensor::BMM150Sensor(ScanI2C::FoundDevice foundDevice) : MotionSensor::MotionSensor(foundDevice) {}

bool BMM150Sensor::init()
{
    // Initialise the sensor
    sensor = BMM150Singleton::GetInstance(device);
    return sensor->init(device);
}

void BMM150Sensor::calReset_()
{
    calActive_ = false;
    calEndMs_ = 0;
    calSamples_ = 0;
    min_[0] = min_[1] = min_[2] = +1e9f;
    max_[0] = max_[1] = max_[2] = -1e9f;
}

void BMM150Sensor::calibrate(uint16_t seconds)
{
    // (Re)start background capture; user can keep using device while moving it around
    calActive_ = true;
    calEndMs_ = millis() + (uint32_t)seconds * 1000u;
    calSamples_ = 0;
    min_[0] = min_[1] = min_[2] = +1e9f;
    max_[0] = max_[1] = max_[2] = -1e9f;
}

void BMM150Sensor::calPush_(float mx, float my, float mz)
{
    if (mx < min_[0])
        min_[0] = mx;
    if (mx > max_[0])
        max_[0] = mx;

    if (my < min_[1])
        min_[1] = my;
    if (my > max_[1])
        max_[1] = my;

    if (mz < min_[2])
        min_[2] = mz;
    if (mz > max_[2])
        max_[2] = mz;

    ++calSamples_;
}

void BMM150Sensor::calSolve_()
{
    for (int i = 0; i < 3; ++i)
        cal_.offset[i] = 0.5f * (max_[i] + min_[i]);
    float r[3] = {0.5f * (max_[0] - min_[0]), 0.5f * (max_[1] - min_[1]), 0.5f * (max_[2] - min_[2])};
    float rmean = (r[0] + r[1] + r[2]) / 3.0f;
    for (int i = 0; i < 3; ++i)
        cal_.scale[i] = (r[i] > 1e-3f) ? (rmean / r[i]) : 1.0f;
    cal_.valid = true;
    calActive_ = false;
    // TODO: persist cal_ to NVS/preferences if you want it across reboots
}

int32_t BMM150Sensor::runOnce()
{
#if !defined(MESHTASTIC_EXCLUDE_SCREEN) && HAS_SCREEN
    // --- read raw magnetic field (uT or counts depending on driver) ---
    float mx = 0, my = 0, mz = 0;
    auto m = sensor->getGeomagneticData(); // returns sBmm150MagData_t
    mx = m.x;
    my = m.y;
    mz = m.z;

    // --- background calibration sampling/finish ---
    if (calActive_) {
        calPush_(mx, my, mz);
        if ((int32_t)(millis() - calEndMs_) >= 0) {
            const float spanX = max_[0] - min_[0];
            const float spanY = max_[1] - min_[1];
            const float spanZ = max_[2] - min_[2];
            if (calSamples_ >= 200 && (spanX > 20 && spanY > 20 && spanZ > 20)) {
                calSolve_();
            } else {
                calActive_ = false; // not enough motion; keep previous cal (if any)
            }
        }
    }

    // --- apply calibration (if any) ---
    const float mxc = (mx - cal_.offset[0]) * cal_.scale[0];
    const float myc = (my - cal_.offset[1]) * cal_.scale[1];
    const float mzc = (mz - cal_.offset[2]) * cal_.scale[2];

    // --- compute heading (tilt-comp if BMI270 gravity is available; else flat) ---
    float headingDeg = 0.0f;

    // Get gravity (unit vector) from BMI270
    float gx = 0, gy = 0, gz = 1;
#if !MESHTASTIC_EXCLUDE_BMI270
    GetGravityXYZ(&gx, &gy, &gz); // provided by BMI270Sensor.cpp
#endif

    // Roll/Pitch from gravity (radians)
    const float pitch = asinf(-gx);    // +pitch = nose up
    const float roll = atan2f(gy, gz); // +roll  = right wing down

    // Rotate mag vector into the horizontal plane (tilt compensation)
    const float mx2 = mxc * cosf(pitch) + mzc * sinf(pitch);
    const float my2 = mxc * sinf(roll) * sinf(pitch) + myc * cosf(roll) - mzc * sinf(roll) * cosf(pitch);

    // Heading (0° = North, +CW)
    headingDeg = atan2f(-my2, mx2) * 180.0f / (float)M_PI;
    if (headingDeg < 0)
        headingDeg += 360.0f;

    // --- apply your existing orientation mapping ---
    switch (config.display.compass_orientation) {
    case meshtastic_Config_DisplayConfig_CompassOrientation_DEGREES_0_INVERTED:
    case meshtastic_Config_DisplayConfig_CompassOrientation_DEGREES_0:
        break;
    case meshtastic_Config_DisplayConfig_CompassOrientation_DEGREES_90:
    case meshtastic_Config_DisplayConfig_CompassOrientation_DEGREES_90_INVERTED:
        headingDeg += 90;
        break;
    case meshtastic_Config_DisplayConfig_CompassOrientation_DEGREES_180:
    case meshtastic_Config_DisplayConfig_CompassOrientation_DEGREES_180_INVERTED:
        headingDeg += 180;
        break;
    case meshtastic_Config_DisplayConfig_CompassOrientation_DEGREES_270:
    case meshtastic_Config_DisplayConfig_CompassOrientation_DEGREES_270_INVERTED:
        headingDeg += 270;
        break;
    }
    if (headingDeg >= 360.0f)
        headingDeg -= 360.0f;

    // --- publish to screen (unchanged behavior) ---
    if (screen)
        screen->setHeading(headingDeg);

    // --- publish to renderer (so GetHeadingRadiansForRenderer() can prefer mag) ---
    float r = headingDeg * (float)M_PI / 180.0f;
    while (r < 0)
        r += 2.0f * (float)M_PI;
    while (r >= 2.0f * M_PI)
        r -= 2.0f * (float)M_PI;
    g_magHeadingRad = r;
    g_hasMagHeading = true;
#endif

    return MOTION_SENSOR_CHECK_INTERVAL_MS;
}

// ----------------------------------------------------------------------
// BMM150Singleton
// ----------------------------------------------------------------------

// Get a singleton wrapper for an Sparkfun BMM_150_I2C
BMM150Singleton *BMM150Singleton::GetInstance(ScanI2C::FoundDevice device)
{
#if defined(WIRE_INTERFACES_COUNT) && (WIRE_INTERFACES_COUNT > 1)
    TwoWire &bus = (device.address.port == ScanI2C::I2CPort::WIRE1 ? Wire1 : Wire);
#else
    TwoWire &bus = Wire; // fallback if only one I2C interface
#endif
    if (pinstance == nullptr) {
        pinstance = new BMM150Singleton(&bus, device.address.address);
    }
    return pinstance;
}

BMM150Singleton::~BMM150Singleton() {}

BMM150Singleton *BMM150Singleton::pinstance{nullptr};

// Initialise the BMM150 Sensor
// https://github.com/DFRobot/DFRobot_BMM150/blob/master/examples/getGeomagneticData/getGeomagneticData.ino
bool BMM150Singleton::init(ScanI2C::FoundDevice device)
{

    // startup
    LOG_DEBUG("BMM150 begin on addr 0x%02X (port=%d)", device.address.address, device.address.port);
    uint8_t status = begin();
    if (status != 0) {
        LOG_DEBUG("BMM150 init error %u", status);
        return false;
    }

    // SW reset to make sure the device starts in a known state
    setOperationMode(BMM150_POWERMODE_NORMAL);
    setPresetMode(BMM150_PRESETMODE_LOWPOWER);
    setRate(BMM150_DATA_RATE_02HZ);
    setMeasurementXYZ();
    return true;
}

#endif