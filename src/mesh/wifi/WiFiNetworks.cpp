#include "mesh/wifi/WiFiNetworks.h"

#if BASEUI_WIFI_MANAGER

#include "DebugConfiguration.h"
#include "NodeDB.h"
#include "UptimeClock.h"
#include "mesh/Throttle.h"
#include "mesh/wifi/WiFiAPClient.h"

#include <WiFi.h>
#include <algorithm>
#include <string.h>

namespace WiFiNetworks
{
namespace
{

constexpr const char *kFile = "/prefs/wifi_networks.proto";
constexpr int kMaxScan = 20;
constexpr uint32_t kRoamAfterMs = 45000; // out of reach this long before looking for another known network
constexpr uint32_t kRoamRetryMs = 60000; // and no more often than this

meshtastic_KnownWifiNetworks list = meshtastic_KnownWifiNetworks_init_zero;
bool loaded = false;

ScanResult results[kMaxScan];
int resultCount = 0;
ScanState state = ScanState::Idle;
bool roamScan = false; // this scan is the roaming one, not the menu's

uint32_t disconnectedSinceMs = 0, lastRoamMs = 0;

void ensureLoaded()
{
    if (loaded)
        return;
    loaded = true;
    if (nodeDB->loadProto(kFile, meshtastic_KnownWifiNetworks_size, sizeof(list), &meshtastic_KnownWifiNetworks_msg, &list) !=
        LOAD_SUCCESS)
        list = meshtastic_KnownWifiNetworks_init_zero;
}

void save()
{
    nodeDB->saveProto(kFile, meshtastic_KnownWifiNetworks_size, &meshtastic_KnownWifiNetworks_msg, &list);
}

int indexOf(const char *ssid)
{
    for (int i = 0; i < (int)list.networks_count; i++) {
        if (strcmp(list.networks[i].ssid, ssid) == 0)
            return i;
    }
    return -1;
}

void copyString(char *dst, size_t size, const char *src)
{
    strncpy(dst, src ? src : "", size - 1);
    dst[size - 1] = '\0';
}

// To the front of the list, as the most recently joined; the oldest drops off a full list.
void remember(const char *ssid, const char *psk)
{
    ensureLoaded();
    const int at = indexOf(ssid);
    if (at == 0 && strcmp(list.networks[0].psk, psk) == 0)
        return; // already first, unchanged: nothing to write
    const int last = at >= 0 ? at : std::min<int>(list.networks_count, kMaxKnown - 1);
    for (int i = last; i > 0; i--)
        list.networks[i] = list.networks[i - 1];
    copyString(list.networks[0].ssid, sizeof(list.networks[0].ssid), ssid);
    copyString(list.networks[0].psk, sizeof(list.networks[0].psk), psk);
    if (at < 0 && list.networks_count < kMaxKnown)
        list.networks_count++;
    save();
}

// Collects a finished scan: each name once at its strongest, hidden networks left out, strongest first.
void collect(int found)
{
    resultCount = 0;
    for (int i = 0; i < found; i++) {
        const String ssid = WiFi.SSID(i);
        if (ssid.length() == 0 || ssid.length() > 32)
            continue;
        const int8_t rssi = (int8_t)WiFi.RSSI(i);
        const bool secured = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        int existing = -1;
        for (int j = 0; j < resultCount; j++) {
            if (strcmp(results[j].ssid, ssid.c_str()) == 0)
                existing = j;
        }
        if (existing >= 0) {
            if (rssi > results[existing].rssi)
                results[existing].rssi = rssi;
            continue;
        }
        if (resultCount == kMaxScan)
            continue;
        ScanResult &r = results[resultCount++];
        copyString(r.ssid, sizeof(r.ssid), ssid.c_str());
        r.rssi = rssi;
        r.secured = secured;
    }
    std::sort(results, results + resultCount, [](const ScanResult &a, const ScanResult &b) { return a.rssi > b.rssi; });
    WiFi.scanDelete();
}

} // namespace

int knownCount()
{
    ensureLoaded();
    return list.networks_count;
}

const meshtastic_KnownWifiNetwork &known(int index)
{
    ensureLoaded();
    return list.networks[index];
}

const char *knownPsk(const char *ssid)
{
    ensureLoaded();
    const int at = indexOf(ssid);
    return at >= 0 ? list.networks[at].psk : nullptr;
}

void forget(const char *ssid)
{
    ensureLoaded();
    const int at = indexOf(ssid);
    if (at < 0)
        return;
    for (int i = at; i + 1 < (int)list.networks_count; i++)
        list.networks[i] = list.networks[i + 1];
    list.networks_count--;
    save();
}

bool join(const char *ssid, const char *psk)
{
    copyString(config.network.wifi_ssid, sizeof(config.network.wifi_ssid), ssid);
    copyString(config.network.wifi_psk, sizeof(config.network.wifi_psk), psk);
    config.network.wifi_enabled = true;
    nodeDB->saveToDisk(SEGMENT_CONFIG);
    LOG_INFO("WiFi: switching to %s", ssid);
    disconnectedSinceMs = 0;
    if (!wifiReconnect)
        return false;
    needReconnect = true; // the reconnect loop drops the old network and joins this one from config
    wifiReconnect->setIntervalFromNow(0);
    return true;
}

bool startScan()
{
    if (!config.network.wifi_enabled || state == ScanState::Running)
        return false;
    if (WiFi.getMode() == WIFI_MODE_NULL)
        WiFi.mode(WIFI_STA); // never started this boot, for want of a network: scanning still needs the radio
    resultCount = 0;
    roamScan = false;
    const int16_t started = WiFi.scanNetworks(true, false);
    state = started == WIFI_SCAN_FAILED ? ScanState::Failed : ScanState::Running;
    return state == ScanState::Running;
}

ScanState scanState()
{
    if (state == ScanState::Running && !roamScan) {
        const int16_t found = WiFi.scanComplete();
        if (found >= 0) {
            collect(found);
            state = ScanState::Done;
        } else if (found == WIFI_SCAN_FAILED) {
            state = ScanState::Failed;
        }
    }
    return roamScan ? ScanState::Running : state;
}

int scanCount()
{
    return state == ScanState::Done && !roamScan ? resultCount : 0;
}

const ScanResult &scanResult(int index)
{
    return results[index];
}

void clearScan()
{
    if (state != ScanState::Running)
        state = ScanState::Idle;
}

void tick(bool connected)
{
    if (connected) {
        disconnectedSinceMs = 0;
        if (config.network.wifi_ssid[0])
            remember(config.network.wifi_ssid, config.network.wifi_psk); // no write unless something changed
        return;
    }

    // A roaming scan in flight: once it is in, switch to the strongest known network it saw.
    if (roamScan) {
        const int16_t found = WiFi.scanComplete();
        if (found == WIFI_SCAN_RUNNING)
            return;
        roamScan = false;
        state = ScanState::Idle;
        if (found < 0)
            return;
        collect(found);
        for (int i = 0; i < resultCount; i++) { // strongest first
            const char *psk = knownPsk(results[i].ssid);
            if (!psk)
                continue;
            if (strcmp(results[i].ssid, config.network.wifi_ssid) != 0)
                join(results[i].ssid, psk);
            break; // the current one is the strongest known: keep trying it
        }
        return;
    }

    if (!config.network.wifi_enabled || knownCount() == 0 || state == ScanState::Running)
        return;
    if (disconnectedSinceMs == 0) {
        disconnectedSinceMs = Time::skipZero(millis());
        return;
    }
    if (Throttle::isWithinTimespanMs(disconnectedSinceMs, kRoamAfterMs) ||
        (lastRoamMs && Throttle::isWithinTimespanMs(lastRoamMs, kRoamRetryMs)))
        return;
    lastRoamMs = Time::skipZero(millis());
    if (WiFi.scanNetworks(true, false) != WIFI_SCAN_FAILED) {
        roamScan = true;
        state = ScanState::Running;
    }
}

} // namespace WiFiNetworks

#endif
