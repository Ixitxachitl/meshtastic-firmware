#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR && (__has_include(<bsec2.h>) || __has_include(<Adafruit_BME680.h>))

#include "../mesh/generated/meshtastic/telemetry.pb.h"
#include "BME680Sensor.h"
#include "FSCommon.h"
#include "SPILock.h"
#include "TelemetrySensor.h"

// Static member definitions (shared across instances to save heap)
uint8_t BME680Sensor::bsecState[BSEC_MAX_STATE_BLOB_SIZE] = {0};
bsecSensor BME680Sensor::sensorList[9] = {BSEC_OUTPUT_IAQ,
                                          BSEC_OUTPUT_RAW_TEMPERATURE,
                                          BSEC_OUTPUT_RAW_PRESSURE,
                                          BSEC_OUTPUT_RAW_HUMIDITY,
                                          BSEC_OUTPUT_RAW_GAS,
                                          BSEC_OUTPUT_STABILIZATION_STATUS,
                                          BSEC_OUTPUT_RUN_IN_STATUS,
                                          BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE,
                                          BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY};

BME680Sensor::BME680Sensor() : TelemetrySensor(meshtastic_TelemetrySensorType_BME680, "BME680") {}

#if __has_include(<bsec2.h>)
int32_t BME680Sensor::runOnce()
{
    static uint32_t next_due = 0;
    const uint32_t now = millis();
    if ((int32_t)(now - next_due) < 0)
        return (int32_t)(next_due - now);

#if defined(M5STACK_UNITC6L) && defined(GROVE_SDA) && defined(GROVE_SCL)
    // ESP32-C6: Temporarily switch to Grove pins if sensor is on WIRE1
    bool needsPinSwitch = (deviceAddress.port == ScanI2C::I2CPort::WIRE1);
    if (needsPinSwitch) {
        Wire.end();
        Wire.begin(GROVE_SDA, GROVE_SCL);
        Wire.setClock(100000);
        delay(5);
    }
#endif

    if (!bme680.run()) {
        checkStatus("runTrigger");
#if defined(M5STACK_UNITC6L) && defined(GROVE_SDA) && defined(GROVE_SCL)
        // Restore internal I2C pins
        if (needsPinSwitch) {
            Wire.end();
            Wire.begin(I2C_SDA, I2C_SCL);
            Wire.setClock(100000);
        }
#endif
        next_due = now + 3000;
        return 3000;
    }

#if defined(__has_include)
// Most Arduino BSEC2 ports expose a 'gasResistance' field updated by run()
#if __has_include(<bsec2.h>)
    // If your wrapper has a public field:
    // (If this line fails, try one of the alt code paths below)
    lastGasOhms = bme680.getData(BSEC_OUTPUT_RAW_GAS).signal;
#endif
#endif

#if defined(M5STACK_UNITC6L) && defined(GROVE_SDA) && defined(GROVE_SCL)
    // Restore internal I2C pins after reading
    if (needsPinSwitch) {
        Wire.end();
        Wire.begin(I2C_SDA, I2C_SCL);
        Wire.setClock(100000);
    }
#endif

    next_due = now + 3000;
    return 3000;
}
#endif

bool BME680Sensor::initDevice(TwoWire *bus, ScanI2C::FoundDevice *dev)
{
    status = 0;
    deviceAddress = dev->address; // Store for pin switching

#if defined(M5STACK_UNITC6L) && defined(GROVE_SDA) && defined(GROVE_SCL)
    // ESP32-C6: Switch to Grove pins if sensor is on WIRE1
    bool needsPinSwitch = (dev->address.port == ScanI2C::I2CPort::WIRE1);
    if (needsPinSwitch) {
        Wire.end();
        Wire.begin(GROVE_SDA, GROVE_SCL);
        Wire.setClock(100000);
        delay(10);
        bus = &Wire; // Use the reconfigured Wire
    }
#endif

#if __has_include(<bsec2.h>)
    if (!bme680.begin(dev->address.address, *bus))
        checkStatus("begin");

    if (bme680.status == BSEC_OK) {
        status = 1;
        if (!bme680.setConfig(bsec_config)) {
            checkStatus("setConfig");
            status = 0;
        }
        loadState();
        if (!bme680.updateSubscription(sensorList, ARRAY_LEN(sensorList), BSEC_SAMPLE_RATE_LP)) {
            checkStatus("updateSubscription");
            status = 0;
        }
        LOG_INFO("Init sensor: %s with the BSEC Library version %d.%d.%d.%d ", sensorName, bme680.version.major,
                 bme680.version.minor, bme680.version.major_bugfix, bme680.version.minor_bugfix);
    }

    if (status == 0)
        LOG_DEBUG("BME680Sensor::runOnce: bme680.status %d", bme680.status);

#if defined(M5STACK_UNITC6L) && defined(GROVE_SDA) && defined(GROVE_SCL)
    // Restore internal I2C pins after initialization
    if (needsPinSwitch) {
        Wire.end();
        Wire.begin(I2C_SDA, I2C_SCL);
        Wire.setClock(100000);
    }
#endif

#else
    bme680 = makeBME680(bus);

    if (!bme680->begin(dev->address.address)) {
        LOG_ERROR("Init sensor: %s failed at begin()", sensorName);
        return status;
    }

    status = 1;

#endif

    initI2CSensor();
    return status;
}

