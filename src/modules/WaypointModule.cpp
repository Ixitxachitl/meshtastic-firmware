#include "WaypointModule.h"
#include "NodeDB.h"
#include "PowerFSM.h"
#include "configuration.h"
#include "graphics/SharedUIDisplay.h"
#include "graphics/draw/CompassRenderer.h"
#include "meshUtils.h"

#if HAS_SCREEN
#include "gps/RTC.h"
#include "graphics/Screen.h"
#include "graphics/TimeFormatters.h"
#include "graphics/draw/NodeListRenderer.h"
#include "graphics/draw/UIRenderer.h"
#include "main.h"
#endif

WaypointModule *waypointModule;

ProcessMessage WaypointModule::handleReceived(const meshtastic_MeshPacket &mp)
{
#if defined(DEBUG_PORT) && !defined(DEBUG_MUTE)
    auto &p = mp.decoded;
    LOG_INFO("Received waypoint msg from=0x%08x, id=0x%08x, msg=%.*s", mp.from, mp.id, p.payload.size, p.payload.bytes);
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
    graphics::clearForFrame(display, state);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);
    int line = 1;

    // Rows 1..4 below: age, name, description, distance. Reserved as a block so the compass
    // does not move between waypoints that fill a different number of them.
    constexpr int kWaypointContentRows = 4;

    // === Set Title
    const char *titleStr = "Waypoint";

    // === Header ===
    graphics::drawCommonHeader(display, x, y, titleStr);

    // Laid out like the favorite node screen: rows hang off a y that carries the
    // below-header margins, and the compass comes from the shared placement helper.
    y += BASEUI_BELOW_HEADER_MARGIN + BASEUI_BODY_TOP_MARGIN;
    auto row = [&](int slot) { return graphics::getTextPositions(display)[slot] + y; };
    const int bodyX = x + BASEUI_BODY_LR_MARGIN;

    // Decode the waypoint
    const meshtastic_MeshPacket &mp = devicestate.rx_waypoint;
    meshtastic_Waypoint wp{};
    if (!pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, &meshtastic_Waypoint_msg, &wp)) {
        devicestate.has_rx_waypoint = false;
        return;
    }

    // Sanitize before these reach the OLED renderer (defense-in-depth vs PB_VALIDATE_UTF8).
    sanitizeUtf8(wp.name, sizeof(wp.name));
    sanitizeUtf8(wp.description, sizeof(wp.description));

    // Get timestamp info. Will pass as a field to drawColumns
    char lastStr[20];
    getTimeAgoStr(sinceReceived(&mp), lastStr, sizeof(lastStr));

    // Will contain distance information, passed as a field to drawColumns
    char distStr[20] = "";

    // Get our node, to use our own position
    const meshtastic_NodeInfoLite *ourNode = nodeDB->getMeshNode(nodeDB->getNodeNum());

    // Match compass sizing/placement to favorite node screen logic.
    int16_t compassRadius = 8;
    int16_t compassX = x + display->getWidth() - compassRadius - 8;
    int16_t compassY = y + display->getHeight() / 2;
    bool haveCompassPlacement = true;

    if (SCREEN_WIDTH > SCREEN_HEIGHT) {
        const int16_t topY = row(1);
        const int16_t bottomY = SCREEN_HEIGHT - (FONT_HEIGHT_SMALL - 1);
        const int16_t usableHeight = bottomY - topY - 5;
        compassRadius = usableHeight / 2;
        if (compassRadius < 8)
            compassRadius = 8;
        compassX = x + SCREEN_WIDTH - compassRadius - 8;
        compassY = topY + (usableHeight / 2) + ((FONT_HEIGHT_SMALL - 1) / 2) + 2;
    } else {
        // Waypoint content uses rows 1..4. Reserve that whole block rather than the rows a
        // given waypoint happens to fill, so the compass stays put between waypoints - the
        // same rule BASEUI_FIXED_COMPASS_SIZE applies on the favorite node screen.
        const int yBelowContent = row(kWaypointContentRows) + FONT_HEIGHT_SMALL + 2;
#if defined(USE_EINK)
        const int iconSize = (graphics::currentResolution == graphics::ScreenResolution::High) ? 16 : 8;
        const int navBarHeight = iconSize + 6;
#else
        const int navBarHeight = 0;
#endif
        haveCompassPlacement = graphics::UIRenderer::computeBottomCompassPlacement(display, x, yBelowContent, navBarHeight, 4,
                                                                                   &compassX, &compassY, &compassRadius);
    }

    const bool hasOwnPositionFix = (ourNode && nodeDB->hasValidPosition(ourNode));
    const char *statusLine1 = nullptr;
    const char *statusLine2 = nullptr;
    bool showCompass = false;
    float myHeading = 0.0f;
    float bearingToOther = 0.0f;

    // Distance only needs our own position fix; compass/bearing additionally needs heading.
    meshtastic_PositionLite ownPos;
    const bool haveOwnPos = ourNode && nodeDB->copyNodePosition(ourNode->num, ownPos);
    if (hasOwnPositionFix && haveOwnPos) {
        const meshtastic_PositionLite &op = ownPos;
        const float d =
            GeoCoord::latLongToMeter(DegD(wp.latitude_i), DegD(wp.longitude_i), DegD(op.latitude_i), DegD(op.longitude_i));

        // Always show distance once we have an own-position fix, even without heading.
        if (config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL) {
            float feet = d * METERS_TO_FEET;
            snprintf(distStr, sizeof(distStr), feet < (2 * MILES_TO_FEET) ? "%.0fft" : "%.1fmi",
                     feet < (2 * MILES_TO_FEET) ? feet : feet / MILES_TO_FEET);
        } else {
            snprintf(distStr, sizeof(distStr), d < 2000 ? "%.0fm" : "%.1fkm", d < 2000 ? d : d / 1000);
        }

        const bool hasHeading =
            graphics::CompassRenderer::getHeadingRadians(DegD(op.latitude_i), DegD(op.longitude_i), myHeading);
        if (hasHeading) {
            showCompass = true;
            bearingToOther =
                GeoCoord::bearing(DegD(op.latitude_i), DegD(op.longitude_i), DegD(wp.latitude_i), DegD(wp.longitude_i));
            bearingToOther = graphics::CompassRenderer::adjustBearingForCompassMode(bearingToOther, myHeading);

            const float bearingToOtherDegrees = graphics::CompassRenderer::radiansToDegrees360(bearingToOther);

            // Distance to waypoint with relative bearing when heading is available.
            if (config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL) {
                float feet = d * METERS_TO_FEET;
                snprintf(distStr, sizeof(distStr), feet < (2 * MILES_TO_FEET) ? "%.0fft   %.0f°" : "%.1fmi   %.0f°",
                         feet < (2 * MILES_TO_FEET) ? feet : feet / MILES_TO_FEET, bearingToOtherDegrees);
            } else {
                snprintf(distStr, sizeof(distStr), d < 2000 ? "%.0fm   %.0f°" : "%.1fkm   %.0f°", d < 2000 ? d : d / 1000,
                         bearingToOtherDegrees);
            }

        } else {
            statusLine1 = "No";
            statusLine2 = "Heading";
        }
    } else {
        // No own fix yet, so compass/bearing data would be misleading.
        statusLine1 = "No";
        statusLine2 = "Fix";
    }

    if (haveCompassPlacement && (showCompass || statusLine1)) {
        graphics::UIRenderer::drawBearingCompassOrStatus(display, compassX, compassY, compassRadius, showCompass, myHeading,
                                                         bearingToOther, statusLine1, statusLine2);
    }

    display->setTextAlignment(TEXT_ALIGN_LEFT); // Something above me changes to a different alignment, forcing a fix here!
    display->drawString(bodyX, row(line++), lastStr);
    display->drawString(bodyX, row(line++), wp.name);
    display->drawString(bodyX, row(line++), wp.description);
    if (distStr[0])
        display->drawString(bodyX, row(line++), distStr);

    graphics::drawCommonFooter(display, x, y);
}
#endif
