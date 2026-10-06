#include "configuration.h"
#if HAS_SCREEN
#include "../Screen.h"
#include "DebugRenderer.h"
#include "FSCommon.h"
#include "MenuHandler.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "UIRenderer.h"
#include "airtime.h"
#include "gps/RTC.h"
#include "graphics/ScreenFonts.h"
#include "graphics/SharedUIDisplay.h"
#include "graphics/TFTColorRegions.h"
#include "graphics/TFTPalette.h"
#include "graphics/TimeFormatters.h"
#include "graphics/images.h"
#include "main.h"
#include "mesh/Throttle.h"
#if defined(ARCH_ESP32) && defined(HAS_SDCARD) && !defined(SDCARD_USE_SOFT_SPI) && !defined(HAS_SD_MMC)
#include "SPILock.h"
#include <SD.h>
#include <ff.h>
#define SYSTEM_SHOWS_SD_USAGE 1

// SDFS keeps its FatFs drive number protected; a member pointer taken through a subclass reads it legally.
struct SDDrive : fs::SDFS {
    static uint8_t of(const fs::SDFS &sd) { return sd.*(&SDDrive::_pdrv); }
};
#elif defined(SENSECAP_INDICATOR)
// The card is the RP2040's: its statistics come from the state the co-processor caches, without touching the card.
#include "mesh/IndicatorSerial.h"
#define SYSTEM_SHOWS_SD_USAGE 1
#define SYSTEM_SD_REMOTE 1
#endif

#if HAS_WIFI && !defined(ARCH_PORTDUINO)
#include "mesh/wifi/WiFiAPClient.h"
#include <WiFi.h>
#ifdef ARCH_ESP32
#include "mesh/wifi/WiFiAPClient.h"
#endif
#if HAS_ETHERNET && defined(ETH_SHARED_SPI)
#include "platform/esp32/SharedBusEthernet.h"
#endif
#if HAS_ETHERNET && defined(USE_CH390D)
#include "ESP32_CH390.h"
#define ETH CH390
#endif
#endif

#include <DisplayFormatters.h>
#include <RadioLibInterface.h>
#include <target_specific.h>

using namespace meshtastic;

// External variables
extern std::unique_ptr<graphics::Screen> screen;
extern NodeStatus *nodeStatus;
extern AirTime *airTime;

// External functions from Screen.cpp
extern bool heartbeat;

