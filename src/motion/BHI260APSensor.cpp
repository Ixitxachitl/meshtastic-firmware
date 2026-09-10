#include "BHI260APSensor.h"

#if !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C && defined(HAS_BHI260AP) && __has_include(<SensorBHI260AP.hpp>)
#define BOSCH_BHI260_KLIO

#include <BoschFirmware.h>
#include <bosch/bhi260x/bhi3_defs.h>
#include <bosch/bhi260x/bhi3_multi_tap_defs.h>

// Candidate wake-on-motion triggers, best first for a wrist-worn device: raise-to-wake gestures
// ahead of taps, taps ahead of the blunt any-motion detectors. Which of these exist depends on the
// firmware image loaded into the BHI260AP above, so init() picks the first one the running image
// reports as available rather than assuming.
static const uint8_t wakeGestureCandidates[] = {
    BHI3_SENSOR_ID_WRIST_WEAR_LP_WU,        // Wrist wear (raise-to-wake), low power
    BoschSensorID::WRIST_TILT_GESTURE,      //
    BHI3_SENSOR_ID_WRIST_GEST_DETECT_LP_WU, //
    BoschSensorID::PICKUP_GESTURE,          //
    BHI3_SENSOR_ID_MULTI_TAP,               // Tap-to-wake
    BoschSensorID::WAKE_GESTURE,            //
    BoschSensorID::GLANCE_GESTURE,          //
    BoschSensorID::TILT_DETECTOR,           //
    BoschSensorID::MOTION_DETECT,           //
    BoschSensorID::ANY_MOTION_LOW_POWER_WAKE_UP,
    BoschSensorID::ANY_MOTION_LOW_POWER,
};

// Gesture sensors report on an event, not on a schedule, so this only sets how often the part
// evaluates the detector. Keep it modest - it runs whenever the watch is awake.
#define BHI260AP_WAKE_GESTURE_RATE_HZ 25.0f

// How the part is mounted on the board, as one of the SensorRemap constants (diagrams in
// sensor/SensorDefs.hpp). Only the gesture detectors below care, and they fail silently when it is
// wrong, so measure rather than guess: log the gravity vector with the remap set to
// TOP_LAYER_LEFT_CORNER (identity) and find the constant that lands the board's own axes on X at 3
// o'clock, Y at 12, Z out of the screen. Boards that have not been measured keep the value this
// driver has always used.
#ifndef BHI260AP_REMAP_AXES
#define BHI260AP_REMAP_AXES TOP_LAYER_BOTTOM_RIGHT_CORNER
#endif

BHI260APSensor::BHI260APSensor(ScanI2C::FoundDevice foundDevice) : MotionSensor::MotionSensor(foundDevice) {}
// https://github.com/lewisxhe/SensorLib/blob/master/examples/Sensors/IMU/BHI260AP_InterruptSettings/BHI260AP_InterruptSettings.ino

