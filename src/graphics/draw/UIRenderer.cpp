#include "configuration.h"
#if HAS_SCREEN
#include "CompassRenderer.h"
#include "GPSStatus.h"
#include "MessageRenderer.h"
#include "NodeDB.h"
#include "NodeListRenderer.h"
#include "UIRenderer.h"
#include "airtime.h"
#include "gps/GeoCoord.h"
#include "graphics/SharedUIDisplay.h"
#include "graphics/TimeFormatters.h"
#include "graphics/draw/Math3D.h"
#include "graphics/emotes.h"
#include "graphics/images.h"
#include "main.h"
#include "target_specific.h"
#include <OLEDDisplay.h>
#include <RTC.h>
#include <cstring>

// External variables
extern graphics::Screen *screen;
#if !MESHTASTIC_EXCLUDE_BMI270
extern "C" Quat GetAttitudeForRenderer();
extern "C" uint32_t GetStepCountForRenderer();
extern "C" bool HasStepCounterForRenderer();
#endif
#ifdef HAS_BHI260AP_SENSORLIB
extern "C" bool GetBHI260APDataForRenderer(float *accelX, float *accelY, float *accelZ, float *gyroX, float *gyroY, float *gyroZ);
#endif
#if defined(M5STACK_UNITC6L) || defined(USE_TINY_FONT)
static uint32_t lastSwitchTime = 0;
#endif
namespace graphics
{
NodeNum UIRenderer::currentFavoriteNodeNum = 0;
std::vector<meshtastic_NodeInfoLite *> graphics::UIRenderer::favoritedNodes;

using graphics::Emote;
using graphics::emotes;
using graphics::numEmotes;

static inline void drawSatelliteIcon(OLEDDisplay *display, int16_t x, int16_t y)
{
#if defined(M5STACK_UNITC6L) || defined(USE_TINY_FONT)
    int yOffset = -2;
#else
    int yOffset = (currentResolution == ScreenResolution::High) ? -5 : 1;
#endif
    if (currentResolution == ScreenResolution::High) {
        NodeListRenderer::drawScaledXBitmap16x16(x, y + yOffset, imgSatellite_width, imgSatellite_height, imgSatellite, display);
    } else {
        display->drawXbm(x + 1, y + yOffset, imgSatellite_width, imgSatellite_height, imgSatellite);
    }
}

// Footprint icon bitmap (16x16)
static const unsigned char footprint[] PROGMEM = {0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x78, 0x00, 0x78, 0x0C, 0x78,
                                                  0x1E, 0x78, 0x1E, 0x78, 0x1E, 0x00, 0x1E, 0x78, 0x1E, 0x78, 0x00,
                                                  0x30, 0x1E, 0x00, 0x1E, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x00};

static inline void drawFootprintIcon(OLEDDisplay *display, int16_t x, int16_t y)
{
    // Draw the 16x16 footprint bitmap
    display->drawXbm(x, y, 16, 16, footprint);
}

void graphics::UIRenderer::rebuildFavoritedNodes()
{
    favoritedNodes.clear();
    size_t total = nodeDB->getNumMeshNodes();
    for (size_t i = 0; i < total; i++) {
        meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
        if (!n || n->num == nodeDB->getNodeNum())
            continue;
        if (n->is_favorite)
            favoritedNodes.push_back(n);
    }

    std::sort(favoritedNodes.begin(), favoritedNodes.end(),
              [](const meshtastic_NodeInfoLite *a, const meshtastic_NodeInfoLite *b) { return a->num < b->num; });
}

#if !MESHTASTIC_EXCLUDE_GPS
// GeoCoord object for coordinate conversions
extern GeoCoord geoCoord;

// Threshold values for the GPS lock accuracy bar display
extern uint32_t dopThresholds[5];

// Draw GPS status summary
void UIRenderer::drawGps(OLEDDisplay *display, int16_t x, int16_t y, const meshtastic::GPSStatus *gps)
{
#if defined(M5STACK_UNITC6L) || defined(USE_TINY_FONT)
    int yOffset = -2;
#else
    int yOffset = (currentResolution == ScreenResolution::High) ? -2 : 1;
#endif
    // Draw satellite image
    if (currentResolution == ScreenResolution::High) {
        NodeListRenderer::drawScaledXBitmap16x16(x, y + yOffset, imgSatellite_width, imgSatellite_height, imgSatellite, display);
    } else {
        display->drawXbm(x + 1, y + yOffset, imgSatellite_width, imgSatellite_height, imgSatellite);
    }
    char textString[10];

    if (config.position.fixed_position) {
        // GPS coordinates are currently fixed
        snprintf(textString, sizeof(textString), "Fixed");
    }
    if (!gps->getIsConnected()) {
        snprintf(textString, sizeof(textString), "No Lock");
    }
    if (!gps->getHasLock()) {
        // Draw "No sats" to the right of the icon with slightly more gap
        snprintf(textString, sizeof(textString), "No Sats");
    } else {
        snprintf(textString, sizeof(textString), "%u sats", gps->getNumSatellites());
    }
    if (currentResolution == ScreenResolution::High) {
        display->drawString(x + 18, y, textString);
    } else {
        display->drawString(x + 11, y, textString);
    }
}

// Draw status when GPS is disabled or not present
void UIRenderer::drawGpsPowerStatus(OLEDDisplay *display, int16_t x, int16_t y, const meshtastic::GPSStatus *gps)
{
    const char *displayLine;
    int pos;
    if (y < FONT_HEIGHT_SMALL) { // Line 1: use short string
        displayLine = config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_NOT_PRESENT ? "No GPS" : "GPS off";
        pos = display->getWidth() - display->getStringWidth(displayLine);
    } else {
        displayLine = config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_NOT_PRESENT ? "GPS not present"
                                                                                                       : "GPS is disabled";
        pos = (display->getWidth() - display->getStringWidth(displayLine)) / 2;
    }
    display->drawString(x + pos, y, displayLine);
}

void UIRenderer::drawGpsAltitude(OLEDDisplay *display, int16_t x, int16_t y, const meshtastic::GPSStatus *gps)
{
    char displayLine[32];
    if (!gps->getIsConnected() && !config.position.fixed_position) {
        // displayLine = "No GPS Module";
        // display->drawString(x + (SCREEN_WIDTH - (display->getStringWidth(displayLine))) / 2, y, displayLine);
    } else if (!gps->getHasLock() && !config.position.fixed_position) {
        // displayLine = "No GPS Lock";
        // display->drawString(x + (SCREEN_WIDTH - (display->getStringWidth(displayLine))) / 2, y, displayLine);
    } else {
        geoCoord.updateCoords(int32_t(gps->getLatitude()), int32_t(gps->getLongitude()), int32_t(gps->getAltitude()));
        if (config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL)
            snprintf(displayLine, sizeof(displayLine), "Altitude: %.0fft", geoCoord.getAltitude() * METERS_TO_FEET);
        else
            snprintf(displayLine, sizeof(displayLine), "Altitude: %.0im", geoCoord.getAltitude());
        display->drawString(x + (display->getWidth() - (display->getStringWidth(displayLine))) / 2, y, displayLine);
    }
}

// Draw GPS status coordinates
void UIRenderer::drawGpsCoordinates(OLEDDisplay *display, int16_t x, int16_t y, const meshtastic::GPSStatus *gps,
                                    const char *mode)
{
    auto gpsFormat = uiconfig.gps_format;
    char displayLine[32];

    if (!gps->getIsConnected() && !config.position.fixed_position) {
        if (strcmp(mode, "line1") == 0) {
            strcpy(displayLine, "No GPS present");
            display->drawString(x, y, displayLine);
        }
    } else if (!gps->getHasLock() && !config.position.fixed_position) {
        if (strcmp(mode, "line1") == 0) {
            strcpy(displayLine, "No GPS Lock");
            display->drawString(x, y, displayLine);
        }
    } else {

        geoCoord.updateCoords(int32_t(gps->getLatitude()), int32_t(gps->getLongitude()), int32_t(gps->getAltitude()));

        if (gpsFormat != meshtastic_DeviceUIConfig_GpsCoordinateFormat_DMS) {
            char coordinateLine_1[22];
            char coordinateLine_2[22];
            if (gpsFormat == meshtastic_DeviceUIConfig_GpsCoordinateFormat_DEC) { // Decimal Degrees
                snprintf(coordinateLine_1, sizeof(coordinateLine_1), "Lat: %f", geoCoord.getLatitude() * 1e-7);
                snprintf(coordinateLine_2, sizeof(coordinateLine_2), "Lon: %f", geoCoord.getLongitude() * 1e-7);
            } else if (gpsFormat == meshtastic_DeviceUIConfig_GpsCoordinateFormat_UTM) { // Universal Transverse Mercator
                snprintf(coordinateLine_1, sizeof(coordinateLine_1), "%2i%1c %06u E", geoCoord.getUTMZone(),
                         geoCoord.getUTMBand(), geoCoord.getUTMEasting());
                snprintf(coordinateLine_2, sizeof(coordinateLine_2), "%07u N", geoCoord.getUTMNorthing());
            } else if (gpsFormat == meshtastic_DeviceUIConfig_GpsCoordinateFormat_MGRS) { // Military Grid Reference System
                snprintf(coordinateLine_1, sizeof(coordinateLine_1), "%2i%1c %1c%1c", geoCoord.getMGRSZone(),
                         geoCoord.getMGRSBand(), geoCoord.getMGRSEast100k(), geoCoord.getMGRSNorth100k());
                snprintf(coordinateLine_2, sizeof(coordinateLine_2), "%05u E %05u N", geoCoord.getMGRSEasting(),
                         geoCoord.getMGRSNorthing());
            } else if (gpsFormat == meshtastic_DeviceUIConfig_GpsCoordinateFormat_OLC) { // Open Location Code
                geoCoord.getOLCCode(coordinateLine_1);
                coordinateLine_2[0] = '\0';
            } else if (gpsFormat == meshtastic_DeviceUIConfig_GpsCoordinateFormat_OSGR) { // Ordnance Survey Grid Reference
                if (geoCoord.getOSGRE100k() == 'I' || geoCoord.getOSGRN100k() == 'I') { // OSGR is only valid around the UK region
                    snprintf(coordinateLine_1, sizeof(coordinateLine_1), "%s", "Out of Boundary");
                    coordinateLine_2[0] = '\0';
                } else {
                    snprintf(coordinateLine_1, sizeof(coordinateLine_1), "%1c%1c", geoCoord.getOSGRE100k(),
                             geoCoord.getOSGRN100k());
                    snprintf(coordinateLine_2, sizeof(coordinateLine_2), "%05u E %05u N", geoCoord.getOSGREasting(),
                             geoCoord.getOSGRNorthing());
                }
            } else if (gpsFormat == meshtastic_DeviceUIConfig_GpsCoordinateFormat_MLS) { // Maidenhead Locator System
                double lat = geoCoord.getLatitude() * 1e-7;
                double lon = geoCoord.getLongitude() * 1e-7;

                // Normalize
                if (lat > 90.0)
                    lat = 90.0;
                if (lat < -90.0)
                    lat = -90.0;
                while (lon < -180.0)
                    lon += 360.0;
                while (lon >= 180.0)
                    lon -= 360.0;

                double adjLon = lon + 180.0;
                double adjLat = lat + 90.0;

                char maiden[10]; // enough for 8-char + null

                // Field (2 letters)
                int lonField = int(adjLon / 20.0);
                int latField = int(adjLat / 10.0);
                adjLon -= lonField * 20.0;
                adjLat -= latField * 10.0;

                // Square (2 digits)
                int lonSquare = int(adjLon / 2.0);
                int latSquare = int(adjLat / 1.0);
                adjLon -= lonSquare * 2.0;
                adjLat -= latSquare * 1.0;

                // Subsquare (2 letters)
                double lonUnit = 2.0 / 24.0;
                double latUnit = 1.0 / 24.0;
                int lonSub = int(adjLon / lonUnit);
                int latSub = int(adjLat / latUnit);

                snprintf(maiden, sizeof(maiden), "%c%c%c%c%c%c", 'A' + lonField, 'A' + latField, '0' + lonSquare, '0' + latSquare,
                         'A' + lonSub, 'A' + latSub);

                snprintf(coordinateLine_1, sizeof(coordinateLine_1), "MH: %s", maiden);
                coordinateLine_2[0] = '\0'; // only need one line
            }

            if (strcmp(mode, "line1") == 0) {
                display->drawString(x, y, coordinateLine_1);
            } else if (strcmp(mode, "line2") == 0) {
                display->drawString(x, y, coordinateLine_2);
            } else if (strcmp(mode, "combined") == 0) {
                display->drawString(x, y, coordinateLine_1);
                if (coordinateLine_2[0] != '\0') {
                    display->drawString(x + display->getStringWidth(coordinateLine_1), y, coordinateLine_2);
                }
            }

        } else {
            char coordinateLine_1[22];
            char coordinateLine_2[22];
            snprintf(coordinateLine_1, sizeof(coordinateLine_1), "Lat: %2i° %2i' %2u\" %1c", geoCoord.getDMSLatDeg(),
                     geoCoord.getDMSLatMin(), geoCoord.getDMSLatSec(), geoCoord.getDMSLatCP());
            snprintf(coordinateLine_2, sizeof(coordinateLine_2), "Lon: %3i° %2i' %2u\" %1c", geoCoord.getDMSLonDeg(),
                     geoCoord.getDMSLonMin(), geoCoord.getDMSLonSec(), geoCoord.getDMSLonCP());
            if (strcmp(mode, "line1") == 0) {
                display->drawString(x, y, coordinateLine_1);
            } else if (strcmp(mode, "line2") == 0) {
                display->drawString(x, y, coordinateLine_2);
            } else { // both
                display->drawString(x, y, coordinateLine_1);
                display->drawString(x, y + 10, coordinateLine_2);
            }
        }
    }
}
#endif // !MESHTASTIC_EXCLUDE_GPS

// Draw nodes status
void UIRenderer::drawNodes(OLEDDisplay *display, int16_t x, int16_t y, const meshtastic::NodeStatus *nodeStatus, int node_offset,
                           bool show_total, const char *additional_words)
{
    char usersString[20];
    int nodes_online = (nodeStatus->getNumOnline() > 0) ? nodeStatus->getNumOnline() + node_offset : 0;

    snprintf(usersString, sizeof(usersString), "%d %s", nodes_online, additional_words);

    if (show_total) {
        int nodes_total = (nodeStatus->getNumTotal() > 0) ? nodeStatus->getNumTotal() + node_offset : 0;
        snprintf(usersString, sizeof(usersString), "%d/%d %s", nodes_online, nodes_total, additional_words);
    }

#if (defined(USE_EINK) || defined(ILI9341_DRIVER) || defined(ILI9342_DRIVER) || defined(ST7701_CS) || defined(ST7735_CS) ||      \
     defined(ST7789_CS) || defined(USE_ST7789) || defined(ILI9488_CS) || defined(HX8357_CS) || defined(ST7796_CS) ||             \
     defined(HACKADAY_COMMUNICATOR) || defined(USE_ST7796)) &&                                                                   \
    !defined(DISPLAY_FORCE_SMALL_FONTS)