namespace graphics
{
namespace DebugRenderer
{

#if HAS_WIFI && !defined(ARCH_PORTDUINO)
static void drawWiFiStatus(OLEDDisplay *display, int16_t x, int16_t y, int &line)
{
    const char *wifiName = config.network.wifi_ssid;

    if (WiFi.status() != WL_CONNECTED) {
        display->drawString(x + BASEUI_BODY_LR_MARGIN, getTextPositions(display)[line++] + y, "WiFi: Not Connected");
    } else {
        display->drawString(x + BASEUI_BODY_LR_MARGIN, getTextPositions(display)[line++] + y, "WiFi: Connected");

        char rssiStr[32];
        snprintf(rssiStr, sizeof(rssiStr), "RSSI: %d", WiFi.RSSI());
        display->drawString(x + BASEUI_BODY_LR_MARGIN, getTextPositions(display)[line++] + y, rssiStr);
    }

    /*
    - WL_CONNECTED: assigned when connected to a WiFi network;
    - WL_NO_SSID_AVAIL: assigned when no SSID are available;
    - WL_CONNECT_FAILED: assigned when the connection fails for all the attempts;
    - WL_CONNECTION_LOST: assigned when the connection is lost;
    - WL_DISCONNECTED: assigned when disconnected from a network;
    - WL_IDLE_STATUS: it is a temporary status assigned when WiFi.begin() is called and remains active until the number of
    attempts expires (resulting in WL_CONNECT_FAILED) or a connection is established (resulting in WL_CONNECTED);
    - WL_SCAN_COMPLETED: assigned when the scan networks is completed;
    - WL_NO_SHIELD: assigned when no WiFi shield is present;

    */
    if (WiFi.status() == WL_CONNECTED) {
        char ipStr[64];
        snprintf(ipStr, sizeof(ipStr), "IP: %s", WiFi.localIP().toString().c_str());
        display->drawString(x + BASEUI_BODY_LR_MARGIN, getTextPositions(display)[line++] + y, ipStr);
    } else if (WiFi.status() == WL_NO_SSID_AVAIL) {
        display->drawString(x + BASEUI_BODY_LR_MARGIN, getTextPositions(display)[line++] + y, "SSID Not Found");
    } else if (WiFi.status() == WL_CONNECTION_LOST) {
        display->drawString(x + BASEUI_BODY_LR_MARGIN, getTextPositions(display)[line++] + y, "Connection Lost");
    } else if (WiFi.status() == WL_IDLE_STATUS) {
        display->drawString(x + BASEUI_BODY_LR_MARGIN, getTextPositions(display)[line++] + y, "Idle ... Reconnecting");
    } else if (WiFi.status() == WL_CONNECT_FAILED) {
        display->drawString(x + BASEUI_BODY_LR_MARGIN, getTextPositions(display)[line++] + y, "Connection Failed");
    }
#ifdef ARCH_ESP32
    else {
        // Codes:
        // https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/wifi.html#wi-fi-reason-code
        display->drawString(x + BASEUI_BODY_LR_MARGIN, getTextPositions(display)[line++] + y,
                            WiFi.disconnectReasonName(static_cast<wifi_err_reason_t>(getWifiDisconnectReason())));
    }
#else
    else {
        char statusStr[32];
        snprintf(statusStr, sizeof(statusStr), "Unknown status: %d", WiFi.status());
        display->drawString(x + BASEUI_BODY_LR_MARGIN, getTextPositions(display)[line++] + y, statusStr);
    }
#endif

    char ssidStr[64];
    snprintf(ssidStr, sizeof(ssidStr), "SSID: %s", wifiName);
    display->drawString(x + BASEUI_BODY_LR_MARGIN, getTextPositions(display)[line++] + y, ssidStr);
}
#endif

// ****************************
// * WiFi Screen              *
// ****************************
void drawFrameWiFi(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
#if HAS_WIFI && !defined(ARCH_PORTDUINO)
#if BASEUI_WIFI_MANAGER
    menuHandler::pollWifiScan(); // the Networks menu's scan, once it is in
#endif
    clearForFrame(display, state);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);
    int line = 1;

    bool showEth = false;
#if defined(USE_WS5500) || defined(USE_CH390D)
    // Same condition initWifi() uses to bring WiFi up
    const bool wifiConfigured = config.network.wifi_enabled && config.network.wifi_ssid[0];
    // Prefer Ethernet when it has link or WiFi is not in use
    showEth = config.network.eth_enabled && (ETH.linkUp() || !wifiConfigured);
#endif

    // === Set Title
    const char *titleStr = showEth ? "Ethernet" : "WiFi";

    // === Header ===
    graphics::drawCommonHeader(display, x, y, titleStr);
    y += BASEUI_BELOW_HEADER_MARGIN;

    if (showEth) {
#if defined(USE_WS5500) || defined(USE_CH390D)
        const uint32_t ip = (uint32_t)ETH.localIP();
        if (!ETH.linkUp()) {
            display->drawString(x + BASEUI_BODY_LR_MARGIN, getTextPositions(display)[line++] + y, "Ethernet: No Link");
        } else {
            display->drawString(x + BASEUI_BODY_LR_MARGIN, getTextPositions(display)[line++] + y,
                                ip ? "Ethernet: Connected" : "Ethernet: Waiting for IP");

            char linkStr[32];
            snprintf(linkStr, sizeof(linkStr), "Link: %d Mbps %s", (int)ETH.linkSpeed(), ETH.fullDuplex() ? "Full" : "Half");
            display->drawString(x + BASEUI_BODY_LR_MARGIN, getTextPositions(display)[line++] + y, linkStr);
        }
        if (ip) {
            char ipStr[64];
            snprintf(ipStr, sizeof(ipStr), "IP: %s", ETH.localIP().toString().c_str());
            display->drawString(x + BASEUI_BODY_LR_MARGIN, getTextPositions(display)[line++] + y, ipStr);
        }
        if (wifiConfigured) {
            char wifiStr[64];
            if (WiFi.status() == WL_CONNECTED)
                snprintf(wifiStr, sizeof(wifiStr), "WiFi: %s", WiFi.localIP().toString().c_str());
            else
                snprintf(wifiStr, sizeof(wifiStr), "WiFi: Not Connected");
            display->drawString(x + BASEUI_BODY_LR_MARGIN, getTextPositions(display)[line++] + y, wifiStr);
        }
#endif
    } else {
        drawWiFiStatus(display, x, y, line);
    }

