#include "WaypointModule.h"
#include "NodeDB.h"
#include "PowerFSM.h"
#include "WaypointUtils.h"
#include "configuration.h"
#include "graphics/SharedUIDisplay.h"
#include "graphics/draw/CompassRenderer.h"
#include "mesh/Throttle.h"
#include "meshUtils.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#if !MESHTASTIC_EXCLUDE_WAYPOINT
#include "ExternalNotificationModule.h"
#include "MeshService.h"
#include "WaypointStore.h"
#include "mesh/Router.h"
#include <pb_encode.h>
#endif

#if HAS_SCREEN && !MESHTASTIC_EXCLUDE_WAYPOINT
#include "gps/RTC.h"
#include "graphics/Screen.h"
#include "graphics/TimeFormatters.h"
#include "graphics/draw/NodeListRenderer.h"
#include "graphics/draw/UIRenderer.h"
#include "main.h"
#endif

WaypointModule *waypointModule;

#if HAS_SCREEN && !MESHTASTIC_EXCLUDE_WAYPOINT
namespace
{

constexpr int16_t WAYPOINT_ROW_GAP = 2;
// The title is underlined on its baseline, so whatever follows needs a gap the other rows don't.
constexpr int16_t WAYPOINT_TITLE_GAP = 2;
// EmoteRenderer draws emotes at their native size, so the icon column follows the emote bitmaps
// rather than the body font - otherwise a 16px pin is drawn straight over the name.
constexpr int16_t WAYPOINT_ICON_BOX = 16;
constexpr int16_t WAYPOINT_ICON_GAP = 3;

// Short panels fit one card in the body font. WAYPOINT_LIST_TINY_FONT trades legibility for
// rows; the header is drawn in FONT_SMALL either way.
#ifdef WAYPOINT_LIST_TINY_FONT
#define WAYPOINT_LIST_FONT FONT_TINY
#define WAYPOINT_LIST_FONT_HEIGHT FONT_HEIGHT_TINY
#else
#define WAYPOINT_LIST_FONT FONT_SMALL
#define WAYPOINT_LIST_FONT_HEIGHT FONT_HEIGHT_SMALL
#endif

void drawFallbackWaypointIcon(OLEDDisplay *display, int16_t left, int16_t top, uint16_t boxSize)
{
    const int16_t cx = left + (boxSize / 2);
    const int16_t circleY = top + std::max<int16_t>(2, boxSize / 3);
    const int16_t r = std::max<int16_t>(1, boxSize / 4);
    display->drawCircle(cx, circleY, r);
    display->drawLine(cx, circleY + r, cx, top + boxSize - 2);
    display->setPixel(cx - 1, top + boxSize - 2);
    display->setPixel(cx + 1, top + boxSize - 2);
}

void drawWaypointIcon(OLEDDisplay *display, const meshtastic_Waypoint &wp, int16_t left, int16_t top, uint16_t boxSize)
{
    if (!wp.icon) {
        drawFallbackWaypointIcon(display, left, top, boxSize);
        return;
    }

    const std::string utf8 = WaypointUtils::utf8FromCodepoint(wp.icon);
    if (utf8.empty()) {
        drawFallbackWaypointIcon(display, left, top, boxSize);
        return;
    }

    graphics::UIRenderer::drawStringWithEmotes(display, left, top, utf8, boxSize, 1, false);
}

void formatWaypointDistance(char *out, size_t outSize, float meters)
{
    if (config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL) {
        const float feet = meters * METERS_TO_FEET;
        snprintf(out, outSize, feet < (2 * MILES_TO_FEET) ? "%.0fft" : "%.1fmi",
                 feet < (2 * MILES_TO_FEET) ? feet : feet / MILES_TO_FEET);
    } else {
        snprintf(out, outSize, meters < 2000 ? "%.0fm" : "%.1fkm", meters < 2000 ? meters : meters / 1000);
    }
}

void formatWaypointCoordinates(char *out, size_t outSize, const meshtastic_Waypoint &wp)
{
    if (!(wp.has_latitude_i && wp.has_longitude_i)) {
        snprintf(out, outSize, "--");
        return;
    }

    snprintf(out, outSize, "%.4f,%.4f", wp.latitude_i * 1e-7, wp.longitude_i * 1e-7);
}

void formatWaypointExpire(char *out, size_t outSize, const meshtastic_Waypoint &wp)
{
    if (wp.expire == 0) {
        out[0] = '\0';
        return;
    }

    const uint32_t now = getValidTime(RTCQuality::RTCQualityDevice);
    if (now == 0) {
        out[0] = '\0';
        return;
    }
    if (wp.expire <= now) {
        snprintf(out, outSize, "0m");
        return;
    }

    const uint32_t left = wp.expire - now;
    if (left < 3600)
        snprintf(out, outSize, "%lum", (unsigned long)((left + 59) / 60));
    else if (left < 86400)
        snprintf(out, outSize, "%luh", (unsigned long)((left + 3599) / 3600));
    else
        snprintf(out, outSize, "%lud", (unsigned long)((left + 86399) / 86400));
}

std::string trimmedWaypointText(const char *text)
{
    if (!text)
        return "";

    std::string value(text);
    const auto first = std::find_if(value.begin(), value.end(), [](unsigned char c) { return !std::isspace(c); });
    if (first == value.end())
        return "";

    const auto last = std::find_if(value.rbegin(), value.rend(), [](unsigned char c) { return !std::isspace(c); }).base();
    return std::string(first, last);
}

size_t collectDrawableWaypoints(const StoredWaypoint *entries[], size_t maxEntries)
{
    size_t count = 0;
    for (const StoredWaypoint &entry : waypointStore.getWaypoints()) {
        if (WaypointStore::isExpired(entry))
            continue;
        if (count >= maxEntries)
            break;
        entries[count] = &entry;
        ++count;
    }

    return count;
}

void drawDottedHorizontalDivider(OLEDDisplay *display, int16_t xStart, int16_t xEnd, int16_t y)
{
    for (int16_t x = xStart; x <= xEnd; x += 2) {
        display->setPixel(x, y);
    }
}

void notifyWaypointReceived(const StoredWaypoint &stored)
{
    if (screen) {
        const std::string waypointName = trimmedWaypointText(stored.waypoint.name);
        if (!waypointName.empty()) {
            char banner[96];
            snprintf(banner, sizeof(banner), "New Waypoint\n%s", waypointName.c_str());
            screen->showSimpleBanner(banner, 3000);
        } else {
            screen->showSimpleBanner("New Waypoint", 3000);
        }
    }

    if (externalNotificationModule)
        externalNotificationModule->startNotification();
}

// Autoscroll, timed like MessageRenderer's so the two screens behave the same: settle, crawl to
// the end, hold, snap back. The T1 has no way to scroll a list by hand.
constexpr uint32_t WAYPOINT_SCROLL_SETTLE_MS = 2000;
constexpr uint32_t WAYPOINT_SCROLL_HOLD_MS = 3000;
constexpr float WAYPOINT_SCROLL_SPEED = 2.0f;

float waypointScrollY = 0.0f;
uint32_t waypointScrollTick = 0;
uint32_t waypointScrollMark = 0;
uint32_t waypointScrollSignature = 0;
bool waypointScrollRunning = false;
bool waypointScrollHolding = false;

int16_t waypointCardHeight(bool hasDescription)
{
    const int16_t rows = hasDescription ? ((WAYPOINT_LIST_FONT_HEIGHT * 3) + WAYPOINT_TITLE_GAP + 1)
                                        : ((WAYPOINT_LIST_FONT_HEIGHT * 2) + WAYPOINT_TITLE_GAP);
    // A card is never shorter than its icon, or the icon bleeds into the card below.
    return std::max<int16_t>(rows, WAYPOINT_ICON_BOX);
}

bool waypointHasDescription(const meshtastic_Waypoint &wp)
{
    char safeDescription[sizeof(wp.description)];
    memcpy(safeDescription, wp.description, sizeof(safeDescription));
    safeDescription[sizeof(safeDescription) - 1] = '\0';
    sanitizeUtf8(safeDescription, sizeof(safeDescription));
    return !trimmedWaypointText(safeDescription).empty();
}

void advanceWaypointScroll(int16_t maxScroll, uint32_t signature)
{
    const uint32_t now = millis();
    if (signature != waypointScrollSignature) {
        waypointScrollSignature = signature;
        waypointScrollY = 0.0f;
        waypointScrollRunning = false;
        waypointScrollHolding = false;
        waypointScrollMark = now;
        waypointScrollTick = now;
    }

#ifdef USE_EINK
    (void)maxScroll;
    waypointScrollY = 0.0f; // a partial refresh per frame is not worth a moving list
#else
    const float delta = (now - waypointScrollTick) / 400.0f;
    waypointScrollTick = now;

    if (maxScroll <= 0) {
        waypointScrollY = 0.0f;
        waypointScrollRunning = false;
        waypointScrollHolding = false;
        waypointScrollMark = now;
        return;
    }

    if (!waypointScrollRunning) {
        if (Throttle::hasElapsed(waypointScrollMark, WAYPOINT_SCROLL_SETTLE_MS))
            waypointScrollRunning = true;
        return;
    }

    if (waypointScrollHolding) {
        if (Throttle::hasElapsed(waypointScrollMark, WAYPOINT_SCROLL_HOLD_MS)) {
            waypointScrollY = 0.0f;
            waypointScrollHolding = false;
            waypointScrollRunning = false;
            waypointScrollMark = now;
        }
        return;
    }

    waypointScrollY += delta * WAYPOINT_SCROLL_SPEED;
    if (waypointScrollY >= maxScroll) {
        waypointScrollY = maxScroll;
        waypointScrollHolding = true;
        waypointScrollMark = now;
    }
#endif
}

} // namespace
#endif

