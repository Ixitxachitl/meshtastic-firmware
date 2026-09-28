#pragma once

#include "configuration.h"

#if BASEUI_WIFI_MANAGER

#include "mesh/generated/meshtastic/known_wifi.pb.h"
#include <stdint.h>

// Known WiFi networks, scanning, and switching between them. The network in use is still config.network's
// wifi_ssid/wifi_psk - this keeps the others, and changes which one that is. Main loop only.
namespace WiFiNetworks
{

constexpr int kMaxKnown = 8; // known_wifi.options

int knownCount();
const meshtastic_KnownWifiNetwork &known(int index);
// The saved password for a network, or null if it isn't known.
const char *knownPsk(const char *ssid);
void forget(const char *ssid);

// Makes this the network in use and saves it to config. True if WiFi is already running and will switch to it now;
// false if WiFi was never started this boot (it had no network then), so it is joined after a reboot.
bool join(const char *ssid, const char *psk);

struct ScanResult {
    char ssid[33];
    int8_t rssi;
    bool secured;
};
enum class ScanState : uint8_t { Idle, Running, Done, Failed };

// False if WiFi is off in config or a scan is already running.
bool startScan();
ScanState scanState();
// Strongest first, each network once. Valid while the state is Done.
int scanCount();
const ScanResult &scanResult(int index);
void clearScan();

// From the WiFi reconnect loop. Connected, it remembers the network in use; out of reach for a while, it looks for
// another known network in range and switches to the strongest.
void tick(bool connected);

} // namespace WiFiNetworks

#endif
