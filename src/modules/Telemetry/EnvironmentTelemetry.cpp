#include "configuration.h"

#if HAS_TELEMETRY && !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR

#include "../mesh/generated/meshtastic/telemetry.pb.h"
#include "Default.h"
#include "EnvironmentTelemetry.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "PowerFSM.h"
#include "RTC.h"
#include "Router.h"
#include "UnitConversions.h"
#include "buzz.h"
#include "graphics/SharedUIDisplay.h"
#include "graphics/images.h"
#include "main.h"
#include "modules/ExternalNotificationModule.h"
#include "power.h"
#include "sleep.h"
#include "target_specific.h"
#include <OLEDDisplay.h>
#include <math.h>
#include <deque>
#include <map>
#include <unordered_map>
#include <cmath>
#include <algorithm>

OLEDDisplay *display = nullptr;

// Your chosen layout constants
static constexpr int kRulerPadLR       = 4;  // L/R padding (px)
static constexpr int kNeedleH          = 5;  // triangle height below baseline
static constexpr int kNeedleBaseHalf   = 5;  // half base width
static constexpr int kRulerBaselineOfs = 8;  // baseline = y + this
static constexpr int kLabelGap         = -1;  // pixels below triangle base

// IAQ "ruler": |-----▲-----| with danger ticks and filled up-triangle below baseline
static void drawIAQRuler(OLEDDisplay* dpy, int x, int y, int w, int iaqValue, const String& label) {
    // inner rect after padding
    int ix = x + kRulerPadLR;
    int iw = w - (kRulerPadLR * 2);
    if (iw < 12) { ix = x; iw = w; }

    // clamp + layout
    int iaq = iaqValue; if (iaq < 0) iaq = 0; if (iaq > 500) iaq = 500;
    const int baselineY = y + kRulerBaselineOfs;
    const int capHalf   = 3;

    // end caps ‘|’
    dpy->drawLine(ix,          baselineY - capHalf, ix,          baselineY + capHalf);
    dpy->drawLine(ix + iw - 1, baselineY - capHalf, ix + iw - 1, baselineY + capHalf);

    // dashed baseline
    for (int i = ix + 1; i < ix + iw - 1; i += 3) {
        dpy->drawLine(i, baselineY, std::min(i + 1, ix + iw - 2), baselineY);
    }

    // ticks at danger levels: 0,25,50,100,150,200,300,500
    auto valToX = [&](int v) -> int {
        return ix + (int)std::lround((float)v / 500.0f * (iw - 1));
    };
    const int tickShort = 2;  // minor tick half-height
    const int tickLong  = 3;  // major tick half-height

    auto drawTick = [&](int v, bool major) {
        int tx = valToX(v);
        int h  = major ? tickLong : tickShort;
        dpy->drawLine(tx, baselineY - h, tx, baselineY + h);
    };

    // majors: 0,100,150,200,300,500 (category boundaries)
    drawTick(0,   true);
    drawTick(100, true);
    drawTick(150, true);
    drawTick(200, true);
    drawTick(300, true);
    drawTick(500, true);

    // minors: 25, 50 (within "Excellent/Good/Moderate")
    drawTick(25,  false);
    drawTick(50,  false);

    // filled upward triangle BELOW the baseline (tip pokes through)
    const int nx = valToX(iaq);
    for (int dy = 0; dy <= kNeedleH; ++dy) {
        int half = (int)std::lround((float)kNeedleBaseHalf * ((float)dy / (float)kNeedleH));
        int yrow = baselineY + dy - 1;                 // rows below baseline
        dpy->drawLine(nx - half, yrow, nx + half, yrow);
    }

    // centered label snug under the triangle base
    const int lw = dpy->getStringWidth(label);
    const int lx = x + (w - lw) / 2;
    dpy->drawString(lx, baselineY + kNeedleH - 1 + kLabelGap, label);
}

// keep last N samples per source
static constexpr int    kSparkW  = 120;         // pixels wide
static constexpr int    kSparkH  = 10;         // pixels tall
static constexpr size_t kHistLen = size_t(kSparkW); // kept for clarity; equals spark width

// Fixed-capacity ring buffer (oldest→newest iteration)
template <size_t N>
struct RingF {
    uint16_t len  = 0;   // 0..N
    uint16_t head = 0;   // index of oldest element
    float    v[N];
    inline void push(float x) {
        if (std::isnan(x)) return;
        if (len < N) {
            v[(head + len) % N] = x;
            ++len;
        } else {
            v[head] = x;
            head = (head + 1) % N;
        }
    }
    inline float at(size_t i) const { return v[(head + i) % N]; } // 0..len-1
};

template <size_t N>
struct NodeHist {
    RingF<N> temp, hum, press;
};

// One record per node; no per-sample heap churn
static std::unordered_map<uint32_t, NodeHist<kSparkW>> s_hist;
// (Optional) pre-size if you have a typical node count:
// static bool s_histReserved = (s_hist.reserve(64), true);

// Helper: apply both offsets centrally
// Overload for fixed-capacity ring

// Sparkline placement offsets (tweak to taste)
static constexpr int kSparkXOffset = -4;  // shift left
static constexpr int kSparkYOffset =  1;  // shift down