    display->drawString(x + BASEUI_BODY_LR_MARGIN, getTextPositions(display)[line++] + y, "URL: http://meshtastic.local");

    /* Display a heartbeat pixel that blinks every time the frame is redrawn */
#ifdef SHOW_REDRAWS
    if (heartbeat)
        display->setPixel(0, 0);
    heartbeat = !heartbeat;
#endif
#endif
}

// ****************************
// * LoRa Focused Screen      *
// ****************************
void drawLoRaFocused(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    clearForFrame(display, state);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);
    int line = 1;

    // === Set Title
    const char *titleStr = (currentResolution == ScreenResolution::High) ? "LoRa Info" : "LoRa";

    // === Header ===
    graphics::drawCommonHeader(display, x, y, titleStr);
    y += BASEUI_BELOW_HEADER_MARGIN;

    // === First Row: Region / BLE Name ===
    graphics::UIRenderer::drawNodes(display, x + BASEUI_BODY_LR_MARGIN, getTextPositions(display)[line] + 2 + y, nodeStatus, 0,
                                    true, "");

    uint8_t dmac[6];
    char shortnameble[35];
    getMacAddr(dmac);
    snprintf(screen->ourId, sizeof(screen->ourId), "%02x%02x", dmac[4], dmac[5]);
    if (currentResolution == ScreenResolution::UltraLow) {
        snprintf(shortnameble, sizeof(shortnameble), "%s", screen->ourId);
    } else {
        snprintf(shortnameble, sizeof(shortnameble), "BLE: %s", screen->ourId);
    }
    int textWidth = display->getStringWidth(shortnameble);
    int nameX = x + (SCREEN_WIDTH - textWidth - BASEUI_BODY_LR_MARGIN);
    display->drawString(nameX, getTextPositions(display)[line++] + y, shortnameble);

    if (!graphics::isCompactPanel(display)) {
        // === Second Row: Role ===
        auto role = DisplayFormatters::getDeviceRole(config.device.role);
        char device_role[25];
        snprintf(device_role, sizeof(device_role), "Role: %s", role);
        textWidth = display->getStringWidth(device_role);
        nameX = x + (SCREEN_WIDTH - textWidth) / 2;
        display->drawString(nameX, getTextPositions(display)[line++] + y, device_role);
    }

    // === Third Row: Radio Preset ===
    // For custom modem settings show the actual parameters; for presets use the preset name.
    char modeStr[16];
    if (!config.lora.use_preset) {
        snprintf(modeStr, sizeof(modeStr), "BW%u-SF%u-CR%u", static_cast<unsigned>(config.lora.bandwidth),
                 static_cast<unsigned>(config.lora.spread_factor), static_cast<unsigned>(config.lora.coding_rate));
    } else {
        strncpy(modeStr, DisplayFormatters::getModemPresetDisplayName(config.lora.modem_preset, false, true),
                sizeof(modeStr) - 1);
        modeStr[sizeof(modeStr) - 1] = '\0';
    }

    char regionradiopreset[25];
    const char *region = myRegion ? myRegion->name : NULL;
    if (region != nullptr) {
        if (currentResolution == ScreenResolution::UltraLow) {
            snprintf(regionradiopreset, sizeof(regionradiopreset), "%s", region);
        } else {
            snprintf(regionradiopreset, sizeof(regionradiopreset), "%s/%s", region, modeStr);
        }
    }
    textWidth = display->getStringWidth(regionradiopreset);
    nameX = x + (SCREEN_WIDTH - textWidth) / 2;
    display->drawString(nameX, getTextPositions(display)[line++] + y, regionradiopreset);

