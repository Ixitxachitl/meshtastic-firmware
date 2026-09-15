#pragma once

// Whether the Map frame's view has changed enough to write to uiconfig.map_data.home, which device-ui shares.
// Dependency-free so the policy is unit-tested natively; MapRenderer owns the state and decides when to ask.

#include <cstdint>

namespace graphics
{
namespace MapViewPersistence
{

constexpr float kLocationMinMoveMeters = 100.0f; // GPS jitter below this never rewrites flash

struct Inputs {
    bool zoomPending;       // the user changed zoom since the last write
    bool haveLivePosition;  // localPosition holds a fix
    bool haveSavedLocation; // uiconfig already holds a non-zero home
    float metersFromSaved;  // live to saved; read only when both exist
    bool locationAllowed;   // false under lockdown, since uiconfig is the one file stored unencrypted
};

struct Decision {
    bool write;         // save uiconfig now
    bool writeLocation; // copy the live position into home
    bool writeZoom;     // copy the current zoom into home
};

inline Decision decide(const Inputs &in)
{
    const bool liveUsable = in.haveLivePosition && in.locationAllowed;
    const bool moved = liveUsable && (!in.haveSavedLocation || in.metersFromSaved >= kLocationMinMoveMeters);

    // home is one message: a zoom with no location would hand device-ui a view centred on 0,0.
    const bool zoomDue = in.zoomPending && (in.haveSavedLocation || liveUsable);

    Decision d{};
    d.writeLocation = moved || (zoomDue && liveUsable); // already writing, so refresh it for free
    // A first-ever home needs some zoom, or device-ui opens it at world view.
    d.writeZoom = zoomDue || (d.writeLocation && !in.haveSavedLocation);
    d.write = d.writeLocation || d.writeZoom;
    return d;
}

} // namespace MapViewPersistence
} // namespace graphics