template <size_t N>
static void drawMiniSparkBoxed(OLEDDisplay *dpy, int x, int y, int w, int h,
                               const RingF<N> &hist) {
  const int x0 = x + kSparkXOffset;
  const int y0 = y + kSparkYOffset;

  dpy->drawRect(x0, y0, w, h);
  if (hist.len < 2) return;

  // Tuning knobs
  constexpr float kHeadroomFrac     = 0.10f; // 10% visual headroom
  constexpr float kWinsorLowerFrac  = 0.02f; // clamp bottom 2% (set 0 to disable)
  constexpr float kWinsorUpperFrac  = 0.02f; // clamp top   2% (set 0 to disable)
  constexpr float kSmoothAlpha      = 0.0f;  // 0=no smoothing; try 0.2–0.4 if noisy

  // Geometry
  const int ix = x0 + 1, iy = y0 + 1;
  const int iw = w - 2, ih = h - 2;
  const uint16_t L = hist.len;
  const float step = (L > 1) ? (float(iw) / float(L - 1)) : 0.0f;

  // First pass: raw extrema
  float lo = hist.at(0), hi = hist.at(0);
  for (uint16_t i = 1; i < L; ++i) {
    const float v = hist.at(i);
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  float span0 = hi - lo;
  if (span0 < 1e-6f) { lo -= 0.5f; hi += 0.5f; span0 = hi - lo; }

  // Winsorization bounds
  const float loClamp = lo + span0 * kWinsorLowerFrac;
  const float hiClamp = hi - span0 * kWinsorUpperFrac;

  auto clampTo = [&](float v) {
    if (kWinsorLowerFrac > 0.f && v < loClamp) v = loClamp;
    if (kWinsorUpperFrac > 0.f && v > hiClamp) v = hiClamp;
    return v;
  };

  // Second pass: extrema after clamp (+ optional smoothing)
  float sPrev = clampTo(hist.at(0));
  float lo2 = sPrev, hi2 = sPrev;
  for (uint16_t i = 1; i < L; ++i) {
    float v = clampTo(hist.at(i));
    float s = (kSmoothAlpha > 0.f && kSmoothAlpha < 1.f)
                ? (kSmoothAlpha * v + (1.0f - kSmoothAlpha) * sPrev)
                : v;
    if (s < lo2) lo2 = s;
    if (s > hi2) hi2 = s;
    sPrev = s;
  }

  // Headroom + final span
  float loS = lo2, hiS = hi2;
  float span = hiS - loS;
  if (span < 1e-6f) { loS -= 0.5f; hiS += 0.5f; span = hiS - loS; }
  const float pad = span * kHeadroomFrac;
  loS -= pad; hiS += pad; span = hiS - loS;

  auto valToY = [&](float v) -> int {
    const float t = (v - loS) / span;
    int yy = iy + ih - int(std::lround(t * ih));
    if (yy < iy) yy = iy;
    if (yy > iy + ih) yy = iy + ih;
    return yy;
  };

  // Draw polyline (start from real first sample)
  sPrev = clampTo(hist.at(0));
  int xPrev = ix;
  int yPrev = valToY(sPrev);

  for (uint16_t i = 1; i < L; ++i) {
    const int xCur = ix + int(std::lround(step * i));
    float v = clampTo(hist.at(i));
    float s = (kSmoothAlpha > 0.f && kSmoothAlpha < 1.f)
                ? (kSmoothAlpha * v + (1.0f - kSmoothAlpha) * sPrev)
                : v;
    const int yCur = valToY(s);
    dpy->drawLine(xPrev, yPrev, xCur, yCur);
    xPrev = xCur;
    yPrev = yCur;
    sPrev = s;
  }
}

// Prefer long_name when available (and width allows), else short_name, else hex id.
// Mirrors the selection logic used by MessageRenderer.
static inline const char *getSenderLongName(const meshtastic_MeshPacket &mp) {
    static char buf[64];

    const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(mp.from);
    if (node && node->has_user) {
        const char *ln = node->user.long_name;
        const char *sn = node->user.short_name;

    #if defined(M5STACK_UNITC6L)
        // On this target, MessageRenderer prefers short_name.
        if (sn && sn[0]) return sn;
        if (ln && ln[0]) return ln;
    #else
        // On wider screens, prefer long_name; otherwise short_name.
        if (SCREEN_WIDTH >= 200 && ln && ln[0]) return ln;
        if (sn && sn[0]) return sn;
        if (ln && ln[0]) return ln; // last resort if short_name empty
    #endif
    }

    // Fallback: hex node id like "ABCDEF12"
    snprintf(buf, sizeof(buf), "%08x", (unsigned int)mp.from);
    return buf;
}

// Magnus-Tetens over water (good for typical indoor temps)
static float dewPointC(float tempC, float rhPercent) {
  if (!(rhPercent > 0.0f && rhPercent <= 100.0f)) return NAN;
  const float a = 17.62f;
  const float b = 243.12f; // °C
  const float gamma = (a * tempC) / (b + tempC) + logf(rhPercent / 100.0f);
  return (b * gamma) / (a - gamma);
}

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR_EXTERNAL

// Sensors
#include "Sensor/CGRadSensSensor.h"
#include "Sensor/RCWL9620Sensor.h"
#include "Sensor/nullSensor.h"

EnvironmentTelemetryModule* environmentTelemetryModule = nullptr;

namespace graphics
{
extern void drawCommonHeader(OLEDDisplay *display, int16_t x, int16_t y, const char *titleStr, bool force_no_invert,
                             bool show_date);
}
#if __has_include(<Adafruit_AHTX0.h>)
#include "Sensor/AHT10.h"
#endif

#if __has_include(<Adafruit_BME280.h>)
#include "Sensor/BME280Sensor.h"
#endif

#if __has_include(<Adafruit_BMP085.h>)
#include "Sensor/BMP085Sensor.h"
#endif

#if __has_include(<Adafruit_BMP280.h>)
#include "Sensor/BMP280Sensor.h"
#endif

#if __has_include(<Adafruit_LTR390.h>)
#include "Sensor/LTR390UVSensor.h"
#endif

#if __has_include(<bsec2.h>)
#include "Sensor/BME680Sensor.h"
#endif

#if __has_include(<Adafruit_DPS310.h>)
#include "Sensor/DPS310Sensor.h"
#endif

#if __has_include(<Adafruit_MCP9808.h>)
#include "Sensor/MCP9808Sensor.h"
#endif

#if __has_include(<Adafruit_SHT31.h>)
#include "Sensor/SHT31Sensor.h"
#endif

#if __has_include(<Adafruit_LPS2X.h>)
#include "Sensor/LPS22HBSensor.h"
#endif

#if __has_include(<Adafruit_SHTC3.h>)
#include "Sensor/SHTC3Sensor.h"
#endif

#if __has_include("RAK12035_SoilMoisture.h") && defined(RAK_4631) && RAK_4631 == 1
#include "Sensor/RAK12035Sensor.h"
#endif

#if __has_include(<Adafruit_VEML7700.h>)
#include "Sensor/VEML7700Sensor.h"
#endif

#if __has_include(<Adafruit_TSL2591.h>)
#include "Sensor/TSL2591Sensor.h"
#endif

#if __has_include(<ClosedCube_OPT3001.h>)
#include "Sensor/OPT3001Sensor.h"
#endif

#if __has_include(<Adafruit_SHT4x.h>)
#include "Sensor/SHT4XSensor.h"
#endif

#if __has_include(<SparkFun_MLX90632_Arduino_Library.h>)
#include "Sensor/MLX90632Sensor.h"
#endif

#if __has_include(<DFRobot_LarkWeatherStation.h>)
#include "Sensor/DFRobotLarkSensor.h"
#endif

#if __has_include(<DFRobot_RainfallSensor.h>)
#include "Sensor/DFRobotGravitySensor.h"
#endif

#if __has_include(<SparkFun_Qwiic_Scale_NAU7802_Arduino_Library.h>)
#include "Sensor/NAU7802Sensor.h"
#endif

#if __has_include(<Adafruit_BMP3XX.h>)
#include "Sensor/BMP3XXSensor.h"
#endif

#if __has_include(<Adafruit_PCT2075.h>)
#include "Sensor/PCT2075Sensor.h"
#endif

#endif
#ifdef T1000X_SENSOR_EN
#include "Sensor/T1000xSensor.h"
#endif

#ifdef SENSECAP_INDICATOR
#include "Sensor/IndicatorSensor.h"
#endif

#if __has_include(<Adafruit_TSL2561_U.h>)
#include "Sensor/TSL2561Sensor.h"
#endif

#define FAILED_STATE_SENSOR_READ_MULTIPLIER 10
#define DISPLAY_RECEIVEID_MEASUREMENTS_ON_SCREEN true

#include "graphics/ScreenFonts.h"
#include <Throttle.h>

#include <forward_list>

static std::forward_list<TelemetrySensor *> sensors;

template <typename T> void addSensor(ScanI2C *i2cScanner, ScanI2C::DeviceType type)
{
    ScanI2C::FoundDevice dev = i2cScanner->find(type);
    if (dev.type != ScanI2C::DeviceType::NONE || type == ScanI2C::DeviceType::NONE) {
        TelemetrySensor *sensor = new T();
#if WIRE_INTERFACES_COUNT > 1
        TwoWire *bus = ScanI2CTwoWire::fetchI2CBus(dev.address);
        if (dev.address.port != ScanI2C::I2CPort::WIRE1 && sensor->onlyWire1()) {
            // This sensor only works on Wire (Wire1 is not supported)
            delete sensor;
            return;
        }
#else
        TwoWire *bus = &Wire;
#endif
        if (sensor->initDevice(bus, &dev)) {
            sensors.push_front(sensor);
            return;
        }
        // destroy sensor
        delete sensor;
    }
}

void EnvironmentTelemetryModule::i2cScanFinished(ScanI2C *i2cScanner)
{
    if (!moduleConfig.telemetry.environment_measurement_enabled && !ENVIRONMENTAL_TELEMETRY_MODULE_ENABLE) {
        return;
    }
    LOG_INFO("Environment Telemetry adding I2C devices...");

    // order by priority of metrics/values (low top, high bottom)

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR
#ifdef T1000X_SENSOR_EN
    // Not a real I2C device
    addSensor<T1000xSensor>(i2cScanner, ScanI2C::DeviceType::NONE);
#else
#ifdef SENSECAP_INDICATOR
    // Not a real I2C device, uses UART
    addSensor<IndicatorSensor>(i2cScanner, ScanI2C::DeviceType::NONE);
#endif
    addSensor<RCWL9620Sensor>(i2cScanner, ScanI2C::DeviceType::RCWL9620);
    addSensor<CGRadSensSensor>(i2cScanner, ScanI2C::DeviceType::CGRADSENS);
#endif
#endif

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR && !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR_EXTERNAL
#if __has_include(<DFRobot_LarkWeatherStation.h>)
    addSensor<DFRobotLarkSensor>(i2cScanner, ScanI2C::DeviceType::DFROBOT_LARK);
#endif
#if __has_include(<DFRobot_RainfallSensor.h>)
    addSensor<DFRobotGravitySensor>(i2cScanner, ScanI2C::DeviceType::DFROBOT_RAIN);
#endif
#if __has_include(<Adafruit_AHTX0.h>)
    addSensor<AHT10Sensor>(i2cScanner, ScanI2C::DeviceType::AHT10);
#endif
#if __has_include(<Adafruit_BMP085.h>)
    addSensor<BMP085Sensor>(i2cScanner, ScanI2C::DeviceType::BMP_085);
#endif
#if __has_include(<Adafruit_BME280.h>)
    addSensor<BME280Sensor>(i2cScanner, ScanI2C::DeviceType::BME_280);
#endif
#if __has_include(<Adafruit_LTR390.h>)
    addSensor<LTR390UVSensor>(i2cScanner, ScanI2C::DeviceType::LTR390UV);
#endif
#if __has_include(<bsec2.h>)
    addSensor<BME680Sensor>(i2cScanner, ScanI2C::DeviceType::BME_680);
#endif
#if __has_include(<Adafruit_BMP280.h>)
    addSensor<BMP280Sensor>(i2cScanner, ScanI2C::DeviceType::BMP_280);
#endif
#if __has_include(<Adafruit_DPS310.h>)
    addSensor<DPS310Sensor>(i2cScanner, ScanI2C::DeviceType::DPS310);
#endif
#if __has_include(<Adafruit_MCP9808.h>)
    addSensor<MCP9808Sensor>(i2cScanner, ScanI2C::DeviceType::MCP9808);
#endif
#if __has_include(<Adafruit_SHT31.h>)
    addSensor<SHT31Sensor>(i2cScanner, ScanI2C::DeviceType::SHT31);
#endif
#if __has_include(<Adafruit_LPS2X.h>)
    addSensor<LPS22HBSensor>(i2cScanner, ScanI2C::DeviceType::LPS22HB);
#endif
#if __has_include(<Adafruit_SHTC3.h>)
    addSensor<SHTC3Sensor>(i2cScanner, ScanI2C::DeviceType::SHTC3);
#endif
#if __has_include("RAK12035_SoilMoisture.h") && defined(RAK_4631) && RAK_4631 == 1
    addSensor<RAK12035Sensor>(i2cScanner, ScanI2C::DeviceType::RAK12035);
#endif
#if __has_include(<Adafruit_VEML7700.h>)
    addSensor<VEML7700Sensor>(i2cScanner, ScanI2C::DeviceType::VEML7700);
#endif
#if __has_include(<Adafruit_TSL2591.h>)
    addSensor<TSL2591Sensor>(i2cScanner, ScanI2C::DeviceType::TSL2591);
#endif
#if __has_include(<ClosedCube_OPT3001.h>)
    addSensor<OPT3001Sensor>(i2cScanner, ScanI2C::DeviceType::OPT3001);
#endif
#if __has_include(<Adafruit_SHT4x.h>)
    addSensor<SHT4XSensor>(i2cScanner, ScanI2C::DeviceType::SHT4X);
#endif
#if __has_include(<SparkFun_MLX90632_Arduino_Library.h>)
    addSensor<MLX90632Sensor>(i2cScanner, ScanI2C::DeviceType::MLX90632);
#endif

#if __has_include(<Adafruit_BMP3XX.h>)
    addSensor<BMP3XXSensor>(i2cScanner, ScanI2C::DeviceType::BMP_3XX);
#endif
#if __has_include(<Adafruit_PCT2075.h>)
    addSensor<PCT2075Sensor>(i2cScanner, ScanI2C::DeviceType::PCT2075);
#endif
#if __has_include(<Adafruit_TSL2561_U.h>)
    addSensor<TSL2561Sensor>(i2cScanner, ScanI2C::DeviceType::TSL2561);
#endif
#if __has_include(<SparkFun_Qwiic_Scale_NAU7802_Arduino_Library.h>)
    addSensor<NAU7802Sensor>(i2cScanner, ScanI2C::DeviceType::NAU7802);
#endif

#endif
}

int32_t EnvironmentTelemetryModule::runOnce()
{
    if (sleepOnNextExecution == true) {
        sleepOnNextExecution = false;
        uint32_t nightyNightMs = Default::getConfiguredOrDefaultMs(moduleConfig.telemetry.environment_update_interval,
                                                                   default_telemetry_broadcast_interval_secs);
        LOG_DEBUG("Sleep for %ims, then awake to send metrics again", nightyNightMs);
        doDeepSleep(nightyNightMs, true, false);
    }

    uint32_t result = UINT32_MAX;
    /*
        Uncomment the preferences below if you want to use the module
        without having to configure it from the PythonAPI or WebUI.
    */

    // moduleConfig.telemetry.environment_measurement_enabled = 1;
    // moduleConfig.telemetry.environment_screen_enabled = 1;
    // moduleConfig.telemetry.environment_update_interval = 15;

    if (!(moduleConfig.telemetry.environment_measurement_enabled || moduleConfig.telemetry.environment_screen_enabled ||
          ENVIRONMENTAL_TELEMETRY_MODULE_ENABLE)) {
        // If this module is not enabled, and the user doesn't want the display screen don't waste any OSThread time on it
        return disable();
    }

    if (firstTime) {
        // This is the first time the OSThread library has called this function, so do some setup
        firstTime = 0;

        if (moduleConfig.telemetry.environment_measurement_enabled || ENVIRONMENTAL_TELEMETRY_MODULE_ENABLE) {
            LOG_INFO("Environment Telemetry: init");

            // check if we have at least one sensor
            if (!sensors.empty()) {
                result = DEFAULT_SENSOR_MINIMUM_WAIT_TIME_BETWEEN_READS;
            }

#ifdef T1000X_SENSOR_EN
#elif !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR_EXTERNAL
            if (ina219Sensor.hasSensor())
                result = ina219Sensor.runOnce();
            if (ina260Sensor.hasSensor())
                result = ina260Sensor.runOnce();
            if (ina3221Sensor.hasSensor())
                result = ina3221Sensor.runOnce();
            if (max17048Sensor.hasSensor())
                result = max17048Sensor.runOnce();
                // this only works on the wismesh hub with the solar option. This is not an I2C sensor, so we don't need the
                // sensormap here.
#ifdef HAS_RAKPROT
            result = rak9154Sensor.runOnce();
#endif
#endif
        }
        // it's possible to have this module enabled, only for displaying values on the screen.
        // therefore, we should only enable the sensor loop if measurement is also enabled
        return result == UINT32_MAX ? disable() : setStartDelay();
    } else {
        // if we somehow got to a second run of this module with measurement disabled, then just wait forever
        if (!moduleConfig.telemetry.environment_measurement_enabled && !ENVIRONMENTAL_TELEMETRY_MODULE_ENABLE) {
            return disable();
        }

        for (TelemetrySensor *sensor : sensors) {
            uint32_t delay = sensor->runOnce();
            if (delay < result) {
                result = delay;
            }
        }

        if (((lastSentToMesh == 0) ||
             !Throttle::isWithinTimespanMs(lastSentToMesh, Default::getConfiguredOrDefaultMsScaled(
                                                               moduleConfig.telemetry.environment_update_interval,
                                                               default_telemetry_broadcast_interval_secs, numOnlineNodes))) &&
            airTime->isTxAllowedChannelUtil(config.device.role != meshtastic_Config_DeviceConfig_Role_SENSOR) &&
            airTime->isTxAllowedAirUtil()) {
            sendTelemetry();
            lastSentToMesh = millis();
        } else if (((lastSentToPhone == 0) || !Throttle::isWithinTimespanMs(lastSentToPhone, sendToPhoneIntervalMs)) &&
                   (service->isToPhoneQueueEmpty())) {
            // Just send to phone when it's not our time to send to mesh yet
            // Only send while queue is empty (phone assumed connected)
            sendTelemetry(NODENUM_BROADCAST, true);
            lastSentToPhone = millis();
        }
    }
    return min(sendToPhoneIntervalMs, result);
}

bool EnvironmentTelemetryModule::wantUIFrame()
{
    return moduleConfig.telemetry.environment_screen_enabled;
}

#if HAS_SCREEN

std::vector<uint32_t> EnvironmentTelemetryModule::getSourcesWithTelemetry() const {
    std::vector<uint32_t> out;
    out.reserve(lastBySource.size());
    for (const auto &kv : lastBySource) out.push_back(kv.first);
    std::sort(out.begin(), out.end()); // nice to have
    return out;
}

void EnvironmentTelemetryModule::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    ::display = display;
    // === Setup display ===
    display->clear();
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    int line = 1;

    // === Set Title
    const char *titleStr = (graphics::isHighResolution) ? "Environment" : "Env.";

    // === Header ===
    graphics::drawCommonHeader(display, x, y, titleStr);

    // === Row spacing setup ===
    const int rowHeight = FONT_HEIGHT_SMALL - 4;
    int currentY = graphics::getTextPositions(display)[line++];

    // === Show "No Telemetry" if no data available ===
    if (!lastMeasurementPacket) {
        display->drawString(x, currentY, "No Telemetry");
        return;
    }

    const meshtastic_MeshPacket* packetToShow = nullptr;

    if (selectedSource != 0) {
        auto it = lastBySource.find(selectedSource);
        if (it != lastBySource.end()) packetToShow = it->second;
    } else {
        packetToShow = lastMeasurementPacket;
    }

    if (!packetToShow) {
        display->drawString(x, currentY, "No Telemetry");
        return;
    }

    // Decode the telemetry message from the latest received packet
    const meshtastic_Data &p = packetToShow->decoded;
    meshtastic_Telemetry telemetry;
    if (!pb_decode_from_bytes(p.payload.bytes, p.payload.size, &meshtastic_Telemetry_msg, &telemetry)) {
        display->drawString(x, currentY, "No Telemetry");
        return;
    }

    const auto &m = telemetry.variant.environment_metrics;

    // Check if any telemetry field has valid data
    bool hasAny = m.has_temperature || m.has_relative_humidity || m.barometric_pressure != 0 || m.iaq != 0 ||
                  m.has_gas_resistance || m.gas_resistance != 0 ||
                  m.voltage != 0 || m.current != 0 || m.lux != 0 || m.white_lux != 0 || m.weight != 0 ||
                  m.distance != 0 || m.radiation != 0;

    if (!hasAny) {
        display->drawString(x, currentY, "No Telemetry");
        return;
    }

    // === First line: Show sender LONG name + time since received (left)
    const char *sender = getSenderLongName(*packetToShow);  // long name per your request
    uint32_t agoSecs = service->GetTimeSinceMeshPacket(packetToShow);
    String agoStr = (agoSecs > 864000) ? "?"
                    : (agoSecs > 3600) ? String(agoSecs / 3600) + "h"
                    : (agoSecs > 60)   ? String(agoSecs / 60) + "m"
                                       : String(agoSecs) + "s";
    String leftStr = String(sender) + " (" + agoStr + ")";
    display->drawString(x, currentY, leftStr);
    currentY += rowHeight;

    // look up per-source history for sparklines (fixed-capacity rings)
    uint32_t from = packetToShow->from;
    auto &nh = s_hist[from]; // default-constructs empty rings for new nodes

    // Where to draw all graphs (aligned)
    const int graphX = SCREEN_WIDTH - (kSparkW + 2);

    // === Temperature row (Tmp) ===
    if (m.has_temperature) {
        String tStr = moduleConfig.telemetry.environment_display_fahrenheit
                      ? String(UnitConversions::CelsiusToFahrenheit(m.temperature), 1) + "°F"
                      : String(m.temperature, 1) + "°C";
        display->drawString(x, currentY, "Tmp: " + tStr);  // abbreviation kept from original code :contentReference[oaicite:1]{index=1}
        // Box height = roughly rowHeight - 2 so it sits nicely on the text baseline
        drawMiniSparkBoxed(display, graphX, currentY, kSparkW, rowHeight - 2, nh.temp);
        currentY += rowHeight;
    }

    // === Humidity row (Hum) ===
    if (m.has_relative_humidity && m.relative_humidity > 0.0f) {
        String hStr = String(m.relative_humidity, 0) + "%";
        display->drawString(x, currentY, "Hum: " + hStr);  // abbreviation kept from original code :contentReference[oaicite:2]{index=2}
        drawMiniSparkBoxed(display, graphX, currentY, kSparkW, rowHeight - 2, nh.hum);
        currentY += rowHeight;
    }

    // === Pressure row (Prss) ===
    if (m.barometric_pressure != 0) {
        String pStr = String(m.barometric_pressure, 0) + " hPa";
        display->drawString(x, currentY, "Prss: " + pStr); // abbreviation kept from original code :contentReference[oaicite:3]{index=3}
        drawMiniSparkBoxed(display, graphX, currentY, kSparkW, rowHeight - 2, nh.press);
        currentY += rowHeight;
    }

    // === Collect the REST of sensor readings as before (no icons) ===
    std::vector<String> entries;

    // Dew (same as your original, just push as its own entry)
    if (m.has_temperature && m.has_relative_humidity && m.relative_humidity > 0.0f) {
      const float dpC = dewPointC(m.temperature, m.relative_humidity); // existing helper
      if (!isnan(dpC)) {
        if (moduleConfig.telemetry.environment_display_fahrenheit) {
          const float dpF = dpC * 9.0f / 5.0f + 32.0f;
          entries.push_back("Dew: " + String(dpF, 1) + " °F");
        } else {
          entries.push_back("Dew: " + String(dpC, 1) + " °C");
        }
      }
    }

    // Gas (separate entry, as before)
    // Heuristic: ignore obviously bogus readings
    constexpr float kMinGasKOhm = 0.5f;
    constexpr float kMaxGasKOhm = 1000.0f;
    if ((m.has_gas_resistance || m.gas_resistance != 0) &&
        (m.gas_resistance >= kMinGasKOhm && m.gas_resistance <= kMaxGasKOhm)) {
      entries.push_back("Gas: " + String(m.gas_resistance, 2) + " kOhm");
    }

    // keep your remaining entries untouched
    if (m.voltage != 0 || m.current != 0)
        entries.push_back(String(m.voltage, 1) + "V / " + String(m.current, 0) + "mA");
    if (m.lux != 0)         entries.push_back("Light: " + String(m.lux, 0) + "lx");
    if (m.white_lux != 0)   entries.push_back("White: " + String(m.white_lux, 0) + "lx");
    if (m.weight != 0)      entries.push_back("Weight: " + String(m.weight, 0) + "kg");
    if (m.distance != 0)    entries.push_back("Level: " + String(m.distance, 0) + "mm");
    if (m.radiation != 0)   entries.push_back("Rad: " + String(m.radiation, 2) + " µR/h");

    // Give the right column more space by shifting the split left.
    // Positive value => wider right column.
    static constexpr int kSplitShift = 14; // tweak to taste
    const int splitX   = (SCREEN_WIDTH / 2) - kSplitShift;
    const int leftMaxW = splitX - x - 2;

    // === Draw remaining entries in 2-column format (left and right) ===
    for (size_t i = 0; i < entries.size(); i += 2) {
        // Left column
        display->drawString(x, currentY, entries[i]);

        // Right column starts at the new split, so it’s wider
        if (i + 1 < entries.size()) {
            display->drawString(splitX, currentY, entries[i + 1]);
        }

        // keep your existing spacing (no entrySpacing change)
        currentY += rowHeight;
    }

    // === IAQ gauge (0..500) instead of inline text ===
    if (m.iaq != 0) {
        // Build the same label text you used before (keeps your categories)
        String aqi = "IAQ: " + String(m.iaq);
        const char *bannerMsg = nullptr;
        if (m.iaq <= 25) aqi += " (Excellent)";
        else if (m.iaq <= 50) aqi += " (Good)";
        else if (m.iaq <= 100) aqi += " (Moderate)";
        else if (m.iaq <= 150) aqi += " (Poor)";
        else if (m.iaq <= 200) { aqi += " (Unhealthy)";      bannerMsg = "Unhealthy IAQ"; }
        else if (m.iaq <= 300) { aqi += " (Very Unhealthy)"; bannerMsg = "Very Unhealthy IAQ"; }
        else                   { aqi += " (Hazardous)";      bannerMsg = "Hazardous IAQ"; }

        // Full-row gauge at the bottom (monochrome)
        const int gMargin = 2;
        const int gX = x;
        const int gW = SCREEN_WIDTH - gX - gMargin;
        const int gH = rowHeight + 6;

        // If there's not enough vertical room, nudge it up so it fits
        if (currentY + gH > SCREEN_HEIGHT) {
            currentY = SCREEN_HEIGHT - gH;
            if (currentY < 0) currentY = 0;
        }

        drawIAQRuler(display, gX, currentY, gW, (int)m.iaq, aqi);
        currentY += gH;

        // Keep your existing IAQ alert logic (banner/beep) exactly as before:
        static uint32_t lastAlertTime = 0;
        static bool inBanner = false;
        uint32_t now = millis();
        bool isCooldownOver = (now - lastAlertTime > 60000);
        bool isOwnTelemetry = lastMeasurementPacket->from == nodeDB->getNodeNum();
        if (!inBanner && isOwnTelemetry && bannerMsg && isCooldownOver) {
            inBanner = true; lastAlertTime = now;
            screen->showSimpleBanner(bannerMsg, 3000);
            if (m.iaq > 200 && moduleConfig.external_notification.enabled && !externalNotificationModule->getMute())
                playLongBeep();
            inBanner = false;
        }
    }
}
#endif