bool BHI260APSensor::init()
{
    LOG_WARN("Initializing BHI260AP sensor %u", deviceAddress());
    sensor.setFirmware(bosch_firmware_image, bosch_firmware_size, bosch_firmware_type);
    sensor.setBootFromFlash(bosch_firmware_type);
    if (sensor.begin(Wire, deviceAddress())) {
        sensor.setRemapAxes(SensorRemap::BHI260AP_REMAP_AXES);
        BoschSensorInfo info = sensor.getSensorInfo();

        LOG_INFO("Product ID     : %02x\n", info.getProductId());
        LOG_INFO("Kernel version : %04u\n", info.getKernelVersion());
        LOG_INFO("User version   : %04u\n", info.getUserVersion());
        LOG_INFO("ROM version    : %04u\n", info.getRomVersion());
        LOG_INFO("Power state    : %s\n", (info.getHostStatus() & BHY2_HST_POWER_STATE) ? "sleeping" : "active");
        LOG_INFO("Host interface : %s\n", (info.getHostStatus() & BHY2_HST_HOST_PROTOCOL) ? "SPI" : "I2C");
        LOG_INFO("Feature status : 0x%02x\n", info.getFeatStatus());

        stepCounter = new SensorStepCounter(sensor);
        // stepDetector = new SensorStepDetector(sensor);

        // sensor.configAccelerometer(sensor.RANGE_2G, sensor.ODR_100HZ, sensor.BW_NORMAL_AVG4, sensor.PERF_CONTINUOUS_MODE);
        // sensor.enableAccelerometer();
        // sensor.configInterrupt();

#ifdef BHI260AP_INT
        pinMode(BHI260AP_INT, INPUT);
        attachInterrupt(
            BHI260AP_INT,
            [] {
                // Set interrupt to set irq value to true
            },
            RISING); // Select the interrupt mode according to the actual circuit
#endif

        // stepDetector->enable(1.0, 0);
        stepCounter->enable(1.0, 0);

        // Wake-on-motion (config.display.wake_on_tap_or_motion). Unlike the BMA423 there is no
        // fixed tilt/doubletap feature to switch on - we enable whichever gesture virtual sensor
        // this firmware image provides and wake the screen when it reports.
        wakeGesture = selectWakeGesture(info);
        if (wakeGesture) {
            wakeGestureRate = BHI260AP_WAKE_GESTURE_RATE_HZ;
            const bhy2_sensor_info *rates = info.getSensorInfo();
            if (rates) {
                float maxRate = rates[wakeGesture].max_rate.f_val;
                float minRate = rates[wakeGesture].min_rate.f_val;
                if (maxRate > 0 && wakeGestureRate > maxRate)
                    wakeGestureRate = maxRate;
                if (wakeGestureRate < minRate)
                    wakeGestureRate = minRate;
            }

            sensor.onResultEvent(wakeGesture, onWakeGesture, this);
            if (sensor.configure(wakeGesture, wakeGestureRate, 0)) {
                LOG_INFO("BHI260AP wake-on-motion using %s (id %u) at %.1fHz", BoschSensorInfo::getSensorName(wakeGesture),
                         wakeGesture, wakeGestureRate);
            } else {
                LOG_WARN("BHI260AP failed to enable wake gesture %u, wake-on-motion disabled", wakeGesture);
                wakeGesture = 0;
            }
        } else {
            LOG_WARN("BHI260AP firmware offers no wake gesture, wake-on-motion unavailable");
        }

        LOG_DEBUG("BHI260AP init ok");
        return true;
    }
    LOG_DEBUG("BHI260AP init failed");
    return false;
}

uint8_t BHI260APSensor::selectWakeGesture(const BoschSensorInfo &info) const
{
    if (!info.isValid())
        return 0;

    for (uint8_t candidate : wakeGestureCandidates) {
        if (info.isSensorAvailable(candidate))
            return candidate;
    }
    return 0;
}

// Called from sensor.update() (thread context, not the ISR) as the FIFO is drained
void BHI260APSensor::onWakeGesture(uint8_t sensor_id, const uint8_t *data, uint32_t size, uint64_t *timestamp, void *user_data)
{
    // Every one of the candidate sensors reports only when its gesture fires, so the event itself
    // is the signal - the payload (which tap, which wrist gesture) is not needed to wake
    static_cast<BHI260APSensor *>(user_data)->wakeGestureFired = true;
}

int32_t BHI260APSensor::runOnce()
{
    sensor.update();
    if (stepCounter->hasUpdated()) {
        steps = stepCounter->getStepCount();
        LOG_DEBUG("Step count updated: %u", steps);
        if (screen)
            screen->steps = steps;
    }

    if (wakeGestureFired) {
        wakeGestureFired = false;
        LOG_DEBUG("BHI260AP wake gesture detected");
        wakeScreen(); // no-ops unless the screen is off and wake_on_tap_or_motion is set

        // One-shot virtual sensors (significant motion, wake gesture) disarm themselves once they
        // report. Re-arming is a harmless rewrite of the config for the on-change ones.
        sensor.configure(wakeGesture, wakeGestureRate, 0);
    }

    // Only the wake gesture needs a fast poll; the step counter is happy once a second
    return (wakeGesture && config.display.wake_on_tap_or_motion) ? MOTION_SENSOR_CHECK_INTERVAL_MS : 1000;
}

#endif