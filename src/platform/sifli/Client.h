#pragma once

// Arduino's network client interface. Nothing on this board has a network to
// speak to - there is no WiFi or Ethernet on the SF32LB52x - but device-ui's
// Ethernet client compiles against the type, so the abstract base has to exist.

#include "Arduino.h"
#include "IPAddress.h"

class Client : public Stream
{
  public:
    virtual int connect(IPAddress ip, uint16_t port) = 0;
    virtual int connect(const char *host, uint16_t port) = 0;
    virtual size_t write(uint8_t) override = 0;
    virtual size_t write(const uint8_t *buf, size_t size) override = 0;
    virtual int available() override = 0;
    virtual int read() override = 0;
    virtual int read(uint8_t *buf, size_t size) = 0;
    virtual int peek() override = 0;
    virtual void flush() override = 0;
    virtual void stop() = 0;
    virtual uint8_t connected() = 0;
    virtual operator bool() = 0;
};
