#if defined(ARCH_PORTDUINO) && defined(_WIN32)

#include "WindowsLocationCOM.h"

// See the header comment: this TU is deliberately kept free of Arduino/HardwareSerial.h, so it's
// safe to widen the repo-wide NOUSER/NOGDI/WIN32_LEAN_AND_MEAN flags back out just for this file -
// roapi.h's transitive <ole2.h>/<oleidl.h> need winuser.h's LPMSG, and NOGDI-gated GDI types are
// referenced by windows.foundation.h's Point/Rect declarations. Same pattern as
// WindowsConsole.cpp. Order matters: undef before the first windows.h-family include below.
#undef WIN32_LEAN_AND_MEAN
#undef NOUSER
#undef NOGDI

#include <cstdio>
#include <mutex>

#include <roapi.h>
#include <winstring.h> // WindowsCreateString/WindowsDeleteString prototypes
#include <wrl/client.h>

#include <windows.devices.geolocation.h>

using Microsoft::WRL::ComPtr;
using namespace ABI::Windows::Devices::Geolocation;
using namespace ABI::Windows::Foundation;

namespace
{
// FILETIME epoch (1601-01-01) -> Unix epoch (1970-01-01), in 100ns ticks.
constexpr int64_t kFileTimeToUnixEpochTicks = 116444736000000000LL;

void initRoOnce()
{
    static std::once_flag flag;
    static HRESULT initResult = S_OK;
    std::call_once(flag, [] { initResult = RoInitialize(RO_INIT_MULTITHREADED); });
    (void)initResult; // RPC_E_CHANGED_MODE is benign (something else already init'd COM here); caller doesn't need it
}

const char *hresultError(const char *what, HRESULT hr)
{
    static char buf[128];
    snprintf(buf, sizeof(buf), "%s failed, hr=0x%08lX", what, (unsigned long)hr);
    return buf;
}
} // namespace

struct WindowsLocationCOM::Impl {
    ComPtr<IGeolocator> geolocator;
    ComPtr<IAsyncOperation<Geoposition *>> pendingOp;
    ComPtr<IAsyncInfo> pendingInfo;
    bool requestInFlight = false;
    bool locationDisabled = false;
};

WindowsLocationCOM::WindowsLocationCOM() : _impl(new Impl()) {}

WindowsLocationCOM::~WindowsLocationCOM()
{
    delete _impl;
}

bool WindowsLocationCOM::activate(const char **errorOut)
{
    initRoOnce();

    HSTRING classId = nullptr;
    HRESULT hr = WindowsCreateString(RuntimeClass_Windows_Devices_Geolocation_Geolocator,
                                      (UINT32)wcslen(RuntimeClass_Windows_Devices_Geolocation_Geolocator), &classId);
    if (FAILED(hr)) {
        if (errorOut)
            *errorOut = hresultError("WindowsCreateString", hr);
        return false;
    }

    ComPtr<IInspectable> inspectable;
    hr = RoActivateInstance(classId, &inspectable);
    WindowsDeleteString(classId);
    if (FAILED(hr)) {
        // This is a WinRT runtime class (not a registry-registered COM class), so a failure
        // here usually means Windows.Devices.Geolocation isn't present on this Windows
        // build/SKU, not a registration problem.
        if (errorOut)
            *errorOut = hresultError("RoActivateInstance(Geolocator)", hr);
        return false;
    }

    hr = inspectable.As(&_impl->geolocator);
    if (FAILED(hr)) {
        if (errorOut)
            *errorOut = hresultError("QueryInterface(IGeolocator)", hr);
        return false;
    }

    _impl->geolocator->put_DesiredAccuracy(PositionAccuracy_High);

    // NOTE (real-world caveat, not something this code can fix): this MinGW header surface has
    // no IGeolocatorStatics/RequestAccessAsync, so we cannot programmatically request location
    // permission - see the class comment in WindowsLocationCOM.h.
    PositionStatus status;
    _impl->locationDisabled = SUCCEEDED(_impl->geolocator->get_LocationStatus(&status)) && status == PositionStatus_Disabled;

    return true;
}

bool WindowsLocationCOM::isLocationDisabled() const
{
    return _impl->locationDisabled;
}

void WindowsLocationCOM::requestFix(uint32_t timeoutMs)
{
    if (_impl->requestInFlight)
        return;

    TimeSpan maxAge{0};
    TimeSpan timeout{(INT64)timeoutMs * 10000LL}; // ms -> 100ns ticks

    HRESULT hr = _impl->geolocator->GetGeopositionAsyncWithAgeAndTimeout(maxAge, timeout, &_impl->pendingOp);
    if (FAILED(hr))
        return;
    if (FAILED(_impl->pendingOp.As(&_impl->pendingInfo))) {
        _impl->pendingOp.Reset();
        return;
    }
    _impl->requestInFlight = true;
}

WindowsLocationCOM::PollResult WindowsLocationCOM::poll(Fix &out, const char **errorOut)
{
    if (!_impl->requestInFlight)
        return PollResult::NotActivated;

    AsyncStatus status;
    if (FAILED(_impl->pendingInfo->get_Status(&status)))
        return PollResult::Pending;

    if (status == Started)
        return PollResult::Pending;

    PollResult result = PollResult::Failed;

    if (status == Completed) {
        ComPtr<IGeoposition> geoposition;
        if (SUCCEEDED(_impl->pendingOp->GetResults(&geoposition)) && geoposition) {
            ComPtr<IGeocoordinate> coord;
            if (SUCCEEDED(geoposition->get_Coordinate(&coord))) {
                ComPtr<IReference<DOUBLE>> altRef;
                DateTime ts{};
                coord->get_Latitude(&out.latitude);
                coord->get_Longitude(&out.longitude);
                coord->get_Accuracy(&out.accuracyMeters);
                coord->get_Altitude(&altRef);
                coord->get_Timestamp(&ts);
                out.altitudeMeters = 0;
                if (altRef)
                    altRef->get_Value(&out.altitudeMeters);
                out.unixTimestamp = (ts.UniversalTime - kFileTimeToUnixEpochTicks) / 10000000LL;
                result = PollResult::GotFix;
            }
        }
    } else if (status == Error) {
        HRESULT errHr = S_OK;
        _impl->pendingInfo->get_ErrorCode(&errHr);
        // 0x80004004 (E_ABORT) here commonly means location access is off in Settings, or the
        // desktop-apps location toggle is disabled.
        if (errorOut)
            *errorOut = hresultError("GetGeopositionAsync", errHr);
    }
    // Canceled: nothing to report, just fall through and reset below.

    _impl->pendingInfo->Close();
    _impl->pendingOp.Reset();
    _impl->pendingInfo.Reset();
    _impl->requestInFlight = false;
    return result;
}

#endif // ARCH_PORTDUINO && _WIN32