    if (currentResolution == ScreenResolution::High) {
        NodeListRenderer::drawScaledXBitmap16x16(x, y - 1, 8, 8, imgUser, display);
    } else {
        display->drawFastImage(x, y + 3, 8, 8, imgUser);
    }
#else
    if (currentResolution == ScreenResolution::High) {
        NodeListRenderer::drawScaledXBitmap16x16(x, y - 1, 8, 8, imgUser, display);
    } else {
#if defined(M5STACK_UNITC6L) || defined(USE_TINY_FONT)
        display->drawFastImage(x, y - 3, 8, 8, imgUser);
#else
        display->drawFastImage(x, y + 1, 8, 8, imgUser);
#endif
    }
#endif
    int string_offset = (currentResolution == ScreenResolution::High) ? 9 : 0;
    display->drawString(x + 10 + string_offset, y - 2, usersString);
}

// **********************
// * Favorite Node Info *
// **********************
void UIRenderer::drawNodeInfo(OLEDDisplay *display, const OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    if (favoritedNodes.empty())
        return;

    // --- Only display if index is valid ---
    int nodeIndex = state->currentFrame - (screen->frameCount - favoritedNodes.size());
    if (nodeIndex < 0 || nodeIndex >= (int)favoritedNodes.size())
        return;

    meshtastic_NodeInfoLite *node = favoritedNodes[nodeIndex];
    if (!node || node->num == nodeDB->getNodeNum() || !node->is_favorite)
        return;

    currentFavoriteNodeNum = node->num;

    display->clear();
#if defined(M5STACK_UNITC6L) || defined(USE_TINY_FONT)
    uint32_t now = millis();
    if (now - lastSwitchTime >= 10000) // 10000 ms = 10 秒
    {
        display->display();
        lastSwitchTime = now;
    }
#endif
    // === Create the shortName and title string ===
    const char *shortName = (node->has_user && haveGlyphs(node->user.short_name)) ? node->user.short_name : "Node";
    char titlestr[32] = {0};
    snprintf(titlestr, sizeof(titlestr), "Fav: %s", shortName);

    // === Draw battery/time/mail header (common across screens) ===
    graphics::drawCommonHeader(display, x, y, titlestr);

    // (Classic small-screen compass is drawn later at the computed compass position)

    // ===== DYNAMIC ROW STACKING WITH YOUR MACROS =====
    // 1. Each potential info row has a macro-defined Y position (not regular increments!).
    // 2. Each row is only shown if it has valid data.
    // 3. Each row "moves up" if previous are empty, so there are never any blank rows.
    // 4. The first line is ALWAYS at your macro position; subsequent lines use the next available macro slot.

    // List of available macro Y positions in order, from top to bottom.
    int line = 1; // which slot to use next
    std::string usernameStr;
    // === 1. Long Name (always try to show first) ===
    const char *username;
#if defined(M5STACK_UNITC6L) || defined(USE_TINY_FONT)
    username = (node->has_user && node->user.long_name[0]) ? node->user.short_name : nullptr;
#else
    if (currentResolution == ScreenResolution::UltraLow) {
        username = (node->has_user && node->user.long_name[0]) ? node->user.short_name : nullptr;
    } else {
        username = (node->has_user && node->user.long_name[0]) ? node->user.long_name : nullptr;
    }
#endif

    if (username) {
        usernameStr = sanitizeString(username); // Sanitize the incoming long_name just in case
        // Print node's long name (e.g. "Backpack Node") with emoji support
        graphics::MessageRenderer::drawStringWithEmotes(display, x, getTextPositions(display)[line++], usernameStr.c_str(),
                                                        emotes, numEmotes);
    }

    // === 2. Signal and Hops (combined on one line, if available) ===
    // If both are present: "Sig: 97%  [2hops]"
    // If only one: show only that one
    char signalHopsStr[32] = "";
    bool haveSignal = false;
    int percentSignal = clamp((int)((node->snr + 10) * 5), 0, 100);

    // Always use "Sig" for the label
    const char *signalLabel = " Sig";

    // --- Build the Signal/Hops line ---
    // If SNR looks reasonable, show signal
    if ((int)((node->snr + 10) * 5) >= 0 && node->snr > -100) {
        snprintf(signalHopsStr, sizeof(signalHopsStr), "%s: %d%%", signalLabel, percentSignal);
        haveSignal = true;
    }
    // If hops is valid (>0), show right after signal
    if (node->hops_away > 0) {
        size_t len = strlen(signalHopsStr);
        // Decide between "1 Hop" and "N Hops"
        if (haveSignal) {
            snprintf(signalHopsStr + len, sizeof(signalHopsStr) - len, " [%d %s]", node->hops_away,
                     (node->hops_away == 1 ? "Hop" : "Hops"));
        } else {
            snprintf(signalHopsStr, sizeof(signalHopsStr), "[%d %s]", node->hops_away, (node->hops_away == 1 ? "Hop" : "Hops"));
        }
    }
    if (signalHopsStr[0] && line < 5) {
        display->drawString(x, getTextPositions(display)[line++], signalHopsStr);
    }

    // === 3. Heard (last seen, skip if node never seen) ===
    char seenStr[20] = "";
    uint32_t seconds = sinceLastSeen(node);
    if (seconds != 0 && seconds != UINT32_MAX) {
        uint32_t minutes = seconds / 60, hours = minutes / 60, days = hours / 24;
        // Format as "Heard: Xm ago", "Heard: Xh ago", or "Heard: Xd ago"
        snprintf(seenStr, sizeof(seenStr), (days > 365 ? " Heard: ?" : " Heard: %d%c ago"),
                 (days    ? days
                  : hours ? hours
                          : minutes),
                 (days    ? 'd'
                  : hours ? 'h'
                          : 'm'));
    }
    if (seenStr[0] && line < 5) {
        display->drawString(x, getTextPositions(display)[line++], seenStr);
    }
#if !defined(M5STACK_UNITC6L) || defined(USE_TINY_FONT)
    // === 4. Uptime (only show if metric is present) ===
    char uptimeStr[32] = "";
    if (node->has_device_metrics && node->device_metrics.has_uptime_seconds) {
        getUptimeStr(node->device_metrics.uptime_seconds * 1000, " Up", uptimeStr, sizeof(uptimeStr));
    }
    if (uptimeStr[0] && line < 5) {
        display->drawString(x, getTextPositions(display)[line++], uptimeStr);
    }

    // === 5. Distance (only if both nodes have GPS position) ===
    meshtastic_NodeInfoLite *ourNode = nodeDB->getMeshNode(nodeDB->getNodeNum());
    char distStr[24] = ""; // Make buffer big enough for any string
    bool haveDistance = false;

    if (nodeDB->hasValidPosition(ourNode) && nodeDB->hasValidPosition(node)) {
        double lat1 = ourNode->position.latitude_i * 1e-7;
        double lon1 = ourNode->position.longitude_i * 1e-7;
        double lat2 = node->position.latitude_i * 1e-7;
        double lon2 = node->position.longitude_i * 1e-7;
        double earthRadiusKm = 6371.0;
        double dLat = (lat2 - lat1) * DEG_TO_RAD;
        double dLon = (lon2 - lon1) * DEG_TO_RAD;
        double a =
            sin(dLat / 2) * sin(dLat / 2) + cos(lat1 * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD) * sin(dLon / 2) * sin(dLon / 2);
        double c = 2 * atan2(sqrt(a), sqrt(1 - a));
        double distanceKm = earthRadiusKm * c;

        if (config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL) {
            double miles = distanceKm * 0.621371;
            if (miles < 0.1) {
                int feet = (int)(miles * 5280);
                if (feet > 0 && feet < 1000) {
                    snprintf(distStr, sizeof(distStr), " Distance: %dft", feet);
                    haveDistance = true;
                } else if (feet >= 1000) {
                    snprintf(distStr, sizeof(distStr), " Distance: ¼mi");
                    haveDistance = true;
                }
            } else {
                int roundedMiles = (int)(miles + 0.5);
                if (roundedMiles > 0 && roundedMiles < 1000) {
                    snprintf(distStr, sizeof(distStr), " Distance: %dmi", roundedMiles);
                    haveDistance = true;
                }
            }
        } else {
            if (distanceKm < 1.0) {
                int meters = (int)(distanceKm * 1000);
                if (meters > 0 && meters < 1000) {
                    snprintf(distStr, sizeof(distStr), " Distance: %dm", meters);
                    haveDistance = true;
                } else if (meters >= 1000) {
                    snprintf(distStr, sizeof(distStr), " Distance: 1km");
                    haveDistance = true;
                }
            } else {
                int km = (int)(distanceKm + 0.5);
                if (km > 0 && km < 1000) {
                    snprintf(distStr, sizeof(distStr), " Distance: %dkm", km);
                    haveDistance = true;
                }
            }
        }
    }
    // Only display if we actually have a value!
    if (haveDistance && distStr[0] && line < 5) {
        display->drawString(x, getTextPositions(display)[line++], distStr);
    }

    // --- Compass Rendering: landscape (wide) screens use the original side-aligned logic ---
    if (SCREEN_WIDTH > SCREEN_HEIGHT) {
        bool showCompass = false;
        if (ourNode && (nodeDB->hasValidPosition(ourNode) || screen->hasHeading()) && nodeDB->hasValidPosition(node)) {
            showCompass = true;
        }
        if (showCompass) {
            const int16_t topY = getTextPositions(display)[1];
            const int16_t bottomY = SCREEN_HEIGHT - (FONT_HEIGHT_SMALL - 1);
            const int16_t usableHeight = bottomY - topY - 5;
            int16_t compassRadius = usableHeight / 2;
            if (compassRadius < 8)
                compassRadius = 8;
            const int16_t compassDiam = compassRadius * 2;
            const int16_t compassX = x + SCREEN_WIDTH - compassRadius - 8;
            const int16_t compassY = topY + (usableHeight / 2) + ((FONT_HEIGHT_SMALL - 1) / 2) + 2;

            const auto &op = ourNode->position;
            float myHeading = screen->hasHeading() ? screen->getHeading() * PI / 180
                                                   : screen->estimatedHeading(DegD(op.latitude_i), DegD(op.longitude_i));

            const auto &p = node->position;
            /* unused
            float d =
                GeoCoord::latLongToMeter(DegD(p.latitude_i), DegD(p.longitude_i), DegD(op.latitude_i), DegD(op.longitude_i));
            */
            // Absolute world bearing (do NOT subtract heading here; the 3D renderer already does -heading)
            float bearingWorld =
                GeoCoord::bearing(DegD(op.latitude_i), DegD(op.longitude_i), DegD(p.latitude_i), DegD(p.longitude_i));
            // If you still draw any legacy 2D bits, you can keep this relative bearing:
            float bearingRel = bearingWorld;
            if (uiconfig.compass_mode != meshtastic_CompassMode_FREEZE_HEADING)
                bearingRel -= myHeading;

            // Elevation (radians)
            int32_t myAltM = ourNode ? ourNode->position.altitude : 0;
            if (myAltM == 0)
                myAltM = geoCoord.getAltitude();
            int32_t tgtAltM = p.altitude;
            float groundM =
                GeoCoord::latLongToMeter(DegD(p.latitude_i), DegD(p.longitude_i), DegD(op.latitude_i), DegD(op.longitude_i));
            float dzM = float(tgtAltM - myAltM);
            float elevRad = (fabsf(groundM) > 0.5f) ? atanf(dzM / groundM) : 0.0f;

            // Render compass based on display resolution and type
#if !defined(USE_EINK)
            if (isHighResolution()) {
                // 3D spherical compass + 3D-aware rim chevron toward favorite node
#if !MESHTASTIC_EXCLUDE_BMI270
                const Quat att = GetAttitudeForRenderer();
#else
                const Quat att = {1, 0, 0, 0}; // Identity quaternion
#endif
                graphics::CompassRenderer::drawNodeHeading(display, compassX, compassY, compassDiam, bearingRel);
                graphics::CompassRenderer::drawCenterNeedle3D(display, compassX, compassY, compassRadius, att, bearingWorld,
                                                              elevRad);
            } else
#endif
            {
                // Classic small-screen compass: circle + 'N' + simple arrow (for TFTs, low-res OLEDs, and e-ink)
                display->drawCircle(compassX, compassY, compassRadius);
                graphics::CompassRenderer::drawCompassNorth(display, compassX, compassY, myHeading, compassRadius);
                graphics::CompassRenderer::drawNodeHeading(display, compassX, compassY, compassDiam, bearingRel);
            }
        }
        // else show nothing
    } else {
        // Portrait or square: put compass at the bottom and centered, scaled to fit available space
        bool showCompass = false;
        if (ourNode && (nodeDB->hasValidPosition(ourNode) || screen->hasHeading()) && nodeDB->hasValidPosition(node)) {
            showCompass = true;
        }
        if (showCompass) {
            int yBelowContent = (line > 0 && line <= 5) ? (getTextPositions(display)[line - 1] + FONT_HEIGHT_SMALL + 2)
                                                        : getTextPositions(display)[1];
            const int margin = 4;
// --------- PATCH FOR EINK NAV BAR (ONLY CHANGE BELOW) -----------
#if defined(USE_EINK)
            const int iconSize = (currentResolution == ScreenResolution::High) ? 16 : 8;
            const int navBarHeight = iconSize + 6;
#else
            const int navBarHeight = 0;
#endif
            int availableHeight = SCREEN_HEIGHT - yBelowContent - navBarHeight - margin;
            // --------- END PATCH FOR EINK NAV BAR -----------

            if (availableHeight < FONT_HEIGHT_SMALL * 2)
                return;

            int compassRadius = availableHeight / 2;
            if (compassRadius < 8)
                compassRadius = 8;
            if (compassRadius * 2 > SCREEN_WIDTH - 16)
                compassRadius = (SCREEN_WIDTH - 16) / 2;

            int compassX = x + SCREEN_WIDTH / 2;
            int compassY = yBelowContent + availableHeight / 2;

            const auto &op = ourNode->position;
            float myHeading = 0;
            if (uiconfig.compass_mode != meshtastic_CompassMode_FREEZE_HEADING) {
                myHeading = screen->hasHeading() ? screen->getHeading() * PI / 180
                                                 : screen->estimatedHeading(DegD(op.latitude_i), DegD(op.longitude_i));
            }

            const auto &p = node->position;
            /* unused
            float d =
                GeoCoord::latLongToMeter(DegD(p.latitude_i), DegD(p.longitude_i), DegD(op.latitude_i), DegD(op.longitude_i));
            */
            float bearingWorld =
                GeoCoord::bearing(DegD(op.latitude_i), DegD(op.longitude_i), DegD(p.latitude_i), DegD(p.longitude_i));
            float bearingRel = bearingWorld;
            if (uiconfig.compass_mode != meshtastic_CompassMode_FREEZE_HEADING)
                bearingRel -= myHeading;

            // --- Elevation angle (radians) for 3D chevron ---
            // My altitude: prefer ourNode->position.altitude, else GeoCoord (same units: meters)
            int32_t myAltM = ourNode ? ourNode->position.altitude : 0;
            if (myAltM == 0) { // fall back to current GeoCoord altitude if set
                myAltM = geoCoord.getAltitude();
            }
            // Target altitude:
            int32_t tgtAltM = p.altitude;
            // Ground range in meters:
            float groundM =
                GeoCoord::latLongToMeter(DegD(p.latitude_i), DegD(p.longitude_i), DegD(op.latitude_i), DegD(op.longitude_i));
            // Elevation angle: +up = positive
            float dzM = float(tgtAltM - myAltM);
            float elevRad = (fabsf(groundM) > 0.5f) ? atanf(dzM / groundM) : 0.0f;

#if !defined(USE_EINK)
            if (isHighResolution()) {
#if !MESHTASTIC_EXCLUDE_BMI270
                const Quat att = GetAttitudeForRenderer();
#else
                const Quat att = {1, 0, 0, 0};
#endif
                graphics::CompassRenderer::drawNodeHeading(display, compassX, compassY, compassRadius * 2, bearingRel);
                graphics::CompassRenderer::drawCenterNeedle3D(display, compassX, compassY, compassRadius, att, bearingWorld,
                                                              elevRad);
            } else
#endif
            {
                // Classic small-screen compass (for TFTs, low-res OLEDs, and e-ink)
                display->drawCircle(compassX, compassY, compassRadius);
                graphics::CompassRenderer::drawCompassNorth(display, compassX, compassY, myHeading, compassRadius);
                graphics::CompassRenderer::drawNodeHeading(display, compassX, compassY, compassRadius * 2, bearingRel);
            }
        }
        // else show nothing
    }
#endif
    graphics::drawCommonFooter(display, x, y);
}