bool EnvironmentTelemetryModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_Telemetry *t)
{
    if (t->which_variant == meshtastic_Telemetry_environment_metrics_tag) {
#if defined(DEBUG_PORT) && !defined(DEBUG_MUTE)
        const char *sender = getSenderShortName(mp);

        LOG_INFO("(Received from %s): barometric_pressure=%f, current=%f, gas_resistance=%f, relative_humidity=%f, "
                 "temperature=%f",
                 sender, t->variant.environment_metrics.barometric_pressure, t->variant.environment_metrics.current,
                 t->variant.environment_metrics.gas_resistance, t->variant.environment_metrics.relative_humidity,
                 t->variant.environment_metrics.temperature);
        LOG_INFO("(Received from %s): voltage=%f, IAQ=%d, distance=%f, lux=%f, white_lux=%f", sender,
                 t->variant.environment_metrics.voltage, t->variant.environment_metrics.iaq,
                 t->variant.environment_metrics.distance, t->variant.environment_metrics.lux,
                 t->variant.environment_metrics.white_lux);

        LOG_INFO("(Received from %s): wind speed=%fm/s, direction=%d degrees, weight=%fkg", sender,
                 t->variant.environment_metrics.wind_speed, t->variant.environment_metrics.wind_direction,
                 t->variant.environment_metrics.weight);

        LOG_INFO("(Received from %s): radiation=%fµR/h", sender, t->variant.environment_metrics.radiation);

#endif
        // release previous packet before occupying a new spot
        if (lastMeasurementPacket != nullptr)
            packetPool.release(lastMeasurementPacket);

        lastMeasurementPacket = packetPool.allocCopy(mp);
        
                // NEW: track per-source
        uint32_t from = mp.from;            // (sender nodenum)
        auto it = lastBySource.find(from);
        if (it != lastBySource.end() && it->second) {
            packetPool.release(it->second);
        }
        lastBySource[from] = packetPool.allocCopy(mp);
        // record per-source history for tiny graphs
        const auto &em = t->variant.environment_metrics;
        auto &nh2 = s_hist[from];
        if (em.has_temperature)       nh2.temp.push(em.temperature);
        if (em.has_relative_humidity) nh2.hum.push(em.relative_humidity);
        if (em.barometric_pressure)   nh2.press.push(em.barometric_pressure);
    }

    return false; // Let others look at this message also if they want
}

