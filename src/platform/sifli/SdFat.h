#pragma once

// Enough of SdFat's surface for device-ui's SD tile service header to parse.
//
// That service is only built with HAS_SDCARD, which this board does not set:
// its card shares the SPI bus with the radio, so it needs the shared-bus
// service instead. The header is still included unconditionally, hence a stub
// rather than the real library.

#include "FS.h"

class FsFile
{
  public:
    explicit operator bool() const { return false; }
    void close() {}
};

class SdFat
{
  public:
    bool begin(...) { return false; }
};