// ****************************
// * Device Focused Screen    *
// ****************************
void UIRenderer::drawDeviceFocused(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    display->clear();
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);
    int line = 1;
    meshtastic_NodeInfoLite *ourNode = nodeDB->getMeshNode(nodeDB->getNodeNum());

    // === Header ===
    if (currentResolution == ScreenResolution::UltraLow) {
        graphics::drawCommonHeader(display, x, y, "Home");
    } else {
        graphics::drawCommonHeader(display, x, y, "");
    }

    // === Content below header ===

    // Determine if we need to show 4 or 5 rows on the screen
    int rows = 4;
    if (!config.bluetooth.enabled) {
        rows = 5;
    }

    // === First Row: Region / Channel Utilization and Uptime ===
    bool origBold = config.display.heading_bold;
    config.display.heading_bold = false;

    // Display Region and Channel Utilization
#if defined(M5STACK_UNITC6L)
    drawNodes(display, x, getTextPositions(display)[line++] + 2, nodeStatus, -1, false, "online");
    // Uptime calculated here but drawn later with channel utilization
    uint32_t uptime = millis() / 1000;
    uint32_t days = uptime / 86400;
    uint32_t hours = (uptime % 86400) / 3600;
    uint32_t mins = (uptime % 3600) / 60;
#else
    if (currentResolution == ScreenResolution::UltraLow) {
        drawNodes(display, x, getTextPositions(display)[line] + 2, nodeStatus, -1, false, "online");
    } else {
        drawNodes(display, x + 1, getTextPositions(display)[line] + 2, nodeStatus, -1, false, "online");
    }
    char uptimeStr[32] = "";
    if (currentResolution != ScreenResolution::UltraLow) {
        getUptimeStr(millis(), "Up", uptimeStr, sizeof(uptimeStr));
    }
    display->drawString(SCREEN_WIDTH - display->getStringWidth(uptimeStr), getTextPositions(display)[line++], uptimeStr);