bool EnvironmentTelemetryModule::getEnvironmentTelemetry(meshtastic_Telemetry *m)
{
    bool valid = true;
    bool hasSensor = false;
    m->time = getTime();
    m->which_variant = meshtastic_Telemetry_environment_metrics_tag;
    m->variant.environment_metrics = meshtastic_EnvironmentMetrics_init_zero;

    for (TelemetrySensor *sensor : sensors) {
        valid = valid && sensor->getMetrics(m);
        hasSensor = true;
    }

#ifndef T1000X_SENSOR_EN
    if (ina219Sensor.hasSensor()) {
        valid = valid && ina219Sensor.getMetrics(m);
        hasSensor = true;
    }
    if (ina260Sensor.hasSensor()) {
        valid = valid && ina260Sensor.getMetrics(m);
        hasSensor = true;
    }
    if (ina3221Sensor.hasSensor()) {
        valid = valid && ina3221Sensor.getMetrics(m);
        hasSensor = true;
    }
    if (max17048Sensor.hasSensor()) {
        valid = valid && max17048Sensor.getMetrics(m);
        hasSensor = true;
    }
#endif
#ifdef HAS_RAKPROT
    valid = valid && rak9154Sensor.getMetrics(m);
    hasSensor = true;
#endif
    return valid && hasSensor;
}