    // === Fourth Row: Frequency / ChanNum ===
    char frequencyslot[35];
    char freqStr[16];
    float freq = RadioLibInterface::instance->getFreq();
    snprintf(freqStr, sizeof(freqStr), "%.3f", freq);
    if (config.lora.channel_num == 0) {
        if (currentResolution == ScreenResolution::UltraLow) {
            snprintf(frequencyslot, sizeof(frequencyslot), "%sMHz", freqStr);
        } else {
            snprintf(frequencyslot, sizeof(frequencyslot), "Freq: %sMHz", freqStr);
        }
    } else {
        if (currentResolution == ScreenResolution::UltraLow) {
            snprintf(frequencyslot, sizeof(frequencyslot), "%sMHz (%d)", freqStr, config.lora.channel_num);
        } else {
            snprintf(frequencyslot, sizeof(frequencyslot), "Freq: %sMHz (%d)", freqStr, config.lora.channel_num);
        }
    }
    size_t len = strlen(frequencyslot);
    if (len >= 4 && strcmp(frequencyslot + len - 4, " (0)") == 0) {
        frequencyslot[len - 4] = '\0'; // Remove the last three characters
    }
    textWidth = display->getStringWidth(frequencyslot);
    nameX = x + (SCREEN_WIDTH - textWidth) / 2;
    display->drawString(nameX, getTextPositions(display)[line++] + y, frequencyslot);

#if !defined(OLED_TINY)
    // === Fifth Row: Channel Utilization ===
    if (!config.lora.tx_enabled) {
        const char *txdisabled = "Transmit Disabled";
        textWidth = display->getStringWidth(txdisabled);
        display->drawString((SCREEN_WIDTH - textWidth) / 2, getTextPositions(display)[line] + y, txdisabled);
    } else {

        const char *chUtil = "ChUtil:";
        char chUtilPercentage[10];
        snprintf(chUtilPercentage, sizeof(chUtilPercentage), "%2.0f%%", airTime->channelUtilizationPercent());

        int chUtil_x = (currentResolution == ScreenResolution::High) ? display->getStringWidth(chUtil) + 10
                                                                     : display->getStringWidth(chUtil) + 5;
        int chUtil_y = getTextPositions(display)[line] + 3 + y;

        int chutil_bar_width = (currentResolution == ScreenResolution::High) ? 100 : 50;
        int chutil_bar_max_fill = chutil_bar_width - 2; // Account for border
        int chutil_bar_height = (currentResolution == ScreenResolution::High) ? 12 : 7;
        int extraoffset = (currentResolution == ScreenResolution::High) ? 6 : 3;
        int chutil_percent = airTime->channelUtilizationPercent();
        const int raw_chutil_percent = chutil_percent;

        int centerofscreen = x + SCREEN_WIDTH / 2;
        int total_line_content_width =
            (chUtil_x + chutil_bar_width + display->getStringWidth(chUtilPercentage) + extraoffset) / 2;
        int starting_position = centerofscreen - total_line_content_width;

        display->drawString(starting_position, getTextPositions(display)[line] + y, chUtil);

        // Force 61% or higher to show a full 100% bar, text would still show related percent.
        if (chutil_percent >= 61) {
            chutil_percent = 100;
        }

        // Weighting for nonlinear segments
        float milestone1 = 25;
        float milestone2 = 40;
        float weight1 = 0.45; // Weight for 0-25%
        float weight2 = 0.35; // Weight for 25-40%
        float weight3 = 0.20; // Weight for 40-100%
        float totalWeight = weight1 + weight2 + weight3;

        int seg1 = chutil_bar_max_fill * (weight1 / totalWeight);
        int seg2 = chutil_bar_max_fill * (weight2 / totalWeight);
        int seg3 = chutil_bar_max_fill - seg1 - seg2; // Remainder absorbs rounding errors

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
#if GRAPHICS_TFT_COLORING_ENABLED
            uint16_t UtilizationFillColor = TFTPalette::Good;
            if (raw_chutil_percent >= 60) {
                UtilizationFillColor = TFTPalette::Bad;
            } else if (raw_chutil_percent >= 35) {
                UtilizationFillColor = TFTPalette::Medium;
            }
            setAndRegisterTFTColorRole(TFTColorRole::UtilizationFill, UtilizationFillColor, TFTPalette::Black,
                                       starting_position + chUtil_x + 1, chUtil_y + 1, fillRight, chutil_bar_height - 2);
#endif
            display->fillRect(starting_position + chUtil_x + 1, chUtil_y + 1, fillRight, chutil_bar_height - 2);
        }