#endif

    // === Second Row: Satellites and Voltage ===
    config.display.heading_bold = false;

#if HAS_GPS
    if (config.position.gps_mode != meshtastic_Config_PositionConfig_GpsMode_ENABLED) {
        const char *displayLine;
        if (config.position.fixed_position) {
            displayLine = "Fixed GPS";
        } else {
            displayLine = config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_NOT_PRESENT ? "No GPS" : "GPS off";
        }
        drawSatelliteIcon(display, x, getTextPositions(display)[line]);
        int xOffset = (currentResolution == ScreenResolution::High) ? 6 : 0;
        display->drawString(x + 11 + xOffset, getTextPositions(display)[line], displayLine);
    } else {
        UIRenderer::drawGps(display, 0, getTextPositions(display)[line], gpsStatus);
    }
#endif

#if defined(M5STACK_UNITC6L)
    line += 1;

    // === Channel Utilization and Uptime (same line) ===
    char chUtilStr[16];
    snprintf(chUtilStr, sizeof(chUtilStr), "Ch:%2.0f%%", airTime->channelUtilizationPercent());
    display->drawString(x, getTextPositions(display)[line], chUtilStr);

    char uptimeCompactStr[24] = "";
    if (days)
        snprintf(uptimeCompactStr, sizeof(uptimeCompactStr), "Up:%ud%uh", days, hours);
    else if (hours)
        snprintf(uptimeCompactStr, sizeof(uptimeCompactStr), "Up:%uh%um", hours, mins);
    else
        snprintf(uptimeCompactStr, sizeof(uptimeCompactStr), "Up:%um", mins);

    display->drawString(SCREEN_WIDTH - display->getStringWidth(uptimeCompactStr), getTextPositions(display)[line],
                        uptimeCompactStr);
    line += 1;

    // === Node Identity (Long Name + Short Name) ===
    int textWidth = 0;
    int nameX = 0;
    std::string longNameStr;

    if (ourNode && ourNode->has_user && strlen(ourNode->user.long_name) > 0) {
        longNameStr = sanitizeString(ourNode->user.long_name);
    }

    char shortnameble[35];
    snprintf(shortnameble, sizeof(shortnameble), "%s",
             graphics::UIRenderer::haveGlyphs(owner.short_name) ? owner.short_name : "");

    // Show long name if available
    if (!longNameStr.empty()) {
        textWidth = graphics::MessageRenderer::getStringWidthWithEmotes(display, longNameStr, emotes, numEmotes);
        nameX = (SCREEN_WIDTH - textWidth) / 2;
        graphics::MessageRenderer::drawStringWithEmotes(display, nameX, getTextPositions(display)[line++], longNameStr, emotes,
                                                        numEmotes);
    }

    // Show short name on next line
    textWidth = display->getStringWidth(shortnameble);
    nameX = (SCREEN_WIDTH - textWidth) / 2;
    display->drawString(nameX, getTextPositions(display)[line++], shortnameble);
#else
    if (powerStatus->getHasBattery()) {
        char batStr[20];
        int batV = powerStatus->getBatteryVoltageMv() / 1000;
        int batCv = (powerStatus->getBatteryVoltageMv() % 1000) / 10;
        snprintf(batStr, sizeof(batStr), "%01d.%02dV", batV, batCv);
        display->drawString(x + SCREEN_WIDTH - display->getStringWidth(batStr), getTextPositions(display)[line++], batStr);
    } else {
        display->drawString(x + SCREEN_WIDTH - display->getStringWidth("USB"), getTextPositions(display)[line++], "USB");
    }

    config.display.heading_bold = origBold;

    // === Third Row: Channel Utilization Bluetooth Off (Only If Actually Off) ===
    const char *chUtil = "ChUtil:";
    char chUtilPercentage[10];
    snprintf(chUtilPercentage, sizeof(chUtilPercentage), "%2.0f%%", airTime->channelUtilizationPercent());

    int chUtil_x = (currentResolution == ScreenResolution::High) ? display->getStringWidth(chUtil) + 10
                                                                 : display->getStringWidth(chUtil) + 5;
#if defined(USE_TINY_FONT)
    int chUtil_y = getTextPositions(display)[line] - 1;
#else
    int chUtil_y = getTextPositions(display)[line] + 3;
#endif

    int chutil_bar_width = (currentResolution == ScreenResolution::High) ? 100 : 50;
    if (!config.bluetooth.enabled) {
#if defined(USE_EINK)
        chutil_bar_width = (currentResolution == ScreenResolution::High) ? 50 : 30;
#else
        chutil_bar_width = (currentResolution == ScreenResolution::High) ? 80 : 40;
#endif
    }
    int chutil_bar_height = (currentResolution == ScreenResolution::High) ? 12 : 7;
    int extraoffset = (currentResolution == ScreenResolution::High) ? 6 : 3;
    if (!config.bluetooth.enabled) {
        extraoffset = (currentResolution == ScreenResolution::High) ? 6 : 1;
    }
    int chutil_percent = airTime->channelUtilizationPercent();

    int centerofscreen = SCREEN_WIDTH / 2;
    int total_line_content_width = (chUtil_x + chutil_bar_width + display->getStringWidth(chUtilPercentage) + extraoffset) / 2;
    int starting_position = centerofscreen - total_line_content_width;
    if (!config.bluetooth.enabled) {
        starting_position = 0;
    }

    display->drawString(starting_position, getTextPositions(display)[line], chUtil);

    // Force 56% or higher to show a full 100% bar, text would still show related percent.
    if (chutil_percent >= 61) {
        chutil_percent = 100;
    }

    // Weighting for nonlinear segments
    float milestone1 = 25;
    float milestone2 = 40;
    float weight1 = 0.45; // Weight for 0–25%
    float weight2 = 0.35; // Weight for 25–40%
    float weight3 = 0.20; // Weight for 40–100%
    float totalWeight = weight1 + weight2 + weight3;

    int seg1 = chutil_bar_width * (weight1 / totalWeight);
    int seg2 = chutil_bar_width * (weight2 / totalWeight);
    int seg3 = chutil_bar_width * (weight3 / totalWeight);

    int fillRight = 0;

    if (chutil_percent <= milestone1) {
        fillRight = (seg1 * (chutil_percent / milestone1));
    } else if (chutil_percent <= milestone2) {
        fillRight = seg1 + (seg2 * ((chutil_percent - milestone1) / (milestone2 - milestone1)));
    } else {
        fillRight = seg1 + seg2 + (seg3 * ((chutil_percent - milestone2) / (100 - milestone2)));
    }

    // Draw outline
    display->drawRect(starting_position + chUtil_x, chUtil_y, chutil_bar_width, chutil_bar_height);

    // Fill progress
    if (fillRight > 0) {
        display->fillRect(starting_position + chUtil_x, chUtil_y, fillRight, chutil_bar_height);
    }

    display->drawString(starting_position + chUtil_x + chutil_bar_width + extraoffset, getTextPositions(display)[line],
                        chUtilPercentage);

    if (!config.bluetooth.enabled) {
        display->drawString(SCREEN_WIDTH - display->getStringWidth("BT off"), getTextPositions(display)[line], "BT off");
    }

    line += 1;

    // === Fourth & Fifth Rows: Node Identity ===
    int textWidth = 0;
    int nameX = 0;
    int yOffset = (currentResolution == ScreenResolution::High) ? 0 : 5;
    std::string longNameStr;

    if (ourNode && ourNode->has_user && strlen(ourNode->user.long_name) > 0) {
        longNameStr = sanitizeString(ourNode->user.long_name);
    }
    char shortnameble[35];
    snprintf(shortnameble, sizeof(shortnameble), "%s",
             graphics::UIRenderer::haveGlyphs(owner.short_name) ? owner.short_name : "");

    char combinedName[50];
    snprintf(combinedName, sizeof(combinedName), "%s (%s)", longNameStr.empty() ? "" : longNameStr.c_str(), shortnameble);
    if (SCREEN_WIDTH - (display->getStringWidth(combinedName)) > 10) {
        size_t len = strlen(combinedName);
        if (len >= 3 && strcmp(combinedName + len - 3, " ()") == 0) {
            combinedName[len - 3] = '\0'; // Remove the last three characters
        }
        textWidth = display->getStringWidth(combinedName);
        nameX = (SCREEN_WIDTH - textWidth) / 2;
        display->drawString(
            nameX, ((rows == 4) ? getTextPositions(display)[line++] : getTextPositions(display)[line++]) + yOffset, combinedName);
    } else {
        // === LongName Centered ===
        textWidth = graphics::MessageRenderer::getStringWidthWithEmotes(display, longNameStr, emotes, numEmotes);
        nameX = (SCREEN_WIDTH - textWidth) / 2;
        graphics::MessageRenderer::drawStringWithEmotes(display, nameX, getTextPositions(display)[line++], longNameStr, emotes,
                                                        numEmotes);

        // === ShortName Centered ===
        textWidth = display->getStringWidth(shortnameble);
        nameX = (SCREEN_WIDTH - textWidth) / 2;
        display->drawString(nameX, getTextPositions(display)[line++], shortnameble);
    }