meshtastic_MeshPacket *EnvironmentTelemetryModule::allocReply()
{
    if (currentRequest) {
        auto req = *currentRequest;
        const auto &p = req.decoded;
        meshtastic_Telemetry scratch;
        meshtastic_Telemetry *decoded = NULL;
        memset(&scratch, 0, sizeof(scratch));
        if (pb_decode_from_bytes(p.payload.bytes, p.payload.size, &meshtastic_Telemetry_msg, &scratch)) {
            decoded = &scratch;
        } else {
            LOG_ERROR("Error decoding EnvironmentTelemetry module!");
            return NULL;
        }
        // Check for a request for environment metrics
        if (decoded->which_variant == meshtastic_Telemetry_environment_metrics_tag) {
            meshtastic_Telemetry m = meshtastic_Telemetry_init_zero;
            if (getEnvironmentTelemetry(&m)) {
                LOG_INFO("Environment telemetry reply to request");
                return allocDataProtobuf(m);
            } else {
                return NULL;
            }
        }
    }
    return NULL;
}

bool EnvironmentTelemetryModule::sendTelemetry(NodeNum dest, bool phoneOnly)
{
    meshtastic_Telemetry m = meshtastic_Telemetry_init_zero;
    m.which_variant = meshtastic_Telemetry_environment_metrics_tag;
    m.time = getTime();

    if (getEnvironmentTelemetry(&m)) {
        LOG_INFO("Send: barometric_pressure=%f, current=%f, gas_resistance=%f, relative_humidity=%f, temperature=%f",
                 m.variant.environment_metrics.barometric_pressure, m.variant.environment_metrics.current,
                 m.variant.environment_metrics.gas_resistance, m.variant.environment_metrics.relative_humidity,
                 m.variant.environment_metrics.temperature);
        LOG_INFO("Send: voltage=%f, IAQ=%d, distance=%f, lux=%f", m.variant.environment_metrics.voltage,
                 m.variant.environment_metrics.iaq, m.variant.environment_metrics.distance, m.variant.environment_metrics.lux);

        LOG_INFO("Send: wind speed=%fm/s, direction=%d degrees, weight=%fkg", m.variant.environment_metrics.wind_speed,
                 m.variant.environment_metrics.wind_direction, m.variant.environment_metrics.weight);

        LOG_INFO("Send: radiation=%fµR/h", m.variant.environment_metrics.radiation);

        LOG_INFO("Send: soil_temperature=%f, soil_moisture=%u", m.variant.environment_metrics.soil_temperature,
                 m.variant.environment_metrics.soil_moisture);

        sensor_read_error_count = 0;

        meshtastic_MeshPacket *p = allocDataProtobuf(m);
        p->to = dest;
        p->decoded.want_response = false;
        if (config.device.role == meshtastic_Config_DeviceConfig_Role_SENSOR)
            p->priority = meshtastic_MeshPacket_Priority_RELIABLE;
        else
            p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
        // release previous packet before occupying a new spot
        if (lastMeasurementPacket != nullptr)
            packetPool.release(lastMeasurementPacket);

        lastMeasurementPacket = packetPool.allocCopy(*p);
        uint32_t self = nodeDB->getNodeNum();
        auto it = lastBySource.find(self);
        if (it != lastBySource.end() && it->second) {
            packetPool.release(it->second);
        }
        lastBySource[self] = packetPool.allocCopy(*p);
        const auto &em = m.variant.environment_metrics;
        auto &selfHist = s_hist[self];
        if (em.has_temperature)       selfHist.temp.push(em.temperature);
        if (em.has_relative_humidity) selfHist.hum.push(em.relative_humidity);
        if (em.barometric_pressure)   selfHist.press.push(em.barometric_pressure);
        
        if (phoneOnly) {
            LOG_INFO("Send packet to phone");
            service->sendToPhone(p);
        } else {
            LOG_INFO("Send packet to mesh");
            service->sendToMesh(p, RX_SRC_LOCAL, true);

            if (config.device.role == meshtastic_Config_DeviceConfig_Role_SENSOR && config.power.is_power_saving) {
                meshtastic_ClientNotification *notification = clientNotificationPool.allocZeroed();
                notification->level = meshtastic_LogRecord_Level_INFO;
                notification->time = getValidTime(RTCQualityFromNet);
                sprintf(notification->message, "Sending telemetry and sleeping for %us interval in a moment",
                        Default::getConfiguredOrDefaultMs(moduleConfig.telemetry.environment_update_interval,
                                                          default_telemetry_broadcast_interval_secs) /
                            1000U);
                service->sendClientNotification(notification);
                sleepOnNextExecution = true;
                LOG_DEBUG("Start next execution in 5s, then sleep");
                setIntervalFromNow(FIVE_SECONDS_MS);
            }
        }
        return true;
    }
    return false;
}

