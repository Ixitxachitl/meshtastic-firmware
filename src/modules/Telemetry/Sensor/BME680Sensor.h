#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR && __has_include(<bsec2.h>)

#include "../mesh/generated/meshtastic/telemetry.pb.h"
#include "TelemetrySensor.h"
#include <bsec2.h>

#define STATE_SAVE_PERIOD UINT32_C(360 * 60 * 1000) // That's 6 hours worth of millis()

// ===== Variant selection (BME680 vs BME688) =====
#ifndef BME68X_VARIANT
#define BME68X_VARIANT 680
#endif

#if BME68X_VARIANT == 688
#ifdef meshtastic_TelemetrySensorType_BME688
#define BME68X_TELEM_TYPE meshtastic_TelemetrySensorType_BME688
#else
#define BME68X_TELEM_TYPE meshtastic_TelemetrySensorType_BME680
#endif
#define SENSOR_NAME "BME688"
#else
#define BME68X_TELEM_TYPE meshtastic_TelemetrySensorType_BME680
#define SENSOR_NAME "BME680"
#endif
// ================================================

// ===== BSEC2 config blob selection (portable) ====
#ifndef BSEC_CONFIG_PATH
// Try several common layouts inside the BSEC2 Arduino lib:
#if __has_include(<config/generic_33v_3s_4d/bsec_iaq.txt>)
#define BSEC_CONFIG_IMPLICIT 1
#define BSEC_CONFIG_INCLUDE <config/generic_33v_3s_4d/bsec_iaq.txt>
#elif __has_include(<config/generic_33v_3s_28d/bsec_iaq.txt>)
#define BSEC_CONFIG_IMPLICIT 1
#define BSEC_CONFIG_INCLUDE <config/generic_33v_3s_28d/bsec_iaq.txt>
#elif __has_include(<config/bme688/bme688_iaq_33v_3s_4d/bsec_iaq.txt>)
#define BSEC_CONFIG_IMPLICIT 1
#define BSEC_CONFIG_INCLUDE <config/bme688/bme688_iaq_33v_3s_4d/bsec_iaq.txt>
#elif __has_include(<config/bme680/bme680_iaq_33v_3s_4d/bsec_iaq.txt>)
#define BSEC_CONFIG_IMPLICIT 1
#define BSEC_CONFIG_INCLUDE <config/bme680/bme680_iaq_33v_3s_4d/bsec_iaq.txt>
#endif
#endif

// Helpers to turn a macro path into a string literal for #include
#define STR_(x) #x
#define STR(x) STR_(x)

// The actual blob. Note the newline before #include (must start a line).
static const uint8_t bsec_config[] = {
#ifdef BSEC_CONFIG_IMPLICIT
#include BSEC_CONFIG_INCLUDE
#else
#ifndef BSEC_CONFIG_PATH
#error "No BSEC config blob found. Define -DBSEC_CONFIG_PATH=<full/path/inside/BSEC2/src/>"
#endif
#include STR(BSEC_CONFIG_PATH)
#endif
};
// ================================================

class BME680Sensor : public TelemetrySensor
{
  private:
    Bsec2 bme680;
    float lastGasOhms = NAN;
    ScanI2C::DeviceAddress deviceAddress; // Track port for pin switching

  protected:
    const char *bsecConfigFileName = "/prefs/bsec.dat";
    static uint8_t bsecState[BSEC_MAX_STATE_BLOB_SIZE]; // Shared state buffer (saves per-instance RAM)
    uint8_t accuracy = 0;
    uint16_t stateUpdateCounter = 0;
    static bsecSensor sensorList[9]; // Shared, initialized in .cpp
    static constexpr const char *sensorName = SENSOR_NAME;
    void loadState();
    void updateState();
    void checkStatus(const char *functionName);

  public:
    BME680Sensor();
    virtual int32_t runOnce() override;
    virtual bool getMetrics(meshtastic_Telemetry *measurement) override;
    virtual bool initDevice(TwoWire *bus, ScanI2C::FoundDevice *dev) override;
    float getGasResistanceOhms() const { return lastGasOhms; }
};

#endif