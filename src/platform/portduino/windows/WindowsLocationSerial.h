#pragma once
#if defined(ARCH_PORTDUINO) && defined(_WIN32)

#include "HardwareSerial.h"
#include <cstdint>
#include <deque>

namespace arduino
{

// Presents the Windows.Devices.Geolocation.Geolocator WinRT API as a HardwareSerial port,
// synthesizing $GNGGA + $GNRMC NMEA sentences from whatever position Windows Location Services
// hands back, so GPS.cpp can consume it exactly like a real NMEA stream (mirrors GpsdSerial.h's
// approach for the gpsd backend). Raw COM activation only (roapi.h + windows.devices.geolocation.h
// + wrl/client.h) - no C++/WinRT, since C++/WinRT has no MSYS2 MINGW32 (i686) package.
//
// KNOWN LIMITATION: this MinGW-w64 header set implements only the Windows 8.0
// UniversalApiContract v1.0 surface - there is no IGeolocatorStatics/RequestAccessAsync
// available to call. This code cannot programmatically prompt for or request location
// permission. The user must already have Settings > Privacy & security > Location > "Let
// desktop apps access your location" enabled, or every GetGeopositionAsync call will fail
// with an access-denied HRESULT, logged via LOG_WARN.
class WindowsLocationSerial : public HardwareSerial
{
    static constexpr size_t RX_BUF_MAX = 4096;
    static constexpr uint32_t RECONNECT_INTERVAL_MS = 5000; // retry activating the Geolocator
    static constexpr uint32_t POLL_INTERVAL_MS = 3000;      // must stay well under GPS_SOL_EXPIRY_MS (5000)
    static constexpr uint32_t REQUEST_TIMEOUT_MS = 10000;   // GetGeopositionAsyncWithAgeAndTimeout timeout

    struct Impl; // hides all WinRT/COM types from HardwareSerial.h's Arduino-namespace consumers
    Impl *_impl;

    std::deque<uint8_t> _rxBuf;
    uint32_t _lastActivateAttemptMs = 0;
    uint32_t _lastRequestMs = 0;
    bool _activated = false;

    bool activateGeolocator(); // RoInitialize (once) + RoActivateInstance + QueryInterface
    void pump();                // drive the request/poll state machine, called from available()
    void startRequest();
    void pollPendingRequest(); // on success, formats NMEA sentences directly into _rxBuf

  public:
    WindowsLocationSerial();
    ~WindowsLocationSerial();

    void begin(unsigned long baud) override { begin(baud, 0); }
    void begin(unsigned long baud, uint16_t config) override;
    void end() override;

    int available() override;
    int peek() override;
    int read() override;
    void flush() override {}
    size_t write(uint8_t) override { return 1; } // Windows Location controls the "hardware"
    using Print::write;
    operator bool() override { return _activated; }
};

extern WindowsLocationSerial windowsLocationSerial;

} // namespace arduino

#endif // ARCH_PORTDUINO && _WIN32