AdminMessageHandleResult EnvironmentTelemetryModule::handleAdminMessageForModule(const meshtastic_MeshPacket &mp,
                                                                                 meshtastic_AdminMessage *request,
                                                                                 meshtastic_AdminMessage *response)
{
    AdminMessageHandleResult result = AdminMessageHandleResult::NOT_HANDLED;
#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR_EXTERNAL

    for (TelemetrySensor *sensor : sensors) {
        result = sensor->handleAdminMessage(mp, request, response);
        if (result != AdminMessageHandleResult::NOT_HANDLED)
            return result;
    }

    if (ina219Sensor.hasSensor()) {
        result = ina219Sensor.handleAdminMessage(mp, request, response);
        if (result != AdminMessageHandleResult::NOT_HANDLED)
            return result;
    }
    if (ina260Sensor.hasSensor()) {
        result = ina260Sensor.handleAdminMessage(mp, request, response);
        if (result != AdminMessageHandleResult::NOT_HANDLED)
            return result;
    }
    if (ina3221Sensor.hasSensor()) {
        result = ina3221Sensor.handleAdminMessage(mp, request, response);
        if (result != AdminMessageHandleResult::NOT_HANDLED)
            return result;
    }
    if (max17048Sensor.hasSensor()) {
        result = max17048Sensor.handleAdminMessage(mp, request, response);
        if (result != AdminMessageHandleResult::NOT_HANDLED)
            return result;
    }
#endif
    return result;
}

#endif
