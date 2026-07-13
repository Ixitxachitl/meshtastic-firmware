// The ESP32-S3 framework-arduinoespressif32-libs/esp32s3/lib/libfreertos.a is compiled without
// configUSE_TRACE_FACILITY, so xEventGroupSetBitsFromISR is absent from that archive.
// Adafruit NeoPixel (via esp32-hal-rmt) needs it at link time.  Provide the standard FreeRTOS
// implementation here so the linker is satisfied.

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

extern "C" BaseType_t xEventGroupSetBitsFromISR(EventGroupHandle_t xEventGroup, const EventBits_t uxBitsToSet,
                                                BaseType_t *pxHigherPriorityTaskWoken)
{
    return xTimerPendFunctionCallFromISR(vEventGroupSetBitsCallback, (void *)xEventGroup, (uint32_t)uxBitsToSet,
                                         pxHigherPriorityTaskWoken);
}