#endif
    graphics::drawCommonFooter(display, x, y);
}

// Start Functions to write date/time to the screen
// Helper function to check if a year is a leap year
constexpr bool isLeapYear(int year)
{
    return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

// Array of days in each month (non-leap year)
const int daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

// Fills the buffer with a formatted date/time string and returns pixel width
int UIRenderer::formatDateTime(char *buf, size_t bufSize, uint32_t rtc_sec, OLEDDisplay *display, bool includeTime)
{
    int sec = rtc_sec % 60;
    rtc_sec /= 60;
    int min = rtc_sec % 60;
    rtc_sec /= 60;
    int hour = rtc_sec % 24;
    rtc_sec /= 24;

    int year = 1970;
    while (true) {
        int daysInYear = isLeapYear(year) ? 366 : 365;
        if (rtc_sec >= (uint32_t)daysInYear) {
            rtc_sec -= daysInYear;
            year++;
        } else {
            break;
        }
    }

    int month = 0;
    while (month < 12) {
        int dim = daysInMonth[month];
        if (month == 1 && isLeapYear(year))
            dim++;
        if (rtc_sec >= (uint32_t)dim) {
            rtc_sec -= dim;
            month++;
        } else {
            break;
        }
    }

    int day = rtc_sec + 1;

    if (includeTime) {
        snprintf(buf, bufSize, "%04d-%02d-%02d %02d:%02d:%02d", year, month + 1, day, hour, min, sec);
    } else {
        snprintf(buf, bufSize, "%04d-%02d-%02d", year, month + 1, day);
    }

    return display->getStringWidth(buf);
}

// Check if the display can render a string (detect special chars; emoji)
bool UIRenderer::haveGlyphs(const char *str)
{
#if defined(OLED_PL) || defined(OLED_UA) || defined(OLED_RU) || defined(OLED_CS)
    // Don't want to make any assumptions about custom language support
    return true;
#endif

    // Check each character with the lookup function for the OLED library
    // We're not really meant to use this directly..
    bool have = true;
    for (uint16_t i = 0; i < strlen(str); i++) {
        uint8_t result = Screen::customFontTableLookup((uint8_t)str[i]);
        // If font doesn't support a character, it is substituted for ¿
        if (result == 191 && (uint8_t)str[i] != 191) {
            have = false;
            break;
        }
    }

    // LOG_DEBUG("haveGlyphs=%d", have);
    return have;
}

#ifdef USE_EINK
/// Used on eink displays while in deep sleep
void UIRenderer::drawDeepSleepFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{

    // Next frame should use full-refresh, and block while running, else device will sleep before async callback
    EINK_ADD_FRAMEFLAG(display, COSMETIC);
    EINK_ADD_FRAMEFLAG(display, BLOCKING);

    LOG_DEBUG("Draw deep sleep screen");

    // Display displayStr on the screen
    graphics::UIRenderer::drawIconScreen("Sleeping", display, state, x, y);
}

/// Used on eink displays when screen updates are paused
void UIRenderer::drawScreensaverOverlay(OLEDDisplay *display, OLEDDisplayUiState *state)
{
    LOG_DEBUG("Draw screensaver overlay");

    EINK_ADD_FRAMEFLAG(display, COSMETIC); // Full refresh for screensaver

    // Config
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    const char *pauseText = "Screen Paused";
    const char *idText = owner.short_name;
    const bool useId = haveGlyphs(idText);
    constexpr uint8_t padding = 2;
    constexpr uint8_t dividerGap = 1;

    // Text widths
    const uint16_t idTextWidth = display->getStringWidth(idText, strlen(idText), true);
    const uint16_t pauseTextWidth = display->getStringWidth(pauseText, strlen(pauseText));
    const uint16_t boxWidth = padding + (useId ? idTextWidth + padding : 0) + pauseTextWidth + padding;
    const uint16_t boxHeight = FONT_HEIGHT_SMALL + (padding * 2);

    // Flush with bottom
    const int16_t boxLeft = (display->width() / 2) - (boxWidth / 2);
    const int16_t boxTop = display->height() - boxHeight;
    const int16_t boxBottom = display->height() - 1;
    const int16_t idTextLeft = boxLeft + padding;
    const int16_t idTextTop = boxTop + padding;
    const int16_t pauseTextLeft = boxLeft + (useId ? idTextWidth + (padding * 2) : 0) + padding;
    const int16_t pauseTextTop = boxTop + padding;
    const int16_t dividerX = boxLeft + padding + idTextWidth + padding;
    const int16_t dividerTop = boxTop + dividerGap;
    const int16_t dividerBottom = boxBottom - dividerGap;

    // Draw: box
    display->setColor(EINK_WHITE);
    display->fillRect(boxLeft, boxTop, boxWidth, boxHeight);
    display->setColor(EINK_BLACK);
    display->drawRect(boxLeft, boxTop, boxWidth, boxHeight);

    // Draw: text
    if (useId)
        display->drawString(idTextLeft, idTextTop, idText);
    display->drawString(pauseTextLeft, pauseTextTop, pauseText);
    display->drawString(pauseTextLeft + 1, pauseTextTop, pauseText); // Faux bold

    // Draw: divider
    if (useId)
        display->drawLine(dividerX, dividerTop, dividerX, dividerBottom);
}
#endif

/**
 * Draw the icon with extra info printed around the corners
 */
void UIRenderer::drawIconScreen(const char *upperMsg, OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    // draw an xbm image.
    // Please note that everything that should be transitioned
    // needs to be drawn relative to x and y

    // draw centered icon left to right and centered above the one line of app text
#if defined(M5STACK_UNITC6L) || defined(USE_TINY_FONT)
    display->drawXbm(x + (SCREEN_WIDTH - 50) / 2, y + (SCREEN_HEIGHT - FONT_HEIGHT_SMALL - icon_height) / 2 + 1, icon_width,
                     icon_height, icon_bits);
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);

    // Draw meshtastic.org at bottom
    const char *title = "meshtastic.org";
    display->drawString(x + getStringCenteredX(title), y + SCREEN_HEIGHT - FONT_HEIGHT_SMALL, title);

    // Draw region in upper left
    if (upperMsg)
        display->drawString(x + 0, y + 0, upperMsg);

    // Draw version and short name in upper right
    char buf[25];
    snprintf(buf, sizeof(buf), "%s\n%s", xstr(APP_VERSION_SHORT),
             graphics::UIRenderer::haveGlyphs(owner.short_name) ? owner.short_name : "");

    display->setTextAlignment(TEXT_ALIGN_RIGHT);
    display->drawString(x + SCREEN_WIDTH, y + 0, buf);
    screen->forceDisplay();

    display->setTextAlignment(TEXT_ALIGN_LEFT); // Restore left align, just to be kind to any other unsuspecting code
#else
    display->drawXbm(x + (SCREEN_WIDTH - icon_width) / 2, y + (SCREEN_HEIGHT - FONT_HEIGHT_MEDIUM - icon_height) / 2 + 2,
                     icon_width, icon_height, icon_bits);

    display->setFont(FONT_MEDIUM);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    const char *title = "meshtastic.org";
    display->drawString(x + getStringCenteredX(title), y + SCREEN_HEIGHT - FONT_HEIGHT_MEDIUM, title);
    display->setFont(FONT_SMALL);
    // Draw region in upper left
    if (upperMsg)
        display->drawString(x + 0, y + 0, upperMsg);

    // Draw version and short name in upper right
    char buf[25];
    snprintf(buf, sizeof(buf), "%s\n%s", xstr(APP_VERSION_SHORT),
             graphics::UIRenderer::haveGlyphs(owner.short_name) ? owner.short_name : "");

    display->setTextAlignment(TEXT_ALIGN_RIGHT);
    display->drawString(x + SCREEN_WIDTH, y + 0, buf);
    screen->forceDisplay();

    display->setTextAlignment(TEXT_ALIGN_LEFT); // Restore left align, just to be kind to any other unsuspecting code
#endif
}

// ****************************
// * My Position Screen       *
// ****************************
void UIRenderer::drawCompassAndLocationScreen(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    display->clear();
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);
    int line = 1;

    // === Set Title
    const char *titleStr = "Position";

    // === Header ===
    graphics::drawCommonHeader(display, x, y, titleStr);

    // === First Row: My Location ===