        display->drawString(starting_position + chUtil_x + chutil_bar_width + extraoffset, getTextPositions(display)[line++] + y,
                            chUtilPercentage);
    }
#endif
}

// ****************************
// *      System Screen       *
// ****************************
// How often the flash usage bar re-reads the filesystem. Nothing here changes faster than the user
// can act, so a slow sample costs nothing visible.
#define SYSTEM_FLASH_USAGE_INTERVAL_MS (10 * 1000)

void drawSystemScreen(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    clearForFrame(display, state);
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);

    // === Set Title
    const char *titleStr = "System";

    // === Header ===
    graphics::drawCommonHeader(display, x, y, titleStr);
    y += BASEUI_BELOW_HEADER_MARGIN;

    // === Layout ===
    int line = 1;
    const int barHeight = 6;
    const int labelX = x + BASEUI_BODY_LR_MARGIN;
    int barsOffset = (currentResolution == ScreenResolution::High) ? 24 : 0;
#ifdef USE_EINK
#ifndef T_DECK_PRO
    barsOffset -= 12;
#endif
#if defined(T5_S3_EPAPER_PRO)
    barsOffset += 60;
#endif
#endif
    int barX = x + barsOffset;
    if (currentResolution == ScreenResolution::UltraLow) {
        barX += 45;
    } else {
        barX += 40;
    }
    // 64-bit with a selectable unit: an SD card's byte count overflows 32 bits, and its KB count overflows the label.
    auto drawUsageRow = [&](const char *label, uint64_t used, uint64_t total, const char *unit = "KB",
                            uint32_t unitBytes = 1024) {
        if (total == 0)
            return;

        if (used > total)
            used = total;
        int percent = (int)((used * 100) / total);

        char combinedStr[24];
        if (currentResolution == ScreenResolution::High) {
            snprintf(combinedStr, sizeof(combinedStr), "%s%3d%%  %u/%u%s", (percent > 80) ? "! " : "", percent,
                     (unsigned)(used / unitBytes), (unsigned)(total / unitBytes), unit);
        } else {
            snprintf(combinedStr, sizeof(combinedStr), "%s%3d%%", (percent > 80) ? "! " : "", percent);
        }

        int textWidth = display->getStringWidth(combinedStr);
        int labelWidth = display->getStringWidth(label);
        if (barX < x + BASEUI_BODY_LR_MARGIN + labelWidth) {
            barX = x + BASEUI_BODY_LR_MARGIN + labelWidth;
        }
        int adjustedBarWidth = x + SCREEN_WIDTH - barX - textWidth - 6 - BASEUI_BODY_LR_MARGIN;
        if (adjustedBarWidth < 10)
            adjustedBarWidth = 10;

        int fillWidth = (int)((used * adjustedBarWidth) / total);

        // Label
        display->setTextAlignment(TEXT_ALIGN_LEFT);
        display->drawString(labelX, getTextPositions(display)[line] + y, label);
#if !defined(OLED_TINY)
        // Bar
        int barY = getTextPositions(display)[line] + y + (FONT_HEIGHT_SMALL - barHeight) / 2;
        display->setColor(WHITE);
        display->drawRect(barX, barY, adjustedBarWidth, barHeight);

#if GRAPHICS_TFT_COLORING_ENABLED
        uint16_t UtilizationFillColor = TFTPalette::Good;
        if (percent >= 80) {
            UtilizationFillColor = TFTPalette::Bad;
        } else if (percent >= 60) {
            UtilizationFillColor = TFTPalette::Medium;
        }
        setAndRegisterTFTColorRole(TFTColorRole::UtilizationFill, UtilizationFillColor, TFTPalette::Black, barX + 1, barY + 1,
                                   fillWidth - 1, barHeight - 2);
#endif

        display->fillRect(barX, barY, fillWidth, barHeight);
        display->setColor(WHITE);
#endif
        // Value string
        display->setTextAlignment(TEXT_ALIGN_RIGHT);
        display->drawString(x + SCREEN_WIDTH - BASEUI_BODY_LR_MARGIN, getTextPositions(display)[line] + y, combinedStr);
    };

    // === Memory values ===
    uint32_t heapUsed = memGet.getHeapSize() - memGet.getFreeHeap();
    uint32_t heapTotal = memGet.getHeapSize();

    uint32_t flashUsed = 0, flashTotal = 0;
