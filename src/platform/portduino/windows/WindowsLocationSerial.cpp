#if defined(ARCH_PORTDUINO) && defined(_WIN32)

#include "WindowsLocationSerial.h"
#include "WindowsLocationCOM.h"
#include "configuration.h"
#include "gps/GeoCoord.h"
#include "gps/NMEAWPL.h" // printGGA()
#include "mesh/generated/meshtastic/mesh.pb.h"

#include <cstring>
#include <ctime>

namespace
{
uint32_t nmeaClampLocal(uint32_t len, size_t bufsz)
{
    if (len >= bufsz)
        return bufsz > 0 ? (uint32_t)(bufsz - 1) : 0;
    return len;
}

uint32_t nmeaChecksumLocal(const char *buf)
{
    uint32_t chk = 0;
    const char *c = strchr(buf, '$');
    if (c)
        for (c++; *c && *c != '*'; c++)
            chk ^= (uint8_t)*c;
    return chk;
}

// Windows Location doesn't expose HDOP/PDOP, but GPS.cpp rejects any fix with a zero HDOP
// (see GPS.cpp's "BOGUS hdop.value() REJECTED" check), so synthesize a plausible non-zero
// value from the reported horizontal accuracy (meters): smaller accuracy -> smaller "HDOP".
// This is cosmetic only - TinyGPS++ just needs the field non-zero to accept the fix.
uint32_t synthesizeHdop(double accuracyMeters)
{
    double hdop = accuracyMeters > 0 ? accuracyMeters / 10.0 : 1.0;
    if (hdop < 0.5)
        hdop = 0.5;
    if (hdop > 99.0)
        hdop = 99.0;
    return (uint32_t)(hdop * 100); // matches the *100-scaled convention GPS.cpp reads back
}

// Hand-written because NMEAWPL.cpp's nmeaClamp()/nmeaChecksum() are file-local statics and
// there is no existing printRMC() anywhere in this codebase. TinyGPS++'s date/time fields are
// only populated by RMC (never GGA), and GPS::hasLock()'s fix-acceptance gate requires both to
// be fresh - so this sentence is what actually keeps a synthesized fix "locked".
//
//        1         2 3       4 5        6 7   8   9      10 11 12
//        |         | |       | |        | |   |   |      |  |  |
// $--RMC,hhmmss.ss,A,llll.ll,a,yyyyy.yy,a,x.x,x.x,ddmmyy,,,A*hh<CR><LF>
uint32_t printRMC(char *buf, size_t bufsz, const meshtastic_Position &pos)
{
    if (bufsz == 0)
        return 0;

    GeoCoord geoCoord(pos.latitude_i, pos.longitude_i, pos.altitude);
    time_t timestamp = pos.timestamp;
    struct tm *t = gmtime(&timestamp);

    uint32_t len = snprintf(
        buf, bufsz, "$GNRMC,%02d%02d%02d.%02d,A,%02d%07.4f,%c,%03d%07.4f,%c,,,%02d%02d%02d,,,A", t->tm_hour, t->tm_min,
        t->tm_sec, pos.timestamp_millis_adjust, geoCoord.getDMSLatDeg(),
        (abs(geoCoord.getLatitude()) - geoCoord.getDMSLatDeg() * 1e+7) * 6e-6, geoCoord.getDMSLatCP(), geoCoord.getDMSLonDeg(),
        (abs(geoCoord.getLongitude()) - geoCoord.getDMSLonDeg() * 1e+7) * 6e-6, geoCoord.getDMSLonCP(), t->tm_mday,
        t->tm_mon + 1, t->tm_year % 100);

    len = nmeaClampLocal(len, bufsz);
    uint32_t chk = nmeaChecksumLocal(buf);
    len = nmeaClampLocal(len + snprintf(buf + len, bufsz - len, "*%02X\r\n", chk), bufsz);
    return len;
}
} // namespace