#if HAS_GPS
    bool origBold = config.display.heading_bold;
    config.display.heading_bold = false;

    const char *displayLine = ""; // Initialize to empty string by default

    if (config.position.gps_mode != meshtastic_Config_PositionConfig_GpsMode_ENABLED) {
        if (config.position.fixed_position) {
            displayLine = "Fixed GPS";
        } else {
            displayLine = config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_NOT_PRESENT ? "No GPS" : "GPS off";
        }
        drawSatelliteIcon(display, x, getTextPositions(display)[line]);
        int xOffset = (currentResolution == ScreenResolution::High) ? 6 : 0;
        display->drawString(x + 11 + xOffset, getTextPositions(display)[line++], displayLine);
    } else {
        // Onboard GPS
        UIRenderer::drawGps(display, 0, getTextPositions(display)[line++], gpsStatus);
    }

    config.display.heading_bold = origBold;

    // === Update GeoCoord ===
    geoCoord.updateCoords(int32_t(gpsStatus->getLatitude()), int32_t(gpsStatus->getLongitude()),
                          int32_t(gpsStatus->getAltitude()));

    // === Determine Compass Heading ===
    float heading = 0;
    static float frozenHeading = 0; // Store the heading when freeze mode was activated
    static bool hasStoredFrozenHeading = false;
    static meshtastic_CompassMode lastCompassMode = (meshtastic_CompassMode)-1; // Force initial detection
    bool validHeading = false;

    // Always get current heading for compass ring rotation
    if (screen->hasHeading()) {
        heading = radians(screen->getHeading());
        validHeading = true;
    } else {
        heading = screen->estimatedHeading(geoCoord.getLatitude() * 1e-7, geoCoord.getLongitude() * 1e-7);
        validHeading = !isnan(heading);
    }

    // Handle freeze heading mode - capture current heading when mode is first activated
    if (uiconfig.compass_mode != lastCompassMode) {
        // Mode changed, reset frozen heading state
        hasStoredFrozenHeading = false;
        lastCompassMode = uiconfig.compass_mode;
    }

    if (uiconfig.compass_mode == meshtastic_CompassMode_FREEZE_HEADING) {
        if (!hasStoredFrozenHeading && validHeading) {
            frozenHeading = heading; // Capture current heading
            hasStoredFrozenHeading = true;
        }
    }

    // Determine needle heading based on mode
    float needleHeading = (uiconfig.compass_mode == meshtastic_CompassMode_FREEZE_HEADING) ? frozenHeading : heading;

    // If GPS is off, no need to display these parts
    if (strcmp(displayLine, "GPS off") != 0 && strcmp(displayLine, "No GPS") != 0) {
        // === Second Row: Last GPS Fix ===
        if (gpsStatus->getLastFixMillis() > 0) {
            uint32_t delta = millis() - gpsStatus->getLastFixMillis();
            char uptimeStr[32];
#if defined(USE_EINK)
            // E-Ink: skip seconds, show only days/hours/mins
            getUptimeStr(delta, "Last", uptimeStr, sizeof(uptimeStr), false);
#else
            // Non E-Ink: include seconds where useful
            getUptimeStr(delta, "Last", uptimeStr, sizeof(uptimeStr), true);
#endif

            display->drawString(0, getTextPositions(display)[line++], uptimeStr);
        } else {
            display->drawString(0, getTextPositions(display)[line++], "Last: ?");
        }

        // === Third Row: Line 1 GPS Info ===
        UIRenderer::drawGpsCoordinates(display, x, getTextPositions(display)[line++], gpsStatus, "line1");

        if (uiconfig.gps_format != meshtastic_DeviceUIConfig_GpsCoordinateFormat_OLC &&
            uiconfig.gps_format != meshtastic_DeviceUIConfig_GpsCoordinateFormat_MLS) {
            // === Fourth Row: Line 2 GPS Info ===
            UIRenderer::drawGpsCoordinates(display, x, getTextPositions(display)[line++], gpsStatus, "line2");
        }

        // === Final Row: Altitude ===
        char altitudeLine[32] = {0};
        int32_t alt = geoCoord.getAltitude();
        if (config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL) {
            snprintf(altitudeLine, sizeof(altitudeLine), "Alt: %.0fft", alt * METERS_TO_FEET);
        } else {
            snprintf(altitudeLine, sizeof(altitudeLine), "Alt: %.0im", alt);
        }
        display->drawString(x, getTextPositions(display)[line++], altitudeLine);
    }
#if !defined(M5STACK_UNITC6L) || defined(USE_TINY_FONT)
    // === Draw Compass if heading is valid ===
    if (validHeading) {
        // --- Compass Rendering: landscape (wide) screens use original side-aligned logic ---
        if (SCREEN_WIDTH > SCREEN_HEIGHT) {
            const int16_t topY = getTextPositions(display)[1];
            const int16_t bottomY = SCREEN_HEIGHT - (FONT_HEIGHT_SMALL - 1); // nav row height
            const int16_t usableHeight = bottomY - topY - 5;

            int16_t compassRadius = usableHeight / 2;
            if (compassRadius < 8)
                compassRadius = 8;
            const int16_t compassDiam = compassRadius * 2;
            const int16_t compassX = x + SCREEN_WIDTH - compassRadius - 8;

            // Center vertically and nudge down slightly to keep "N" clear of header
            const int16_t compassY = topY + (usableHeight / 2) + ((FONT_HEIGHT_SMALL - 1) / 2) + 2;

            // Add step count display above compass (top right area)
#if !MESHTASTIC_EXCLUDE_BMI270
            uint32_t stepCount = GetStepCountForRenderer();
            if (HasStepCounterForRenderer()) { // Show if step counter hardware exists
                display->setTextAlignment(TEXT_ALIGN_RIGHT);
                display->setFont(FONT_SMALL);

                // Position step display in top right, below header
                const int16_t stepX = SCREEN_WIDTH - 2;
                const int16_t stepY = topY - 2;

                // Format step count (show as K if > 1000)
                char stepText[16];
                if (stepCount >= 10000) {
                    snprintf(stepText, sizeof(stepText), "%.1fK", stepCount / 1000.0f);
                } else if (stepCount >= 1000) {
                    snprintf(stepText, sizeof(stepText), "%.2fK", stepCount / 1000.0f);
                } else {
                    snprintf(stepText, sizeof(stepText), "%u", stepCount);
                }

                // Calculate text width to position footprints to the left of the digits
                int16_t textWidth = display->getStringWidth(stepText);

                // Draw footprint icon to the left of the text with some spacing
                drawFootprintIcon(display, stepX - textWidth - 15, stepY + 2);

                display->drawString(stepX, stepY, stepText);
                display->setTextAlignment(TEXT_ALIGN_LEFT); // Reset alignment
            }
#endif

            // Compass with mode-specific behavior
#if !MESHTASTIC_EXCLUDE_BMI270
            const Quat att = GetAttitudeForRenderer();
#else
            const Quat att = {1, 0, 0, 0};
#endif

            // Render compass sphere with mode-specific attitude
            if (uiconfig.compass_mode == meshtastic_CompassMode_FIXED_RING) {
                // FIXED_RING: Use runtime-detected compass (3D for OLED, simple for TFT/e-ink)
                CompassRenderer::setTopDownView(true);
                CompassRenderer::drawCompassSphere(display, compassX, compassY, compassRadius);

#if !defined(USE_EINK)
                if (isHighResolution()) {
                    // High-res OLED: show fixed cardinal labels around the ring
                    const uint16_t rDraw = (uint16_t)std::max<int>(1, (int)(compassRadius));
                    const int16_t cxShift = (int16_t)(compassX - (int)(rDraw * 0.14f));
                    const int16_t cy = compassY;
                    const float rLabel = rDraw * 1.06f;

                    display->setFont(FONT_SMALL);
                    display->setTextAlignment(TEXT_ALIGN_CENTER);
                    display->drawString(cxShift, cy - (int)rLabel - (FONT_HEIGHT_SMALL / 2), "N");
                    display->drawString(cxShift + (int)rLabel, cy - (FONT_HEIGHT_SMALL / 2), "E");
                    display->drawString(cxShift, cy + (int)rLabel - (FONT_HEIGHT_SMALL / 2), "S");
                    display->drawString(cxShift - (int)rLabel, cy - (FONT_HEIGHT_SMALL / 2), "W");

                    CompassRenderer::drawCenterNeedle3D(display, compassX, compassY, compassRadius, Quat::identity(),
                                                        needleHeading, 0.0f);
                } else
#endif
                {
                    // Low-res/e-ink: match develop visuals — no fixed E/N/S/W labels, static 'N' at top, frozen needle if
                    // selected
                    display->setFont(FONT_SMALL);
                    display->setTextAlignment(TEXT_ALIGN_CENTER);
                    int16_t nX = compassX;
                    int16_t nY = compassY - (compassRadius - 1);
                    display->drawString(nX, nY - (FONT_HEIGHT_SMALL / 2), "N");
                    // Use needleHeading so FREEZE_HEADING is respected
                    CompassRenderer::drawNodeHeading(display, compassX, compassY, compassDiam, -needleHeading);
                }

                CompassRenderer::setTopDownView(false);
            } else {
                // DYNAMIC/FREEZE_HEADING
#if !defined(USE_EINK)
                if (isHighResolution()) {
                    // Normal 3D compass with gravity
                    CompassRenderer::drawNodeHeading(display, compassX, compassY, compassDiam, -heading);
                    CompassRenderer::drawCenterNeedle3D(display, compassX, compassY, compassRadius, att, needleHeading, 0.0f);
                } else
#endif
                {
                    // Classic small-screen/e-ink compass: circle + 'N' + simple arrow
                    display->drawCircle(compassX, compassY, compassRadius);
                    display->setFont(FONT_SMALL);
                    display->setTextAlignment(TEXT_ALIGN_CENTER);
                    // North rotates with live heading unless FIXED_RING; needle may be frozen per mode
                    float northAngle = (uiconfig.compass_mode != meshtastic_CompassMode_FIXED_RING) ? -heading : 0.0f;
                    int16_t nX = compassX + (int16_t)((compassRadius - 1) * sinf(northAngle));
                    int16_t nY = compassY - (int16_t)((compassRadius - 1) * cosf(northAngle));
                    display->drawString(nX, nY - (FONT_HEIGHT_SMALL / 2), "N");
                    // Use needleHeading so FREEZE_HEADING affects the arrow on low-res too
                    CompassRenderer::drawNodeHeading(display, compassX, compassY, compassDiam, -needleHeading);
                }
            }

        } else {
            // Portrait or square: put compass at the bottom and centered, scaled to fit available space
            // For E-Ink screens, account for navigation bar at the bottom!
            int yBelowContent = getTextPositions(display)[5] + FONT_HEIGHT_SMALL + 2;
            const int margin = 4;
            int availableHeight =
#if defined(USE_EINK)
                SCREEN_HEIGHT - yBelowContent - 24; // Leave extra space for nav bar on E-Ink
#else
                SCREEN_HEIGHT - yBelowContent - margin;
#endif

            if (availableHeight < FONT_HEIGHT_SMALL * 2)
                return;

                // Add step count display in top right corner for portrait/square screens
#if !MESHTASTIC_EXCLUDE_BMI270
            uint32_t stepCount = GetStepCountForRenderer();
            if (HasStepCounterForRenderer()) {
                display->setTextAlignment(TEXT_ALIGN_RIGHT);
                display->setFont(FONT_SMALL);

                const int16_t stepX = SCREEN_WIDTH - 2;
                const int16_t stepY = getTextPositions(display)[1];

                char stepText[16];
                if (stepCount >= 10000) {
                    snprintf(stepText, sizeof(stepText), "%.1fK", stepCount / 1000.0f);
                } else if (stepCount >= 1000) {
                    snprintf(stepText, sizeof(stepText), "%.2fK", stepCount / 1000.0f);
                } else {
                    snprintf(stepText, sizeof(stepText), "%u", stepCount);
                }

                int16_t textWidth = display->getStringWidth(stepText);
                drawFootprintIcon(display, stepX - textWidth - 15, stepY + 2);
                display->drawString(stepX, stepY, stepText);

#ifdef HAS_BHI260AP_SENSORLIB
                // Show IMU data under step counter for t-echo-plus
                float ax, ay, az, gx, gy, gz;
                if (GetBHI260APDataForRenderer(&ax, &ay, &az, &gx, &gy, &gz)) {
                    char imuText[32];
                    // Show accel on one line
                    snprintf(imuText, sizeof(imuText), "A:%.1f,%.1f,%.1f", ax, ay, az);
                    display->drawString(stepX, stepY + 14, imuText);
                    // Show gyro on next line
                    snprintf(imuText, sizeof(imuText), "G:%.1f,%.1f,%.1f", gx, gy, gz);
                    display->drawString(stepX, stepY + 28, imuText);
                }
#endif

                display->setTextAlignment(TEXT_ALIGN_LEFT);
            }
#endif

            int compassRadius = availableHeight / 2;
            if (compassRadius < 8)
                compassRadius = 8;
            if (compassRadius * 2 > SCREEN_WIDTH - 16)
                compassRadius = (SCREEN_WIDTH - 16) / 2;

            int compassX = x + SCREEN_WIDTH / 2;
            int compassY = yBelowContent + availableHeight / 2;

            // Compass with mode-specific behavior
#if !MESHTASTIC_EXCLUDE_BMI270
            const Quat att = GetAttitudeForRenderer();
#else
            const Quat att = {1, 0, 0, 0};
#endif

            // Render compass sphere with mode-specific attitude
            if (uiconfig.compass_mode == meshtastic_CompassMode_FIXED_RING) {
                // FIXED_RING: Render 3D compass from top-down view (ignoring gravity)
                CompassRenderer::setTopDownView(true);
                CompassRenderer::drawCompassSphere(display, compassX, compassY, compassRadius, Quat::identity());

#if !defined(USE_EINK)
                if (isHighResolution()) {
                    // High-res OLED: show fixed cardinal direction labels
                    const uint16_t rDraw = (uint16_t)std::max<int>(1, (int)(compassRadius));
                    const int16_t cxShift = (int16_t)(compassX - (int)(rDraw * 0.14f));
                    const int16_t cy = compassY;
                    const float rLabel = rDraw * 1.06f;

                    display->setFont(FONT_SMALL);
                    display->setTextAlignment(TEXT_ALIGN_CENTER);
                    display->drawString(cxShift, cy - (int)rLabel - (FONT_HEIGHT_SMALL / 2), "N");
                    display->drawString(cxShift + (int)rLabel, cy - (FONT_HEIGHT_SMALL / 2), "E");
                    display->drawString(cxShift, cy + (int)rLabel - (FONT_HEIGHT_SMALL / 2), "S");
                    display->drawString(cxShift - (int)rLabel, cy - (FONT_HEIGHT_SMALL / 2), "W");

                    CompassRenderer::drawCenterNeedle3D(display, compassX, compassY, compassRadius, Quat::identity(),
                                                        needleHeading, 0.0f);
                } else
#endif
                {
                    // Low-res/e-ink: match develop visuals — no fixed E/N/S/W labels, static 'N', frozen needle if selected
                    display->setFont(FONT_SMALL);
                    display->setTextAlignment(TEXT_ALIGN_CENTER);
                    int16_t nX = compassX;
                    int16_t nY = compassY - (compassRadius - 1);
                    display->drawString(nX, nY - (FONT_HEIGHT_SMALL / 2), "N");
                    // Use needleHeading so FREEZE_HEADING is respected
                    CompassRenderer::drawNodeHeading(display, compassX, compassY, compassRadius * 2, -needleHeading);
                }

                CompassRenderer::setTopDownView(false);
            } else {
                // DYNAMIC/FREEZE_HEADING
#if !defined(USE_EINK)
                if (isHighResolution()) {
                    // Normal 3D compass with gravity
                    CompassRenderer::drawNodeHeading(display, compassX, compassY, compassRadius * 2, -heading);
                    CompassRenderer::drawCenterNeedle3D(display, compassX, compassY, compassRadius, att, needleHeading, 0.0f);
                } else
#endif
                {
                    // Classic small-screen/e-ink compass
                    display->drawCircle(compassX, compassY, compassRadius);
                    display->setFont(FONT_SMALL);
                    display->setTextAlignment(TEXT_ALIGN_CENTER);
                    // North rotates with live heading unless FIXED_RING
                    float northAngle = (uiconfig.compass_mode != meshtastic_CompassMode_FIXED_RING) ? -heading : 0.0f;
                    int16_t nX = compassX + (int16_t)((compassRadius - 1) * sinf(northAngle));
                    int16_t nY = compassY - (int16_t)((compassRadius - 1) * cosf(northAngle));
                    display->drawString(nX, nY - (FONT_HEIGHT_SMALL / 2), "N");
                    // Use needleHeading to honor FREEZE_HEADING on low-res
                    CompassRenderer::drawNodeHeading(display, compassX, compassY, compassRadius * 2, -needleHeading);
                }
            }
        }
    }
