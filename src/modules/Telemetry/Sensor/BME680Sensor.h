#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR && (__has_include(<bsec2.h>) || __has_include(<Adafruit_BME680.h>))

#include "../mesh/generated/meshtastic/telemetry.pb.h"
#include "TelemetrySensor.h"

#if __has_include(<bsec2.h>)
#include <bme68xLibrary.h>
#include <bsec2.h>
#else
#include <Adafruit_BME680.h>
#include <memory>
#endif

#define STATE_SAVE_PERIOD UINT32_C(360 * 60 * 1000) // That's 6 hours worth of millis()

#if __has_include(<bsec2.h>)
const uint8_t bsec_config[] = {
#include "config/bme680/bme680_iaq_33v_3s_4d/bsec_iaq.txt"
};
#endif
class BME680Sensor : public TelemetrySensor
{
  private:
#if __has_include(<bsec2.h>)
    Bsec2 bme680;
    float lastGasOhms = NAN;
    ScanI2C::DeviceAddress deviceAddress; // Track port for pin switching
#else
    using BME680Ptr = std::unique_ptr<Adafruit_BME680>;

    static BME680Ptr makeBME680(TwoWire *bus) { return std::make_unique<Adafruit_BME680>(bus); }

    BME680Ptr bme680;
#endif

  protected:
#if __has_include(<bsec2.h>)
    const char *bsecConfigFileName = "/prefs/bsec.dat";
    static uint8_t bsecState[BSEC_MAX_STATE_BLOB_SIZE]; // Shared state buffer (saves per-instance RAM)
    uint8_t accuracy = 0;
    uint16_t stateUpdateCounter = 0;
    static bsecSensor sensorList[9]; // Shared, initialized in .cpp
    void loadState();
    void updateState();
    void checkStatus(const char *functionName);
#endif

  public:
    BME680Sensor();
#if __has_include(<bsec2.h>)
    virtual int32_t runOnce() override;
#endif
    virtual bool getMetrics(meshtastic_Telemetry *measurement) override;
    virtual bool initDevice(TwoWire *bus, ScanI2C::FoundDevice *dev) override;
    float getGasResistanceOhms() const { return lastGasOhms; }
};

#endif