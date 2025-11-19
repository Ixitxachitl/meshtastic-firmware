#ifdef SENSECAP_INDICATOR

#pragma once

#include "Arduino.h"
#include "mesh/generated/meshtastic/interdevice.pb.h"

// FakeUART intercepts GPS writes and forwards them to the RP2040 via protobuf
class FakeUART : public Stream
{
  public:
    FakeUART();

    // Stream interface
    int available() override;
    int read() override;
    int peek() override;
    size_t write(uint8_t c) override;
    size_t write(const uint8_t *buffer, size_t size) override;
    size_t write(char *buffer, size_t size);
    void flush() override;

    // HardwareSerial compatibility methods
    void flush(bool txOnly) { flush(); }
    int availableForWrite() override { return 1024; }

    // Handle incoming NMEA from RP2040
    void handleIncomingNMEA(const char *nmea);

  private:
    char rx_buffer[1024];
    size_t rx_head = 0;
    size_t rx_tail = 0;
};

extern FakeUART *fakeUART;

#endif