#ifdef ESP32
#ifndef T5_S3_EPAPER_PRO
    uint32_t psramUsed = memGet.getPsramSize() - memGet.getFreePsram();
    uint32_t psramTotal = memGet.getPsramSize();
#endif
    // Both of these call esp_littlefs_info(), which traverses every block in the filesystem to
    // total it up. Run per draw that was slow enough to visibly drag this frame's redraw - and the
    // transition into it - so sample on an interval and reuse the last answer in between.
    static uint32_t flashSampledAtMs = 0;
    static uint32_t flashUsedCached = 0, flashTotalCached = 0;
    if (flashTotalCached == 0 || Throttle::hasElapsed(flashSampledAtMs, SYSTEM_FLASH_USAGE_INTERVAL_MS)) {
        flashUsedCached = FSCom.usedBytes();
        flashTotalCached = FSCom.totalBytes();
        flashSampledAtMs = millis();
    }
    flashUsed = flashUsedCached;
    flashTotal = flashTotalCached;
#endif

    uint64_t sdUsed = 0, sdTotal = 0;
    const char *sdLabel = "SD:";
#if SYSTEM_SHOWS_SD_USAGE
    // setupSDCard() mounts once at boot, so a card absent then stays absent. f_getfree() can walk the FAT; throttle it too.
    static uint32_t sdSampledAtMs = 0;
    static uint64_t sdUsedCached = 0, sdTotalCached = 0;
    static const char *sdFsLabel = nullptr;
    static bool sdSampled = false;
    if (!sdSampled || Throttle::hasElapsed(sdSampledAtMs, SYSTEM_FLASH_USAGE_INTERVAL_MS)) {
        sdTotalCached = sdUsedCached = 0;
        sdFsLabel = nullptr;
#if defined(SYSTEM_SD_REMOTE)
        // used/free arrive once the co-processor's background scan of the card is done; until then, no row.
        meshtastic_SdCardInfo info = meshtastic_SdCardInfo_init_zero;
        if (sensecapIndicator && sensecapIndicator->sd_info(&info) && info.present && info.stats_valid) {
            sdTotalCached = info.used_bytes + info.free_bytes;
            sdUsedCached = info.used_bytes;
            switch (info.fat_type) {
            case meshtastic_SdCardInfo_FatType_FAT16:
                sdFsLabel = "SD FAT16:";
                break;
            case meshtastic_SdCardInfo_FatType_FAT32:
                sdFsLabel = "SD FAT32:";
                break;
            case meshtastic_SdCardInfo_FatType_EXFAT:
                sdFsLabel = "SD exFAT:";
                break;
            default:
                break;
            }
        }
#else
        concurrency::LockGuard g(spiLock);
        FATFS *fatfs = nullptr;
        DWORD freeClusters = 0;
        const char drv[3] = {(char)('0' + SDDrive::of(SD)), ':', 0};
        if (SD.cardType() != CARD_NONE && f_getfree(drv, &freeClusters, &fatfs) == FR_OK && fatfs) {
            // Only fs_type, the first field: FATFS's later layout hangs on ffconf options the prebuilt lib may not share.
            switch (fatfs->fs_type) {
            case FS_FAT12:
                sdFsLabel = "SD FAT12:";
                break;
            case FS_FAT16:
                sdFsLabel = "SD FAT16:";
                break;
            case FS_FAT32:
                sdFsLabel = "SD FAT32:";
                break;
            case FS_EXFAT:
                sdFsLabel = "SD exFAT:";
                break;
            }
            sdTotalCached = SD.totalBytes();
            sdUsedCached = SD.usedBytes();
        }
#endif
        sdSampledAtMs = millis();
        sdSampled = true;
    }
    sdUsed = sdUsedCached;
    sdTotal = sdTotalCached;
    if (currentResolution != ScreenResolution::UltraLow && sdFsLabel)
        sdLabel = sdFsLabel;
