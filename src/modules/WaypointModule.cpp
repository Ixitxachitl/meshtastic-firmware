#include "WaypointModule.h"
#include "NodeDB.h"
#include "PowerFSM.h"
#include "configuration.h"
#include "graphics/SharedUIDisplay.h"
#include "graphics/draw/CompassRenderer.h"

#if HAS_SCREEN
#include "gps/RTC.h"
#include "graphics/Screen.h"
#include "graphics/TimeFormatters.h"
#include "graphics/draw/NodeListRenderer.h"
#include "main.h"
#endif

WaypointModule *waypointModule;

static inline float degToRad(float deg)
{
    return deg * PI / 180.0f;
}
static inline float radToDeg(float rad)
{
    return rad * 180.0f / PI;
}

ProcessMessage WaypointModule::handleReceived(const meshtastic_MeshPacket &mp)
{
#if defined(DEBUG_PORT) && !defined(DEBUG_MUTE)
    auto &p = mp.decoded;
    LOG_INFO("Received waypoint msg from=0x%0x, id=0x%x, msg=%.*s", mp.from, mp.id, p.payload.size, p.payload.bytes);
#endif
    // We only store/display messages destined for us.
    // Keep a copy of the most recent text message.
    devicestate.rx_waypoint = mp;
    devicestate.has_rx_waypoint = true;

    powerFSM.trigger(EVENT_RECEIVED_MSG);

#if HAS_SCREEN

    UIFrameEvent e;

    // New or updated waypoint: focus on this frame next time Screen::setFrames runs
    if (shouldDraw()) {
        requestFocus();
        e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
    }

    // Deleting an old waypoint: remove the frame quietly, don't change frame position if possible
    else
        e.action = UIFrameEvent::Action::REGENERATE_FRAMESET_BACKGROUND;

    notifyObservers(&e);

#endif

    return ProcessMessage::CONTINUE; // Let others look at this message also if they want
}

#if HAS_SCREEN
bool WaypointModule::shouldDraw()
{
#if !MESHTASTIC_EXCLUDE_WAYPOINT
    if (!screen || !devicestate.has_rx_waypoint)
        return false;

    meshtastic_Waypoint wp{}; // <- replaces memset
    if (pb_decode_from_bytes(devicestate.rx_waypoint.decoded.payload.bytes, devicestate.rx_waypoint.decoded.payload.size,
                             &meshtastic_Waypoint_msg, &wp)) {
        return wp.expire > getTime();
    }
    return false; // no LOG_ERROR, no flag writes
#else
    return false;
#endif
}

