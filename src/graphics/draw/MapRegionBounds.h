#pragma once

// Rough extents of each LoRa region, so the Map frame can show where the radio is set for before any position exists.
// Mainland only: remote islands and the high Arctic would push Web Mercator out to a view of the whole world.

#include "meshtastic/config.pb.h"
#include <cmath>

namespace graphics
{
namespace MapRegionBounds
{

struct Bounds {
    float south, west, north, east; // degrees; west < east, so no box crosses the antimeridian
};

struct View {
    float centerLat, centerLng;
    int zoom;
};

// False for UNSET, or a code added to the protobufs without an entry here.
inline bool regionBounds(meshtastic_Config_LoRaConfig_RegionCode region, Bounds &out)
{
    // Places several codes share, named once so their copies can't drift apart.
    static constexpr Bounds kEurope{34.5f, -25.0f, 71.5f, 40.0f};
    static constexpr Bounds kAustraliaNz{-47.3f, 112.9f, -10.0f, 178.6f};
    static constexpr Bounds kUkraine{44.3f, 22.1f, 52.4f, 40.3f};
    static constexpr Bounds kMalaysia{0.8f, 99.6f, 7.4f, 119.3f};
    static constexpr Bounds kPhilippines{4.6f, 116.9f, 21.2f, 126.6f};
    static constexpr Bounds kKazakhstan{40.5f, 46.5f, 55.5f, 87.4f};
    static constexpr Bounds kItu1{-35.0f, -25.5f, 72.0f, 180.0f};  // Europe, Africa, Middle East, Russia
    static constexpr Bounds kItu2{-56.0f, -168.0f, 72.0f, -34.5f}; // the Americas
    static constexpr Bounds kItu3{-47.5f, 44.0f, 54.0f, 180.0f};   // Asia-Pacific east of Iran

    switch (region) {
    case meshtastic_Config_LoRaConfig_RegionCode_US:
        out = {24.5f, -125.0f, 49.5f, -66.9f}; // contiguous states
        return true;
    case meshtastic_Config_LoRaConfig_RegionCode_EU_433:
    case meshtastic_Config_LoRaConfig_RegionCode_EU_868:
    case meshtastic_Config_LoRaConfig_RegionCode_EU_866:
    case meshtastic_Config_LoRaConfig_RegionCode_EU_874:
    case meshtastic_Config_LoRaConfig_RegionCode_EU_917:
    case meshtastic_Config_LoRaConfig_RegionCode_EU_N_868:
        out = kEurope;
        return true;
    case meshtastic_Config_LoRaConfig_RegionCode_CN:
        out = {18.0f, 73.5f, 53.6f, 134.8f};
        return true;
    case meshtastic_Config_LoRaConfig_RegionCode_JP:
        out = {24.0f, 122.9f, 45.6f, 146.0f};
        return true;
    case meshtastic_Config_LoRaConfig_RegionCode_ANZ:
    case meshtastic_Config_LoRaConfig_RegionCode_ANZ_433:
        out = kAustraliaNz;
        return true;
    case meshtastic_Config_LoRaConfig_RegionCode_KR:
        out = {33.0f, 124.5f, 38.7f, 131.0f};
        return true;
    case meshtastic_Config_LoRaConfig_RegionCode_TW:
        out = {21.8f, 118.1f, 26.4f, 122.1f};
        return true;
    case meshtastic_Config_LoRaConfig_RegionCode_RU:
        out = {41.2f, 19.6f, 73.0f, 180.0f};
        return true;
    case meshtastic_Config_LoRaConfig_RegionCode_IN:
        out = {6.7f, 68.1f, 35.7f, 97.4f};
        return true;
    case meshtastic_Config_LoRaConfig_RegionCode_NZ_865:
        out = {-47.3f, 166.4f, -34.4f, 178.6f};
        return true;
    case meshtastic_Config_LoRaConfig_RegionCode_TH:
        out = {5.6f, 97.3f, 20.5f, 105.7f};
        return true;
    case meshtastic_Config_LoRaConfig_RegionCode_LORA_24: // 2.4 GHz is licence-free worldwide
        out = {-56.0f, -180.0f, 72.0f, 180.0f};
        return true;
    case meshtastic_Config_LoRaConfig_RegionCode_UA_433:
    case meshtastic_Config_LoRaConfig_RegionCode_UA_868:
        out = kUkraine;
        return true;
    case meshtastic_Config_LoRaConfig_RegionCode_MY_433:
    case meshtastic_Config_LoRaConfig_RegionCode_MY_919:
        out = kMalaysia;
        return true;
    case meshtastic_Config_LoRaConfig_RegionCode_SG_923:
        out = {1.15f, 103.6f, 1.48f, 104.1f};
        return true;
    case meshtastic_Config_LoRaConfig_RegionCode_PH_433:
    case meshtastic_Config_LoRaConfig_RegionCode_PH_868:
    case meshtastic_Config_LoRaConfig_RegionCode_PH_915:
        out = kPhilippines;
        return true;
    case meshtastic_Config_LoRaConfig_RegionCode_KZ_433:
    case meshtastic_Config_LoRaConfig_RegionCode_KZ_863:
        out = kKazakhstan;
        return true;
    case meshtastic_Config_LoRaConfig_RegionCode_NP_865:
        out = {26.3f, 80.0f, 30.5f, 88.2f};
        return true;
    case meshtastic_Config_LoRaConfig_RegionCode_BR_902:
        out = {-33.8f, -74.0f, 5.3f, -34.8f};
        return true;
    case meshtastic_Config_LoRaConfig_RegionCode_ITU1_2M:
    case meshtastic_Config_LoRaConfig_RegionCode_ITU1_70CM:
        out = kItu1;
        return true;
    case meshtastic_Config_LoRaConfig_RegionCode_ITU2_2M:
    case meshtastic_Config_LoRaConfig_RegionCode_ITU2_70CM:
    case meshtastic_Config_LoRaConfig_RegionCode_ITU2_125CM:
        out = kItu2;
        return true;
    case meshtastic_Config_LoRaConfig_RegionCode_ITU3_2M:
    case meshtastic_Config_LoRaConfig_RegionCode_ITU3_70CM:
        out = kItu3;
        return true;
    default:
        return false;
    }
}

constexpr double kPi = 3.14159265358979323846;
constexpr double kMaxMercatorLat = 85.05112878; // where Web Mercator's square world ends
constexpr double kFitFraction = 0.9;            // margin, matching the node auto-fit

// Web Mercator Y as a fraction of the world's height, 0 at the top.
inline double mercatorY(double latDeg)
{
    latDeg = std::fmax(-kMaxMercatorLat, std::fmin(kMaxMercatorLat, latDeg));
    const double s = std::sin(latDeg * kPi / 180.0);
    return 0.5 - std::log((1.0 + s) / (1.0 - s)) / (4.0 * kPi);
}

inline double latFromMercatorY(double y)
{
    return std::atan(std::sinh(kPi * (1.0 - 2.0 * y))) * 180.0 / kPi;
}

// Highest zoom at which the whole box fits the viewport, centred on its visual middle (not the degree midpoint,
// which sits low on screen for a high-latitude box). 256 px tiles, the same projection MapRenderer draws with.
inline View fit(const Bounds &b, int viewWidth, int viewHeight, int minZoom, int maxZoom)
{
    const double top = mercatorY(b.north);
    const double bottom = mercatorY(b.south);
    const double spanX = (b.east - b.west) / 360.0;
    const double spanY = bottom - top;

    View v{(float)latFromMercatorY((top + bottom) / 2.0), (b.west + b.east) / 2.0f, minZoom};
    for (int z = maxZoom; z > minZoom; z--) {
        const double worldPx = 256.0 * (double)(1u << z);
        if (spanX * worldPx <= viewWidth * kFitFraction && spanY * worldPx <= viewHeight * kFitFraction) {
            v.zoom = z;
            break;
        }
    }
    return v;
}

} // namespace MapRegionBounds
} // namespace graphics
