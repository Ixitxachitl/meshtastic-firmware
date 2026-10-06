#pragma once

#include "configuration.h"

#if defined(SENSECAP_INDICATOR) && defined(ARCH_ESP32)

#include <FS.h>
#include <stdint.h>

// The SD card behind the Indicator's RP2040 as an fs::FS, so the web server's SD browser runs on it unchanged. Every
// call is a round trip over the interdevice link: reads and writes move a chunk at a time, writes only ever append, and a
// directory is listed a page at a time. For the web server's task only - it holds that task's link buffers.
class IndicatorWebSD : public fs::FS
{
  public:
    IndicatorWebSD();
    // From the card state the co-processor caches at mount; used is 0 until its free-space scan has run.
    uint64_t totalBytes();
    uint64_t usedBytes();
};

extern IndicatorWebSD indicatorWebSD;

#endif
