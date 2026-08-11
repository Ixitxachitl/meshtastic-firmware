#pragma once
#if defined(ARCH_PORTDUINO) && defined(_WIN32)

#include <cstdint>

// Isolated from Arduino/HardwareSerial.h on purpose - same reasoning as WindowsConsole.cpp's
// "isolated TU" comment. roapi.h/windows.devices.geolocation.h pull in enough of <windows.h>
// (via ole2.h/oleidl.h, which need winuser.h's LPMSG) that the repo-wide NOUSER/NOGDI flags
// break them, and rpcndr.h's `typedef unsigned char boolean` collides with Arduino's
// `typedef bool boolean` the same way windows.h itself would. Keeping this TU entirely free of
// Arduino headers means WindowsLocationCOM.cpp can safely #undef NOUSER/NOGDI for itself with
// no risk of the INPUT/boolean collisions those flags exist to avoid elsewhere. Only POD types
// cross this boundary; WindowsLocationSerial.cpp (the HardwareSerial-facing side) talks to this
// class exclusively through this header.
class WindowsLocationCOM
{
  public:
    enum class PollResult { Pending, GotFix, Failed, NotActivated };

    struct Fix {
        double latitude = 0;
        double longitude = 0;
        double altitudeMeters = 0;
        double accuracyMeters = 0;
        int64_t unixTimestamp = 0; // seconds since Unix epoch, UTC
    };

    WindowsLocationCOM();
    ~WindowsLocationCOM();

    // Activates the Geolocator. On failure, *errorOut points to a short static string
    // describing which COM call failed (for the caller to LOG_WARN); *errorOut is untouched on
    // success. Safe to call repeatedly (e.g. on a retry cooldown).
    bool activate(const char **errorOut);

    // True once activate() succeeded and Windows itself reports location access is off
    // (Settings > Privacy & security > Location > "Let desktop apps access your location").
    bool isLocationDisabled() const;

    // Starts an async GetGeopositionAsyncWithAgeAndTimeout call. No-op if one is already pending.
    void requestFix(uint32_t timeoutMs);

    // Non-blocking poll of the pending request, if any. GotFix fills `out`; Failed sets
    // *errorOut to a short static string with the failing HRESULT.
    PollResult poll(Fix &out, const char **errorOut);

  private:
    struct Impl;
    Impl *_impl;
};

#endif // ARCH_PORTDUINO && _WIN32