ProcessMessage WaypointModule::handleReceived(const meshtastic_MeshPacket &mp)
{
#if defined(DEBUG_PORT) && !defined(DEBUG_MUTE)
    auto &p = mp.decoded;
    LOG_INFO("Received waypoint msg from=0x%08x, id=0x%08x, msg=%.*s", mp.from, mp.id, p.payload.size, p.payload.bytes);
#endif
#if MESHTASTIC_EXCLUDE_WAYPOINT
    (void)mp;
    return ProcessMessage::CONTINUE;
#else
    StoredWaypoint stored;
    if (!waypointStore.addFromPacket(mp, isFromUs(&mp), &stored))
        return ProcessMessage::CONTINUE;

    powerFSM.trigger(EVENT_RECEIVED_MSG);

#if HAS_SCREEN
    if (!isFromUs(&mp) && !WaypointStore::isExpired(stored))
        notifyWaypointReceived(stored);

    UIFrameEvent e;
    // Refresh the waypoint frame list quietly; new waypoints alert via banner/sound but do not
    // steal focus from the screen the user is already on.
    e.action = UIFrameEvent::Action::REGENERATE_FRAMESET_BACKGROUND;

    notifyObservers(&e);

#endif

    return ProcessMessage::CONTINUE; // Let others look at this message also if they want
#endif
}

