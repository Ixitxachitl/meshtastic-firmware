#ifdef SENSECAP_INDICATOR

#pragma once

#include "concurrency/OSThread.h"
#include "configuration.h"
#include "mesh/generated/meshtastic/interdevice.pb.h"
#include <HardwareSerial.h>
#include <pb_decode.h>
#include <pb_encode.h>

#define PB_BUFSIZE 1024

// Magic bytes for protobuf framing: 0x94 0xc3
#define PROTOBUF_MAGIC_BYTE1 0x94
#define PROTOBUF_MAGIC_BYTE2 0xc3

// BME688 sensor data storage (from RP2040)
// Format: "BME688,temp,humidity,pressure,iaq,gas,co2eq,voceq,accuracy"
struct BME688Data {
    float temperature = 0.0f;    // °C
    float humidity = 0.0f;       // %
    float pressure = 0.0f;       // hPa
    float iaq = 0.0f;            // Indoor Air Quality index (0-500)
    float gas_resistance = 0.0f; // kOhms
    float co2_equivalent = 0.0f; // ppm
    float voc_equivalent = 0.0f; // ppm
    uint8_t accuracy = 0;        // IAQ accuracy (0-3)
    bool has_data = false;
    uint32_t last_update = 0; // millis() timestamp
};

class SensecapIndicator : public concurrency::OSThread
{
  public:
    SensecapIndicator(HardwareSerial &serial);
    int32_t runOnce() override;
    bool send_uplink(meshtastic_InterdeviceMessage message);

    // Send beep command to RP2040 buzzer
    // For packed format: bit 31=queue flag, bits 30-16=frequency, bits 15-0=duration
    void sendBeep(uint32_t packed_value);
    // Legacy method for manual beeps (non-queued)
    void sendBeep(uint16_t frequency_hz, uint16_t duration_ms);
    void stopBeep();

    // Get BME688 sensor data from RP2040
    BME688Data getBME688Data() const { return bme688_data; }
    bool hasBME688Data() const { return bme688_data.has_data; }

  private:
    pb_byte_t protobuf_buffer[PB_BUFSIZE];
    HardwareSerial *_serial = nullptr;
    bool running = false;
    BME688Data bme688_data;
};

extern SensecapIndicator *sensecapIndicator;

#endif
