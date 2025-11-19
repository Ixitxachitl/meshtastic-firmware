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

  private:
    pb_byte_t protobuf_buffer[PB_BUFSIZE];
    HardwareSerial *_serial = nullptr;
    bool running = false;
};

extern SensecapIndicator *sensecapIndicator;

#endif