#if !MESHTASTIC_EXCLUDE_WAYPOINT
bool WaypointModule::broadcastDelete(uint32_t waypointId)
{
    meshtastic_Waypoint wp = meshtastic_Waypoint_init_zero;
    bool found = false;
    for (const auto &entry : waypointStore.getWaypoints()) {
        if (entry.waypoint.id == waypointId) {
            wp = entry.waypoint;
            found = true;
            break;
        }
    }
    if (!found)
        return false;

    // Respect the waypoint's lock: we may remove a locked waypoint from our own device, but
    // we're not the owner, so we have no authority to delete it mesh-wide.
    const NodeNum localNodeNum = nodeDB ? nodeDB->getNodeNum() : 0;
    if (wp.locked_to != 0 && wp.locked_to != localNodeNum) {
        LOG_INFO("Waypoint 0x%08x is locked to 0x%08x; removing locally only", waypointId, wp.locked_to);
        waypointStore.removeWaypoint(waypointId);
        return true;
    }

    // Already-expired = the mesh convention for "delete this waypoint".
    wp.expire = 1;

    if (!service)
        return false;

    meshtastic_MeshPacket *p = router ? router->allocForSending() : nullptr;
    if (!p)
        return false;

    p->decoded.portnum = meshtastic_PortNum_WAYPOINT_APP;
    p->decoded.payload.size =
        pb_encode_to_bytes(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), &meshtastic_Waypoint_msg, &wp);
    if (p->decoded.payload.size == 0) {
        packetPool.release(p);
        return false;
    }

    service->sendToMesh(p, RX_SRC_USER);

    waypointStore.removeWaypoint(waypointId);

    return true;
}
#endif

#if HAS_SCREEN
bool WaypointModule::shouldDraw()
{
#if !MESHTASTIC_EXCLUDE_WAYPOINT
    if (!screen || waypointStore.getWaypoints().empty())
        return false;

    for (const StoredWaypoint &entry : waypointStore.getWaypoints()) {
        if (!WaypointStore::isExpired(entry))
            return true;
    }
    return false;
#else
    return false;
#endif
}