#endif
#endif // HAS_GPS
    graphics::drawCommonFooter(display, x, y);
}

#if defined(M5STACK_UNITC6L)
// ****************************
// * Compass-Only Screen      *
// ****************************
void UIRenderer::drawCompassScreen(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    display->clear();

#if HAS_GPS
    // === Determine Compass Heading ===
    float heading = 0;
    static float frozenHeading = 0;
    static bool hasStoredFrozenHeading = false;
    static meshtastic_CompassMode lastCompassMode = (meshtastic_CompassMode)-1;
    bool validHeading = false;

    // Get current heading
    if (screen->hasHeading()) {
        heading = radians(screen->getHeading());
        validHeading = true;
    } else {
        meshtastic_NodeInfoLite *ourNode = nodeDB->getMeshNode(nodeDB->getNodeNum());
        if (ourNode) {
            double lat = DegD(ourNode->position.latitude_i);
            double lon = DegD(ourNode->position.longitude_i);
            heading = screen->estimatedHeading(lat, lon);
            validHeading = !isnan(heading);
        }
    }

    // Handle freeze heading mode
    if (uiconfig.compass_mode != lastCompassMode) {
        hasStoredFrozenHeading = false;
        lastCompassMode = uiconfig.compass_mode;
    }

    if (uiconfig.compass_mode == meshtastic_CompassMode_FREEZE_HEADING) {
        if (!hasStoredFrozenHeading && validHeading) {
            frozenHeading = heading;
            hasStoredFrozenHeading = true;
        }
    }

    // Determine needle heading based on mode
    float needleHeading = (uiconfig.compass_mode == meshtastic_CompassMode_FREEZE_HEADING) ? frozenHeading : heading;

    if (validHeading) {
        // Calculate compass position - maximize compass size with labels inside
        int availableHeight = SCREEN_HEIGHT - 4;
        int availableWidth = SCREEN_WIDTH - 4;

        // Use smaller dimension for compass radius
        int compassRadius = std::min(availableWidth, availableHeight) / 2;
        if (compassRadius < 16)
            compassRadius = 16;

        // Center horizontally and vertically (shifted 4 pixels right)
        int compassX = x + SCREEN_WIDTH / 2 + 4;
        int compassY = y + (SCREEN_HEIGHT / 2);

        // Get attitude quaternion for 3D compass
#if !MESHTASTIC_EXCLUDE_BMI270
        const Quat att = GetAttitudeForRenderer();
#else
        const Quat att = {1, 0, 0, 0};
#endif

        // Render 3D compass sphere with mode-specific behavior
        if (uiconfig.compass_mode == meshtastic_CompassMode_FIXED_RING) {
            // FIXED_RING: Use top-down view (ignore gravity tilt)
            CompassRenderer::setTopDownView(true);
            CompassRenderer::drawCompassSphere(display, compassX, compassY, compassRadius, Quat::identity());

            // Draw needle respecting freeze mode
            CompassRenderer::drawCenterNeedle3D(display, compassX, compassY, compassRadius, Quat::identity(), needleHeading,
                                                0.0f);
            CompassRenderer::setTopDownView(false);
        } else {
            // DYNAMIC/FREEZE_HEADING: Full 3D compass with gravity
            CompassRenderer::drawCompassSphere(display, compassX, compassY, compassRadius, att);

            // Draw needle (uses needleHeading so freeze mode is respected)
            CompassRenderer::drawCenterNeedle3D(display, compassX, compassY, compassRadius, att, needleHeading, 0.0f);
        }

        // Draw cardinal direction labels on the edge of the compass with black background boxes
        const float rLabel = compassRadius - (FONT_HEIGHT_SMALL / 2) - 1; // Position on the edge
        const int boxWidth = 5;
        const int boxHeight = 7;

        display->setFont(FONT_SMALL);
        display->setTextAlignment(TEXT_ALIGN_CENTER);

        // Draw "N" with black background box (shifted 2 pixels left)
        int nX = compassX - 2;
        int nY = compassY - (int)rLabel - (FONT_HEIGHT_SMALL / 2);
        display->setColor(BLACK);
        display->fillRect(nX - boxWidth / 2 - 1, nY - 1, boxWidth, boxHeight);
        display->setColor(WHITE);
        display->drawString(nX, nY, "N");

        // Draw "E" with black background box (shifted 2 pixels left)
        int eX = compassX + (int)rLabel - 2;
        int eY = compassY - (FONT_HEIGHT_SMALL / 2);
        display->setColor(BLACK);
        display->fillRect(eX - boxWidth / 2 - 1, eY - 1, boxWidth, boxHeight);
        display->setColor(WHITE);
        display->drawString(eX, eY, "E");

        // Draw "S" with black background box (shifted 2 pixels left)
        int sX = compassX - 2;
        int sY = compassY + (int)rLabel - (FONT_HEIGHT_SMALL / 2);
        display->setColor(BLACK);
        display->fillRect(sX - boxWidth / 2 - 1, sY - 1, boxWidth, boxHeight);
        display->setColor(WHITE);
        display->drawString(sX, sY, "S");

        // Draw "W" with black background box (shifted 2 pixels left)
        int wX = compassX - (int)rLabel - 2;
        int wY = compassY - (FONT_HEIGHT_SMALL / 2);
        display->setColor(BLACK);
        display->fillRect(wX - boxWidth / 2 - 1, wY - 1, boxWidth, boxHeight);
        display->setColor(WHITE);
        display->drawString(wX, wY, "W");
    } else {
        // No valid heading - show message
        display->setFont(FONT_SMALL);
        display->setTextAlignment(TEXT_ALIGN_CENTER);
        display->drawString(x + SCREEN_WIDTH / 2, y + SCREEN_HEIGHT / 2 - FONT_HEIGHT_SMALL / 2, "No Heading");
    }
#endif // HAS_GPS

    // Draw footer only (no header to maximize compass size)
    graphics::drawCommonFooter(display, x, y);
}
#endif // M5STACK_UNITC6L