/// Draw the last waypoint we received
void WaypointModule::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    if (!screen)
        return;
    display->clear();
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);
    int line = 1;

    // === Set Title
    const char *titleStr = "Waypoint";

    // === Header ===
    graphics::drawCommonHeader(display, x, y, titleStr);

    const int w = display->getWidth();
    const int h = display->getHeight();

    // Decode the waypoint
    const meshtastic_MeshPacket &mp = devicestate.rx_waypoint;
    meshtastic_Waypoint wp{};
    if (!pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, &meshtastic_Waypoint_msg, &wp)) {
        devicestate.has_rx_waypoint = false;
        return;
    }

    // Get timestamp info. Will pass as a field to drawColumns
    char lastStr[20];
    getTimeAgoStr(sinceReceived(&mp), lastStr, sizeof(lastStr));

    // Will contain distance information, passed as a field to drawColumns
    char distStr[20];

    // Get our node, to use our own position
    meshtastic_NodeInfoLite *ourNode = nodeDB->getMeshNode(nodeDB->getNodeNum());

    // Dimensions / co-ordinates for the compass - match UIRenderer pattern
    const int16_t topY = graphics::getTextPositions(display)[1];
    const int16_t bottomY = h - (FONT_HEIGHT_SMALL - 1);
    const int16_t usableHeight = bottomY - topY - 5;
    int16_t compassRadius = usableHeight / 2;
    if (compassRadius < 8)
        compassRadius = 8;
    const int16_t compassDiam = compassRadius * 2;
    const int16_t compassX = x + w - compassRadius - 8;
    const int16_t compassY = topY + (usableHeight / 2) + ((FONT_HEIGHT_SMALL - 1) / 2) + 2;

    // If our node has a position:
    if (ourNode && (nodeDB->hasValidPosition(ourNode) || screen->hasHeading())) {
        const meshtastic_PositionLite &op = ourNode->position;
        float myHeading = 0;
        if (uiconfig.compass_mode != meshtastic_CompassMode_FREEZE_HEADING) {
            if (screen->hasHeading())
                myHeading = degToRad(screen->getHeading());
            else
                myHeading = screen->estimatedHeading(DegD(op.latitude_i), DegD(op.longitude_i));
        }

        // Compass bearing to waypoint (returns radians, can be negative)
        float bearingToOther =
            GeoCoord::bearing(DegD(op.latitude_i), DegD(op.longitude_i), DegD(wp.latitude_i), DegD(wp.longitude_i));
        // If the top of the compass is not a static north we need adjust bearingToOther based on heading
        float bearingRel = bearingToOther;
        if (uiconfig.compass_mode != meshtastic_CompassMode_FREEZE_HEADING)
            bearingRel -= myHeading;

        // Normalize bearing to 0-360 degrees for display
        float bearingToOtherDegrees = radToDeg(bearingToOther);
        while (bearingToOtherDegrees < 0)
            bearingToOtherDegrees += 360.0f;
        while (bearingToOtherDegrees >= 360.0f)
            bearingToOtherDegrees -= 360.0f;

        // Distance to Waypoint
        float d = GeoCoord::latLongToMeter(DegD(wp.latitude_i), DegD(wp.longitude_i), DegD(op.latitude_i), DegD(op.longitude_i));
        if (config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL) {
            float feet = d * METERS_TO_FEET;
            snprintf(distStr, sizeof(distStr), feet < (2 * MILES_TO_FEET) ? "%.0fft   %.0f°" : "%.1fmi   %.0f°",
                     feet < (2 * MILES_TO_FEET) ? feet : feet / MILES_TO_FEET, bearingToOtherDegrees);
        } else {
            snprintf(distStr, sizeof(distStr), d < 2000 ? "%.0fm   %.0f°" : "%.1fkm   %.0f°", d < 2000 ? d : d / 1000,
                     bearingToOtherDegrees);
        }

        // Render compass based on display resolution - same pattern as favorite node screens
#if !defined(USE_EINK)
        if (graphics::isHighResolution()) {
            // 3D compass for high-resolution displays
            graphics::CompassRenderer::drawNodeHeading(display, compassX, compassY, compassDiam, bearingRel);
            graphics::CompassRenderer::drawCenterNeedle3D(display, compassX, compassY, compassRadius, bearingToOther);
        } else
#endif
        {
            // Classic small-screen compass: circle + 'N' + simple arrow
            display->drawCircle(compassX, compassY, compassRadius);
            graphics::CompassRenderer::drawCompassNorth(display, compassX, compassY, myHeading, compassRadius);
            graphics::CompassRenderer::drawNodeHeading(display, compassX, compassY, compassDiam, bearingRel);
        }
    } else {
        // No valid position - draw placeholder
        display->drawCircle(compassX, compassY, compassRadius);
        display->drawString(compassX - FONT_HEIGHT_SMALL / 4, compassY - FONT_HEIGHT_SMALL / 2, "?");

        // ? in the distance field
        snprintf(distStr, sizeof(distStr), "? %s ?°",
                 (config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL) ? "mi" : "km");
    }

    display->setTextAlignment(TEXT_ALIGN_LEFT); // Something above me changes to a different alignment, forcing a fix here!
    display->drawString(0, graphics::getTextPositions(display)[line++], lastStr);
    display->drawString(0, graphics::getTextPositions(display)[line++], wp.name);
    display->drawString(0, graphics::getTextPositions(display)[line++], wp.description);
    display->drawString(0, graphics::getTextPositions(display)[line++], distStr);
}
#endif