void WaypointModule::onDeviceTimeChanged()
{
#if !MESHTASTIC_EXCLUDE_WAYPOINT
    if (!screen)
        return;

    // Refresh only; never steal focus.
    UIFrameEvent e;
    e.action = UIFrameEvent::Action::REGENERATE_FRAMESET_BACKGROUND;
    notifyObservers(&e);
#endif
}

/// Draw the newest non-expired waypoints we received
void WaypointModule::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    (void)state;
#if MESHTASTIC_EXCLUDE_WAYPOINT
    (void)display;
    (void)x;
    (void)y;
    return;
#else
    if (!screen)
        return;

    display->clear();
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);
    const StoredWaypoint *entries[WAYPOINT_HISTORY_LIMIT];
    const size_t totalWaypoints = collectDrawableWaypoints(entries, WAYPOINT_HISTORY_LIMIT);
    if (totalWaypoints == 0)
        return;

    const char *titleStr = (totalWaypoints == 1) ? "Waypoint" : "Waypoints";
    display->setFont(WAYPOINT_LIST_FONT);

    const meshtastic_NodeInfoLite *ourNode = nodeDB->getMeshNode(nodeDB->getNodeNum());
    const bool hasOwnPositionFix = (ourNode && nodeDB->hasValidPosition(ourNode));
    meshtastic_PositionLite ownPos = meshtastic_PositionLite_init_zero;
    const bool haveOwnPos = ourNode && nodeDB->copyNodePosition(ourNode->num, ownPos);

    const int16_t nameX = WAYPOINT_ICON_BOX + WAYPOINT_ICON_GAP;
    const int16_t contentBottom = display->getHeight() - 1;
    // Body starts below the painted header. textFirstLine sits 2px inside it, which works for text
    // because glyph ascenders hide the overlap, but not for the icon or the title underline.
    const int16_t bodyTop = BASEUI_HEADER_HEIGHT + BASEUI_BELOW_HEADER_MARGIN;

    // Measure every card first: the stride depends on whether each waypoint carries a description.
    int16_t totalHeight = 0;
    uint32_t signature = static_cast<uint32_t>(totalWaypoints);
    for (size_t i = 0; i < totalWaypoints; ++i) {
        totalHeight += waypointCardHeight(waypointHasDescription(entries[i]->waypoint));
        if (i + 1 < totalWaypoints)
            totalHeight += 1 + WAYPOINT_ROW_GAP; // divider plus the gap under it
        signature = (signature * 31u) + entries[i]->waypoint.id;
    }
    const int16_t maxScroll = std::max<int16_t>(0, totalHeight - (contentBottom - bodyTop));
    advanceWaypointScroll(maxScroll, signature);

    int16_t rowTop = bodyTop - static_cast<int16_t>(waypointScrollY);

    for (size_t i = 0; i < totalWaypoints; ++i) {
        const StoredWaypoint &entry = *entries[i];
        const meshtastic_Waypoint &wp = entry.waypoint;

        const bool hasDescription = waypointHasDescription(wp);
        const int16_t cardBottom = rowTop + waypointCardHeight(hasDescription);
        const int16_t separatorY = cardBottom + 1;
        const int16_t nextRowTop = separatorY + WAYPOINT_ROW_GAP;

        if (rowTop > contentBottom)
            break;
        if (cardBottom < bodyTop) { // scrolled off the top
            rowTop = nextRowTop;
            continue;
        }

        char safeName[sizeof(wp.name)];
        memcpy(safeName, wp.name, sizeof(safeName));
        safeName[sizeof(safeName) - 1] = '\0';
        sanitizeUtf8(safeName, sizeof(safeName));

        char safeDescription[sizeof(wp.description)];
        memcpy(safeDescription, wp.description, sizeof(safeDescription));
        safeDescription[sizeof(safeDescription) - 1] = '\0';
        sanitizeUtf8(safeDescription, sizeof(safeDescription));

        char distStr[20] = "";
        char coordStr[40];
        char expireStr[16];
        formatWaypointCoordinates(coordStr, sizeof(coordStr), wp);
        formatWaypointExpire(expireStr, sizeof(expireStr), wp);

        const std::string description = trimmedWaypointText(safeDescription);
        const int16_t row1Y = rowTop;
        const int16_t row2Y = row1Y + WAYPOINT_LIST_FONT_HEIGHT + WAYPOINT_TITLE_GAP;
        const int16_t rowMetaY = hasDescription ? (row2Y + WAYPOINT_LIST_FONT_HEIGHT + 1) : row2Y;

        bool showCompass = false;
        float myHeading = 0.0f;
        float bearingToOther = 0.0f;
        if (hasOwnPositionFix && haveOwnPos && wp.has_latitude_i && wp.has_longitude_i) {
            const float d = GeoCoord::latLongToMeter(DegD(wp.latitude_i), DegD(wp.longitude_i), DegD(ownPos.latitude_i),
                                                     DegD(ownPos.longitude_i));
            formatWaypointDistance(distStr, sizeof(distStr), d);

            if (graphics::CompassRenderer::getHeadingRadians(DegD(ownPos.latitude_i), DegD(ownPos.longitude_i), myHeading)) {
                showCompass = true;
                bearingToOther = GeoCoord::bearing(DegD(ownPos.latitude_i), DegD(ownPos.longitude_i), DegD(wp.latitude_i),
                                                   DegD(wp.longitude_i));
                bearingToOther = graphics::CompassRenderer::adjustBearingForCompassMode(bearingToOther, myHeading);
            }
        }

        const int16_t compactArrowCenterX = display->getWidth() - ((WAYPOINT_LIST_FONT_HEIGHT > 10) ? 9 : 7);
        const int16_t compactArrowCenterY = (hasDescription ? row2Y : row1Y) + (WAYPOINT_LIST_FONT_HEIGHT / 2);
        const int16_t compactContentRight = compactArrowCenterX - 8;
        const char *distanceLabel = distStr[0] ? distStr : "--";
        const char *expireLabel = expireStr[0] ? expireStr : "--";
        const uint16_t metaWidth =
            std::max<uint16_t>(display->getStringWidth(distanceLabel), display->getStringWidth(expireLabel)) + 4;
        const int16_t metaLeft = std::max<int16_t>(nameX + 16, compactContentRight - metaWidth);
        const int16_t textRight = metaLeft - 4;
        const uint16_t nameWidth = (textRight > nameX) ? (textRight - nameX) : 0;
        const std::string shownName = graphics::UIRenderer::truncateStringWithEmotes(display, safeName, nameWidth);
        const std::string shownDescription =
            hasDescription ? graphics::UIRenderer::truncateStringWithEmotes(display, description, nameWidth) : std::string();

        drawWaypointIcon(display, wp, 0, row1Y, WAYPOINT_ICON_BOX);
        graphics::UIRenderer::drawStringWithEmotes(display, nameX, row1Y, shownName, WAYPOINT_LIST_FONT_HEIGHT, 1, false);
        const int16_t underlineY = row1Y + WAYPOINT_LIST_FONT_HEIGHT;
        const int16_t underlineRight =
            std::min<int16_t>(textRight, nameX + graphics::UIRenderer::measureStringWithEmotes(display, shownName) - 1);
        if (underlineRight >= nameX)
            display->drawLine(nameX, underlineY, underlineRight, underlineY);

        if (hasDescription)
            graphics::UIRenderer::drawStringWithEmotes(display, nameX, row2Y, shownDescription, WAYPOINT_LIST_FONT_HEIGHT, 1,
                                                       false);

        if (showCompass)
            graphics::NodeListRenderer::drawRelativeCompassArrow(display, compactArrowCenterX, compactArrowCenterY,
                                                                 graphics::CompassRenderer::radiansToDegrees360(bearingToOther));

        display->drawStringMaxWidth(nameX, rowMetaY, nameWidth, coordStr);
        display->setTextAlignment(TEXT_ALIGN_RIGHT);
        display->drawString(metaLeft + metaWidth - 1, row1Y, distanceLabel);
        display->drawString(metaLeft + metaWidth - 1, rowMetaY, expireLabel);
        display->setTextAlignment(TEXT_ALIGN_LEFT);

        if (i + 1 < totalWaypoints && separatorY >= bodyTop && separatorY <= contentBottom)
            drawDottedHorizontalDivider(display, 0, display->getWidth() - 1, separatorY);
        rowTop = nextRowTop;
    }

    // Header last, so a card scrolled under it is painted over rather than through.
    graphics::drawCommonHeader(display, x, y, titleStr);
#endif
}
#endif