#ifdef USERPREFS_OEM_TEXT

void UIRenderer::drawOEMIconScreen(const char *upperMsg, OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    static const uint8_t xbm[] = USERPREFS_OEM_IMAGE_DATA;
    if (currentResolution == ScreenResolution::High) {
        display->drawXbm(x + (SCREEN_WIDTH - USERPREFS_OEM_IMAGE_WIDTH) / 2,
                         y + (SCREEN_HEIGHT - FONT_HEIGHT_MEDIUM - USERPREFS_OEM_IMAGE_HEIGHT) / 2 + 2, USERPREFS_OEM_IMAGE_WIDTH,
                         USERPREFS_OEM_IMAGE_HEIGHT, xbm);
    } else {

        display->drawXbm(x + (SCREEN_WIDTH - USERPREFS_OEM_IMAGE_WIDTH) / 2,
                         y + (SCREEN_HEIGHT - USERPREFS_OEM_IMAGE_HEIGHT) / 2 + 2, USERPREFS_OEM_IMAGE_WIDTH,
                         USERPREFS_OEM_IMAGE_HEIGHT, xbm);
    }

    switch (USERPREFS_OEM_FONT_SIZE) {
    case 0:
        display->setFont(FONT_SMALL);
        break;
    case 2:
        display->setFont(FONT_LARGE);
        break;
    default:
        display->setFont(FONT_MEDIUM);
        break;
    }

    display->setTextAlignment(TEXT_ALIGN_LEFT);
    const char *title = USERPREFS_OEM_TEXT;
    if (currentResolution == ScreenResolution::High) {
        display->drawString(x + getStringCenteredX(title), y + SCREEN_HEIGHT - FONT_HEIGHT_MEDIUM, title);
    }
    display->setFont(FONT_SMALL);

    // Draw region in upper left
    if (upperMsg)
        display->drawString(x + 0, y + 0, upperMsg);

    // Draw version and shortname in upper right
    char buf[25];
    snprintf(buf, sizeof(buf), "%s\n%s", xstr(APP_VERSION_SHORT), haveGlyphs(owner.short_name) ? owner.short_name : "");

    display->setTextAlignment(TEXT_ALIGN_RIGHT);
    display->drawString(x + SCREEN_WIDTH, y + 0, buf);
    screen->forceDisplay();

    display->setTextAlignment(TEXT_ALIGN_LEFT); // Restore left align, just to be kind to any other unsuspecting code
}

void UIRenderer::drawOEMBootScreen(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    // Draw region in upper left
    const char *region = myRegion ? myRegion->name : NULL;
    drawOEMIconScreen(region, display, state, x, y);
}

#endif

// Navigation bar overlay implementation
static int8_t lastFrameIndex = -1;
static uint32_t lastFrameChangeTime = 0;
constexpr uint32_t ICON_DISPLAY_DURATION_MS = 2000;

void UIRenderer::drawNavigationBar(OLEDDisplay *display, OLEDDisplayUiState *state)
{
    int currentFrame = state->currentFrame;

    // Detect frame change and record time
    if (currentFrame != lastFrameIndex) {
        lastFrameIndex = currentFrame;
        lastFrameChangeTime = millis();

        // Update screen-active flags based on current frame index
        const int msgIdx = graphics::getMessagesFrameIndex();
        graphics::setMessagesScreenActive(state->currentFrame == msgIdx);

        const int envIdx = graphics::getEnvTelemetryFrameIndex();
        graphics::setEnvTelemetryScreenActive(state->currentFrame == envIdx);
    }

    const int iconSize = (currentResolution == ScreenResolution::High) ? 16 : 8;
    const int spacing = (currentResolution == ScreenResolution::High) ? 8 : 4;
    const int bigOffset = (currentResolution == ScreenResolution::High) ? 1 : 0;

    const size_t totalIcons = screen->indicatorIcons.size();
    if (totalIcons == 0)
        return;

    const int navPadding = (currentResolution == ScreenResolution::High) ? 24 : 12; // padding per side

    int usableWidth = SCREEN_WIDTH - (navPadding * 2);
    if (usableWidth < iconSize)
        usableWidth = iconSize;

    const size_t iconsPerPage = usableWidth / (iconSize + spacing);
    const size_t currentPage = currentFrame / iconsPerPage;
    const size_t pageStart = currentPage * iconsPerPage;
    const size_t pageEnd = min(pageStart + iconsPerPage, totalIcons);

    const int totalWidth = (pageEnd - pageStart) * iconSize + (pageEnd - pageStart - 1) * spacing;
    const int xStart = (SCREEN_WIDTH - totalWidth) / 2;

    bool navBarVisible = millis() - lastFrameChangeTime <= ICON_DISPLAY_DURATION_MS;
    int y = navBarVisible ? (SCREEN_HEIGHT - iconSize - 1) : SCREEN_HEIGHT;

#if defined(USE_EINK)
    // Only show bar briefly after switching frames
    static uint32_t navBarLastShown = 0;
    static bool cosmeticRefreshDone = false;
    static bool navBarPrevVisible = false;

    if (navBarVisible && !navBarPrevVisible) {
        EINK_ADD_FRAMEFLAG(display, DEMAND_FAST); // Fast refresh when showing nav bar
        cosmeticRefreshDone = false;
        navBarLastShown = millis();
    }

    if (!navBarVisible && navBarPrevVisible) {
        EINK_ADD_FRAMEFLAG(display, DEMAND_FAST); // Fast refresh when hiding nav bar
        navBarLastShown = millis();               // Mark when it disappeared
    }

    if (!navBarVisible && navBarLastShown != 0 && !cosmeticRefreshDone) {
        if (millis() - navBarLastShown > 10000) {  // 10s after hidden
            EINK_ADD_FRAMEFLAG(display, COSMETIC); // One-time ghost cleanup
            cosmeticRefreshDone = true;
        }
    }

    navBarPrevVisible = navBarVisible;
#endif

    // Pre-calculate bounding rect
    const int rectX = xStart - 2 - bigOffset;
    const int rectWidth = totalWidth + 4 + (bigOffset * 2);
    const int rectHeight = iconSize + 6;

    // Clear background and draw border
    display->setColor(BLACK);
    display->fillRect(rectX + 1, y - 2, rectWidth - 2, rectHeight - 2);
    display->setColor(WHITE);
    display->drawRect(rectX, y - 2, rectWidth, rectHeight);

    // Icon drawing loop for the current page
    for (size_t i = pageStart; i < pageEnd; ++i) {
        const uint8_t *icon = screen->indicatorIcons[i];
        const int x = xStart + (i - pageStart) * (iconSize + spacing);
        const bool isActive = (i == static_cast<size_t>(currentFrame));

        if (isActive) {
            display->setColor(WHITE);
            display->fillRect(x - 2, y - 2, iconSize + 4, iconSize + 4);
            display->setColor(BLACK);
        }

        if (currentResolution == ScreenResolution::High) {
            NodeListRenderer::drawScaledXBitmap16x16(x, y, 8, 8, icon, display);
        } else {
            display->drawXbm(x, y, iconSize, iconSize, icon);
        }

        if (isActive) {
            display->setColor(WHITE);
        }
    }

    // Compact arrow drawer
    auto drawArrow = [&](bool rightSide) {
        display->setColor(WHITE);

        const int offset = (currentResolution == ScreenResolution::High) ? 3 : 1;
        const int halfH = rectHeight / 2;

        const int top = (y - 2) + (rectHeight - halfH) / 2;
        const int bottom = top + halfH - 1;
        const int midY = top + (halfH / 2);

        const int maxW = 4;

        // Determine left X coordinate
        int baseX = rightSide ? (rectX + rectWidth + offset) : // right arrow
                        (rectX - offset - 1);                  // left arrow

        for (int yy = top; yy <= bottom; yy++) {
            int dist = abs(yy - midY);
            int lineW = maxW - (dist * maxW / (halfH / 2));
            if (lineW < 1)
                lineW = 1;

            if (rightSide) {
                display->drawHorizontalLine(baseX, yy, lineW);
            } else {
                display->drawHorizontalLine(baseX - lineW + 1, yy, lineW);
            }
        }
    };
    // Right arrow
    if (pageEnd < totalIcons) {
        drawArrow(true);
    }

    // Left arrow
    if (pageStart > 0) {
        drawArrow(false);
    }

    // Knock the corners off the square
    display->setColor(BLACK);
    display->drawRect(rectX, y - 2, 1, 1);
    display->drawRect(rectX + rectWidth - 1, y - 2, 1, 1);
    display->setColor(WHITE);
}

void UIRenderer::drawFrameText(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y, const char *message)
{
    uint16_t x_offset = display->width() / 2;
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    display->setFont(FONT_MEDIUM);
    display->drawString(x_offset + x, 26 + y, message);
}

std::string UIRenderer::drawTimeDelta(uint32_t days, uint32_t hours, uint32_t minutes, uint32_t seconds)
{
    std::string uptime;

    if (days > (HOURS_IN_MONTH * 6))
        uptime = "?";
    else if (days >= 2)
        uptime = std::to_string(days) + "d";
    else if (hours >= 2)
        uptime = std::to_string(hours) + "h";
    else if (minutes >= 1)
        uptime = std::to_string(minutes) + "m";
    else
        uptime = std::to_string(seconds) + "s";
    return uptime;
}

} // namespace graphics

#endif // HAS_SCREEN