#endif
    // Make room for the SD label up front so every bar starts at the same x.
    if (sdTotal > 0 && barX < labelX + display->getStringWidth(sdLabel))
        barX = labelX + display->getStringWidth(sdLabel);

    // === Draw memory rows
    drawUsageRow("Heap:", heapUsed, heapTotal);
#ifdef ESP32
#ifndef T5_S3_EPAPER_PRO
    if (psramUsed > 0) {
        line += 1;
        drawUsageRow("PSRAM:", psramUsed, psramTotal);
    }
#endif
    if (flashTotal > 0) {
        line += 1;
        drawUsageRow("Flash:", flashUsed, flashTotal);
    }
#endif
    if (sdTotal > 0) {
        line += 1;
        drawUsageRow(sdLabel, sdUsed, sdTotal, "MB", 1024 * 1024);
    }

    display->setTextAlignment(TEXT_ALIGN_LEFT);
    // System Uptime
    if (graphics::isCompactPanel(display)) {
        line += 1;
    } else {
        if (line < 2) {
            line += 1;
        }
        line += 1;
    }

    char appversionstr[35];
    char appversionstr_formatted[40];

    const char *ver = optstr(APP_VERSION);
    char verbuf[32];
    strncpy(verbuf, ver, sizeof(verbuf) - 1);
    verbuf[sizeof(verbuf) - 1] = '\0';

    char *lastDot = strrchr(verbuf, '.');

    if (currentResolution == ScreenResolution::UltraLow) {
        if (lastDot != nullptr) {
            *lastDot = '\0';
        }
        snprintf(appversionstr, sizeof(appversionstr), "Ver: %s", verbuf);
    } else {
        if (lastDot) {
            size_t prefixLen = (size_t)(lastDot - verbuf);
            snprintf(appversionstr_formatted, sizeof(appversionstr_formatted), "Ver: %.*s", (int)prefixLen, verbuf);
            strncat(appversionstr_formatted, " (", sizeof(appversionstr_formatted) - strlen(appversionstr_formatted) - 1);
            strncat(appversionstr_formatted, lastDot + 1, sizeof(appversionstr_formatted) - strlen(appversionstr_formatted) - 1);
            strncat(appversionstr_formatted, ")", sizeof(appversionstr_formatted) - strlen(appversionstr_formatted) - 1);
            strncpy(appversionstr, appversionstr_formatted, sizeof(appversionstr) - 1);
            appversionstr[sizeof(appversionstr) - 1] = '\0';
        } else {
            snprintf(appversionstr, sizeof(appversionstr), "Ver: %s", verbuf);
        }
    }
    int textWidth = display->getStringWidth(appversionstr);
    int nameX = x + (SCREEN_WIDTH - textWidth) / 2;

    display->drawString(nameX, getTextPositions(display)[line++] + y, appversionstr);

    if (!graphics::isCompactPanel(display) &&
        (SCREEN_HEIGHT > 64 || (SCREEN_HEIGHT <= 64 && line <= 5))) { // Only show uptime if the screen can show it
        char uptimeStr[32] = "";
        getUptimeStr(millis(), "Up: ", uptimeStr, sizeof(uptimeStr));
        textWidth = display->getStringWidth(uptimeStr);
        nameX = x + (SCREEN_WIDTH - textWidth) / 2;
        display->drawString(nameX, getTextPositions(display)[line++] + y, uptimeStr);
    }

    if (SCREEN_HEIGHT > 64 || (SCREEN_HEIGHT <= 64 && line <= 5)) { // Only show API state if the screen can show it
        char api_state[32] = "";
#if defined(OLED_COMPACT_UI)
        const char *connection = "None";
        if (service->api_state == service->STATE_BLE) {
            connection = "BLE";
        } else if (service->api_state == service->STATE_WIFI) {
            connection = "WiFi";
        } else if (service->api_state == service->STATE_SERIAL) {
            connection = "USB";
        } else if (service->api_state == service->STATE_PACKET) {
            connection = "Local";
        } else if (service->api_state == service->STATE_HTTP) {
            connection = "HTTP";
        } else if (service->api_state == service->STATE_ETH) {
            connection = "Eth";
        }
        snprintf(api_state, sizeof(api_state), "App: %s", connection);
#else
        const char *clientWord = nullptr;

        // Determine if narrow or wide screen
        if (currentResolution == ScreenResolution::High) {
            clientWord = "Client";
        } else {
            clientWord = "App";
        }
        snprintf(api_state, sizeof(api_state), "No %ss Connected", clientWord);

        if (service->api_state == service->STATE_BLE) {
            snprintf(api_state, sizeof(api_state), "%s Connected (BLE)", clientWord);
        } else if (service->api_state == service->STATE_WIFI) {
            snprintf(api_state, sizeof(api_state), "%s Connected (WiFi)", clientWord);
        } else if (service->api_state == service->STATE_SERIAL) {
            snprintf(api_state, sizeof(api_state), "%s Connected (Serial)", clientWord);
        } else if (service->api_state == service->STATE_PACKET) {
            snprintf(api_state, sizeof(api_state), "%s Connected (Internal)", clientWord);
        } else if (service->api_state == service->STATE_HTTP) {
            snprintf(api_state, sizeof(api_state), "%s Connected (HTTP)", clientWord);
        } else if (service->api_state == service->STATE_ETH) {
            snprintf(api_state, sizeof(api_state), "%s Connected (Ethernet)", clientWord);
        }
#endif
        // With PSRAM, Flash and SD rows this lands past the 7-entry position table; continue its spacing if it fits.
        const int *pos = getTextPositions(display);
        const int rowY = (line <= 6) ? pos[line] : pos[6] + (line - 6) * (pos[6] - pos[5]);
        if (api_state[0] != '\0' && rowY + FONT_HEIGHT_SMALL <= SCREEN_HEIGHT) {
            display->drawString(x + (SCREEN_WIDTH - display->getStringWidth(api_state)) / 2, rowY + y, api_state);
            line++;
        }
    }
}