namespace arduino
{

struct WindowsLocationSerial::Impl {
    WindowsLocationCOM com;
};

WindowsLocationSerial windowsLocationSerial;

WindowsLocationSerial::WindowsLocationSerial() : _impl(new Impl()) {}

WindowsLocationSerial::~WindowsLocationSerial()
{
    delete _impl;
}

bool WindowsLocationSerial::activateGeolocator()
{
    const char *error = nullptr;
    if (!_impl->com.activate(&error)) {
        LOG_WARN("WindowsLocationSerial: %s", error ? error : "activation failed");
        return false;
    }

    // NOTE (real-world caveat, not something this code can fix): this MinGW header surface has
    // no IGeolocatorStatics/RequestAccessAsync, so we cannot programmatically request location
    // permission. If the user hasn't enabled Settings > Privacy & security > Location > "Let
    // desktop apps access your location", every request will fail - logged, not silently
    // swallowed.
    if (_impl->com.isLocationDisabled()) {
        LOG_WARN("WindowsLocationSerial: Windows Location is disabled for this app - enable "
                 "Settings > Privacy & security > Location > \"Let desktop apps access your location\"");
    }

    LOG_INFO("WindowsLocationSerial: Geolocator activated");
    return true;
}

void WindowsLocationSerial::begin(unsigned long /*baud*/, uint16_t /*config*/)
{
    end();
    _lastActivateAttemptMs = 0; // force immediate activation attempt
}

void WindowsLocationSerial::end()
{
    _activated = false;
    _rxBuf.clear();
}

void WindowsLocationSerial::startRequest()
{
    _impl->com.requestFix(REQUEST_TIMEOUT_MS);
    _lastRequestMs = millis();
}

void WindowsLocationSerial::pollPendingRequest()
{
    WindowsLocationCOM::Fix fix;
    const char *error = nullptr;
    WindowsLocationCOM::PollResult result = _impl->com.poll(fix, &error);

    if (result == WindowsLocationCOM::PollResult::NotActivated) {
        // Nothing in flight - start a new request once the poll interval has elapsed.
        if (millis() - _lastRequestMs >= POLL_INTERVAL_MS)
            startRequest();
        return;
    }

    if (result == WindowsLocationCOM::PollResult::Pending)
        return; // still in flight, check again next tick

    if (result == WindowsLocationCOM::PollResult::GotFix) {
        meshtastic_Position pos = meshtastic_Position_init_zero;
        pos.latitude_i = (int32_t)(fix.latitude * 1e7);
        pos.longitude_i = (int32_t)(fix.longitude * 1e7);
        pos.altitude = (int32_t)fix.altitudeMeters;
        pos.timestamp = (uint32_t)fix.unixTimestamp;
        pos.timestamp_millis_adjust = 0;
        pos.fix_quality = 1;  // GPS::hasLock() needs 1..5
        pos.sats_in_view = 0; // not exposed by Windows Location
        pos.HDOP = synthesizeHdop(fix.accuracyMeters);
        pos.altitude_geoidal_separation = 0;

        char gga[96];
        char rmc[96];
        uint32_t ggaLen = printGGA(gga, sizeof(gga), pos);
        uint32_t rmcLen = printRMC(rmc, sizeof(rmc), pos);

        size_t space = RX_BUF_MAX > _rxBuf.size() ? RX_BUF_MAX - _rxBuf.size() : 0;
        size_t total = ggaLen + rmcLen;
        if (total > space) // drop oldest bytes rather than silently truncating the new sentence
            _rxBuf.erase(_rxBuf.begin(), _rxBuf.begin() + std::min(_rxBuf.size(), total - space));
        for (uint32_t i = 0; i < ggaLen; i++)
            _rxBuf.push_back((uint8_t)gga[i]);
        for (uint32_t i = 0; i < rmcLen; i++)
            _rxBuf.push_back((uint8_t)rmc[i]);
    } else if (result == WindowsLocationCOM::PollResult::Failed) {
        LOG_WARN("WindowsLocationSerial: %s", error ? error : "position request failed");
    }
}

void WindowsLocationSerial::pump()
{
    if (!_activated) {
        uint32_t now = millis();
        if (now - _lastActivateAttemptMs < RECONNECT_INTERVAL_MS)
            return;
        _lastActivateAttemptMs = now;
        _activated = activateGeolocator();
        return;
    }

    pollPendingRequest();
}

int WindowsLocationSerial::available()
{
    pump();
    return (int)_rxBuf.size();
}

int WindowsLocationSerial::peek()
{
    return _rxBuf.empty() ? -1 : (int)_rxBuf.front();
}

int WindowsLocationSerial::read()
{
    if (_rxBuf.empty())
        return -1;
    uint8_t c = _rxBuf.front();
    _rxBuf.pop_front();
    return (int)c;
}

} // namespace arduino

#endif // ARCH_PORTDUINO && _WIN32
