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
    // frequency_hz: tone frequency in Hz (e.g., 2000 for 2kHz)
    // duration_ms: beep duration in milliseconds
    void sendBeep(uint16_t frequency_hz, uint16_t duration_ms);
    void stopBeep();

  private:
    pb_byte_t protobuf_buffer[PB_BUFSIZE];
    HardwareSerial *_serial = nullptr;
    bool running = false;
};

extern SensecapIndicator *sensecapIndicator;

#endif