bool BME680Sensor::getMetrics(meshtastic_Telemetry *measurement)
{
#if __has_include(<bsec2.h>)
    if (bme680.getData(BSEC_OUTPUT_RAW_PRESSURE).signal == 0)
        return false;

    // Pull once (BSEC units: °C, %rH, Pa, Ω, IAQ unitless)
    const float temp_c = bme680.getData(BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE).signal;
    const float rh_pct = bme680.getData(BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY).signal;
    const float pres_pa = bme680.getData(BSEC_OUTPUT_RAW_PRESSURE).signal;
    const float gas_ohms = bme680.getData(BSEC_OUTPUT_RAW_GAS).signal; // already in Ω
    const float gas_kohms = gas_ohms / 1000.0f;                        // kΩ
    const float iaq = bme680.getData(BSEC_OUTPUT_IAQ).signal;

    // Mark present
    measurement->variant.environment_metrics.has_temperature = true;
    measurement->variant.environment_metrics.has_relative_humidity = true;
    measurement->variant.environment_metrics.has_barometric_pressure = true;
    measurement->variant.environment_metrics.has_gas_resistance = true;
    measurement->variant.environment_metrics.has_iaq = true;

    // Assign
    measurement->variant.environment_metrics.temperature = temp_c;
    measurement->variant.environment_metrics.relative_humidity = rh_pct;
    measurement->variant.environment_metrics.barometric_pressure = pres_pa; // Pa
    measurement->variant.environment_metrics.gas_resistance = gas_kohms;    // kΩ
    measurement->variant.environment_metrics.iaq = iaq;

    // Persist BSEC state periodically
    updateState();
#else
    if (!bme680->performReading()) {
        LOG_ERROR("BME680Sensor::getMetrics: performReading failed");
        return false;
    }

    measurement->variant.environment_metrics.has_temperature = true;
    measurement->variant.environment_metrics.has_relative_humidity = true;
    measurement->variant.environment_metrics.has_barometric_pressure = true;
    measurement->variant.environment_metrics.has_gas_resistance = true;

    measurement->variant.environment_metrics.temperature = bme680->readTemperature();
    measurement->variant.environment_metrics.relative_humidity = bme680->readHumidity();
    measurement->variant.environment_metrics.barometric_pressure = bme680->readPressure() / 100.0F;
    measurement->variant.environment_metrics.gas_resistance = bme680->readGas() / 1000.0;

#endif
    return true;
}

#if __has_include(<bsec2.h>)
void BME680Sensor::loadState()
{
#ifdef FSCom
    spiLock->lock();
    auto file = FSCom.open(bsecConfigFileName, FILE_O_READ);
    if (file) {
        file.read((uint8_t *)&bsecState, BSEC_MAX_STATE_BLOB_SIZE);
        file.close();
        bme680.setState(bsecState);
        LOG_INFO("%s state read from %s", sensorName, bsecConfigFileName);
    } else {
        LOG_INFO("No %s state found (File: %s)", sensorName, bsecConfigFileName);
    }
    spiLock->unlock();
#else
    LOG_ERROR("ERROR: Filesystem not implemented");
#endif
}

void BME680Sensor::updateState()
{
#ifdef FSCom
    spiLock->lock();
    bool update = false;
    if (stateUpdateCounter == 0) {
        /* First state update when IAQ accuracy is >= 3 */
        accuracy = bme680.getData(BSEC_OUTPUT_IAQ).accuracy;
        if (accuracy >= 2) {
            LOG_DEBUG("%s state update IAQ accuracy %u >= 2", sensorName, accuracy);
            update = true;
            stateUpdateCounter++;
        } else {
            LOG_DEBUG("%s not updated, IAQ accuracy is %u < 2", sensorName, accuracy);
        }
    } else {
        /* Update every STATE_SAVE_PERIOD minutes */
        if ((stateUpdateCounter * STATE_SAVE_PERIOD) < millis()) {
            LOG_DEBUG("%s state update every %d minutes", sensorName, STATE_SAVE_PERIOD / 60000);
            update = true;
            stateUpdateCounter++;
        }
    }

    if (update) {
        bme680.getState(bsecState);
        if (FSCom.exists(bsecConfigFileName) && !FSCom.remove(bsecConfigFileName)) {
            LOG_WARN("Can't remove old state file");
        }
        auto file = FSCom.open(bsecConfigFileName, FILE_O_WRITE);
        if (file) {
            LOG_INFO("%s state write to %s", sensorName, bsecConfigFileName);
            file.write((uint8_t *)&bsecState, BSEC_MAX_STATE_BLOB_SIZE);
            file.flush();
            file.close();
        } else {
            LOG_INFO("Can't write %s state (File: %s)", sensorName, bsecConfigFileName);
        }
    }
    spiLock->unlock();
#else
    LOG_ERROR("ERROR: Filesystem not implemented");
#endif
}

void BME680Sensor::checkStatus(const char *functionName)
{
    // BSEC status
    if (bme680.status < BSEC_OK) {
        LOG_ERROR("%s BSEC2 code: %d", functionName, bme680.status);
    } else if (bme680.status > BSEC_OK) {
        // Quiet timing warning if desired; keep others as WARN
        if (bme680.status == 100) { // BSEC_W_SC_CALL_TIMING_VIOLATION
            LOG_DEBUG("%s BSEC2 code: %d", functionName, bme680.status);
        } else {
            LOG_WARN("%s BSEC2 code: %d", functionName, bme680.status);
        }
    }

    // BME68X driver status
    if (bme680.sensor.status < BME68X_OK) {
        LOG_ERROR("%s BME68X code: %d", functionName, bme680.sensor.status);
    } else if (bme680.sensor.status > BME68X_OK) {
        LOG_WARN("%s BME68X code: %d", functionName, bme680.sensor.status);
    }
}
#endif

#endif