// ****************************
// * Chirpy Screen      *
// ****************************
void drawChirpy(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    clearForFrame(display, state);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);
    int line = 1;
    int scale = BASEUI_ICON_SCALE;
    int textX_offset = 10;
    if (currentResolution == ScreenResolution::High) {
        textX_offset = textX_offset * 4;
        scale = 2 * BASEUI_ICON_SCALE;
    }
    const int iconX = x + SCREEN_WIDTH - (chirpy_width * scale) - ((chirpy_width * scale) / 3);
    const int iconY = (SCREEN_HEIGHT - (chirpy_height * scale)) / 2;
    graphics::drawScaledXbm(display, iconX, iconY, chirpy_width, chirpy_height, chirpy, scale);

#if GRAPHICS_TFT_COLORING_ENABLED
    // Colour Chirpy on colour displays. The glyph is a filled head silhouette whose eyes are holes
    // (off pixels), so two stacked regions render the proper mascot without a second bitmap:
    //   A) whole glyph -> green body / frame / legs
    //   B) the eye band -> black face, with the eye holes turning white via the region's off-colour
    // Start the face band one column in from the head's left edge (col 6) so that edge stays green,
    // matching the green column already left on the right edge (col 31).
    graphics::registerTFTColorRegionDirect(iconX, iconY, chirpy_width * scale, chirpy_height * scale,
                                           graphics::TFTPalette::MeshtasticGreen, graphics::getThemeBodyBg());
    graphics::registerTFTColorRegionDirect(iconX + 7 * scale, iconY + 12 * scale, 24 * scale, 16 * scale,
                                           graphics::TFTPalette::Black, graphics::TFTPalette::White);
#endif

    int textX = x + (display->getWidth() / 2) - textX_offset - (display->getStringWidth("Hello") / 2);
    display->drawString(textX, getTextPositions(display)[line++], "Hello");
    textX = x + (display->getWidth() / 2) - textX_offset - (display->getStringWidth("World!") / 2);
    display->drawString(textX, getTextPositions(display)[line++], "World!");
}

} // namespace DebugRenderer
} // namespace graphics
#endif
