#include "configuration.h"
#if HAS_SCREEN
#include "ClockRenderer.h"
#include "Default.h"
#include "DisplayFormatters.h"
#include "GPS.h"
#include "MenuHandler.h"
#if HAS_HOST_POWEROFF
#include "platform/portduino/LinuxPower.h"
#endif
#include "MeshRadio.h"
#include "MeshService.h"
#include "MessageStore.h"
#include "NodeDB.h"
#include "UptimeClock.h"
#include "buzz.h"
#include "graphics/Backlight.h"
#include "graphics/Screen.h"
#include "graphics/SharedUIDisplay.h"
#include "graphics/TFTColorRegions.h"
#include "graphics/draw/MapRenderer.h"
#if BASEUI_WIFI_MANAGER
#include "mesh/wifi/WiFiNetworks.h"
#include <WiFi.h>
#endif
#if BASEUI_MAP_ROUTING
#include "graphics/draw/NotificationRenderer.h"
#endif
#if BASEUI_MAP_NAVIGATION
#include "graphics/draw/MapCoordinateParse.h"
#include "graphics/draw/MapNavigation.h"
#endif
#if BASEUI_MAP_NAVIGATION
#include "gps/GeoCoord.h"
#endif
#if BASEUI_MAP_ADDRESS_SEARCH
#include "graphics/niche/Map/MapTileFetch.h"
#endif
#include "graphics/draw/MessageRenderer.h"
#include "graphics/draw/UIRenderer.h"
#include "input/RotaryEncoderInterruptImpl1.h"
#include "input/UpDownInterruptImpl1.h"
#include "main.h"
#include "mesh/Default.h"
#if HAS_LORA_FEM
#include "mesh/LoRaFEMInterface.h"
#endif
#include "mesh/MeshTypes.h"
#include "mesh/RadioLibInterface.h"
#include "modules/AdminModule.h"
#include "modules/CannedMessageModule.h"
#if !MESHTASTIC_EXCLUDE_MQTT
#include "mqtt/MQTT.h"
#endif
#include "modules/ExternalNotificationModule.h"
#include "modules/GeofenceModule.h"
#include "modules/KeyVerificationModule.h"
#if HAS_TELEMETRY && HAS_SENSOR && !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR
#include "modules/Telemetry/EnvironmentTelemetry.h"
#endif
#include "modules/TraceRouteModule.h"
#include "modules/WaypointModule.h"
#if !MESHTASTIC_EXCLUDE_WAYPOINT
#include "WaypointStore.h"
#if BASEUI_WAYPOINT_EDITOR
#include "WaypointUtils.h"
#include "gps/RTC.h"
#endif
#endif
#if defined(USE_SDL_AUDIO) && defined(MESHTASTIC_ENABLE_TTS)
#include "platform/portduino/SamPlayback.h"
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <utility>

namespace graphics
{

#if BASEUI_MAP_NAVIGATION || BASEUI_WIFI_MANAGER || BASEUI_WAYPOINT_EDITOR
// A banner shown from inside another banner's callback is cleared as that one closes, so the notice waits a loop.
static char pendingNotice[64];
static void queueNotice(const char *message)
{
    strncpy(pendingNotice, message, sizeof(pendingNotice) - 1);
    pendingNotice[sizeof(pendingNotice) - 1] = '\0';
    menuHandler::menuQueue = menuHandler::NoticeMenu;
    screen->runNow();
}
#endif

#if BASEUI_WAYPOINT_EDITOR
// The waypoint New Waypoint Here is writing, kept across the prompts and pickers that fill it in.
static struct {
    char name[sizeof(meshtastic_Waypoint::name)];
    char description[sizeof(meshtastic_Waypoint::description)];
    uint32_t icon;       // a codepoint, drawn as its emote where there is one
    uint32_t expireSecs; // from now; 0 never
    bool atMapCenter;    // placed where the panned map is centred; otherwise at our fix when sent
    int32_t latitudeI, longitudeI;
} waypointDraft;
#endif

#if BASEUI_WIFI_MANAGER
static bool wifiScanAwaited = false;
static char wifiPendingSsid[33]; // the network a password is being typed for, or an action chosen on

// Joins a network and says so. `deferred`: called from inside a banner's callback, so the notice waits a loop.
static void joinWifi(const char *ssid, const char *psk, bool deferred)
{
    char message[64];
    if (WiFiNetworks::join(ssid, psk)) {
        snprintf(message, sizeof(message), "Joining %s", ssid);
    } else {
        // WiFi had no network at boot, so never started: the reboot brings it up on this one.
        snprintf(message, sizeof(message), "Rebooting to join %s", ssid);
        rebootAtMsec = Time::timerEndsAtMillis(DEFAULT_REBOOT_SECONDS * 1000);
    }
    if (deferred)
        queueNotice(message);
    else
        screen->showSimpleBanner(message, 3000);
}
#endif

namespace
{

#if !MESHTASTIC_EXCLUDE_WAYPOINT
uint32_t selectedGeofenceWaypointId = 0;
#endif

template <typename X> struct Identity {
    using type = X;
};

// One banner is live at a time, so the options table and handler for it live in per-T statics.
template <typename T> struct StaticBannerState {
    static const MenuOption<T> *options;
    static void (*onSelection)(const MenuOption<T> &, int);
    static void dispatch(int selected) { onSelection(options[selected], selected); }
};
template <typename T> const MenuOption<T> *StaticBannerState<T>::options = nullptr;
template <typename T> void (*StaticBannerState<T>::onSelection)(const MenuOption<T> &, int) = nullptr;

// Caller must ensure the provided options array outlives the banner callback.
template <typename T, size_t N>
BannerOverlayOptions createStaticBannerOptions(const char *message, const MenuOption<T> (&options)[N],
                                               std::array<const char *, N> &labels,
                                               typename Identity<void (*)(const MenuOption<T> &, int)>::type onSelection)
{
    for (size_t i = 0; i < N; ++i) {
        labels[i] = options[i].label;
    }

    StaticBannerState<T>::options = options;
    StaticBannerState<T>::onSelection = onSelection;

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = message;
    bannerOptions.optionsArrayPtr = labels.data();
    bannerOptions.optionsCount = static_cast<uint8_t>(N);
    bannerOptions.bannerCallback = &StaticBannerState<T>::dispatch;
    return bannerOptions;
}

const StoredMessage *getNewestMessageForActiveThread()
{
    const auto &messages = messageStore.getMessages();
    if (messages.empty()) {
        return nullptr;
    }

    const auto mode = graphics::MessageRenderer::getThreadMode();
    const int channel = graphics::MessageRenderer::getThreadChannel();
    const uint32_t peer = graphics::MessageRenderer::getThreadPeer();
    const uint32_t localNode = nodeDB->getNodeNum();

    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        const StoredMessage &m = *it;
        if (!messageStore.isMessageVisible(m)) {
            continue;
        }

        if (mode == graphics::MessageRenderer::ThreadMode::ALL) {
            return &m;
        }

        if (mode == graphics::MessageRenderer::ThreadMode::CHANNEL) {
            if (m.type == MessageType::BROADCAST && static_cast<int>(m.channelIndex) == channel) {
                return &m;
            }
            continue;
        }

        if (mode == graphics::MessageRenderer::ThreadMode::DIRECT) {
            if (m.type != MessageType::DM_TO_US) {
                continue;
            }
            const uint32_t other = (m.sender == localNode) ? m.dest : m.sender;
            if (other == peer) {
                return &m;
            }
        }
    }

    return nullptr;
}

// Freetext compose is offered whenever the device can enter text at all: a physical
// keyboard, an on-screen keyboard driven by rotary/trackball/joystick, or a touchscreen
// virtual keyboard.
bool freetextAvailable()
{
#if defined(USE_VIRTUAL_KEYBOARD)
    return true;
#else
    return kb_found || osk_found;
#endif
}

void launchReplyForMessage(const StoredMessage &message, bool freetext)
{
    if (message.type == MessageType::BROADCAST || message.dest == NODENUM_BROADCAST) {
        if (freetext) {
            cannedMessageModule->LaunchFreetextWithDestination(NODENUM_BROADCAST, message.channelIndex);
        } else {
            cannedMessageModule->LaunchWithDestination(NODENUM_BROADCAST, message.channelIndex);
        }
        return;
    }

    const uint32_t localNode = nodeDB->getNodeNum();
    const uint32_t peer = (message.sender == localNode) ? message.dest : message.sender;
    if (peer == 0 || peer == NODENUM_BROADCAST) {
        return;
    }

    if (freetext) {
        cannedMessageModule->LaunchFreetextWithDestination(peer);
    } else {
        cannedMessageModule->LaunchWithDestination(peer);
    }
}

} // namespace

menuHandler::screenMenus menuHandler::menuQueue = MenuNone;
uint32_t menuHandler::pickedNodeNum = 0;
meshtastic_Config_LoRaConfig_RegionCode menuHandler::pendingRegion = meshtastic_Config_LoRaConfig_RegionCode_UNSET;
bool test_enabled = false;
uint8_t test_count = 0;
// WiFi Toggle is reached from both the System and the WiFi menu: its Back returns to whichever opened it.
static menuHandler::screenMenus wifiToggleReturn = menuHandler::MenuNone;

void menuHandler::loraMenu()
{
    static const char *optionsArray[] = {
        "Back",    "Device Role", "Radio Preset", "Frequency Slot", "LoRa Region", "Transmit Enabled",
#if HAS_LORA_FEM
        "FEM LNA",
#endif
    };
    // NOTE: "FEM LNA" must stay last; it is the only entry that can be hidden at runtime by
    // trimming optionsCount, which only works for a trailing option.
    enum optionsNumbers {
        Back = 0,
        DeviceRolePicker = 1,
        RadioPresetPicker = 2,
        FrequencySlot = 3,
        LoraPicker = 4,
        TxEnabled = 5,
#if HAS_LORA_FEM
        LoraFemLna = 6
#endif
    };
    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "LoRa Actions";
    bannerOptions.optionsArrayPtr = optionsArray;
#if HAS_LORA_FEM
    bannerOptions.optionsCount = loraFEMInterface.isLnaCanControl() ? 7 : 6;
#else
    bannerOptions.optionsCount = 6;
#endif
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == Back) {
            // No action
        } else if (selected == DeviceRolePicker) {
            menuHandler::menuQueue = menuHandler::DeviceRolePicker;
        } else if (selected == RadioPresetPicker) {
            menuHandler::menuQueue = menuHandler::RadioPresetPicker;
        } else if (selected == FrequencySlot) {
            menuHandler::menuQueue = menuHandler::FrequencySlot;
        } else if (selected == LoraPicker) {
            menuHandler::menuQueue = menuHandler::LoraPicker;
        } else if (selected == TxEnabled) {
            menuHandler::menuQueue = menuHandler::TXEnabledMenu;
        }
#if HAS_LORA_FEM
        else if (selected == LoraFemLna) {
            menuHandler::menuQueue = menuHandler::LoraFemLnaToggleMenu;
        }
#endif
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::OnboardMessage()
{
    static const char *optionsArray[] = {"OK", "Got it!"};
    enum optionsNumbers { OK, got };
    BannerOverlayOptions bannerOptions;
#if HAS_TFT
    bannerOptions.message = "Welcome to Meshtastic!\nSwipe to navigate and\nlong press to select\nor open a menu.";
#elif defined(BUTTON_PIN)
    bannerOptions.message = "Welcome to Meshtastic!\nClick to navigate and\nlong press to select\nor open a menu.";
#else
    bannerOptions.message = "Welcome to Meshtastic!\nUse the Select button\nto open menus\nand make selections.";
#endif
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = 2;
    bannerOptions.bannerCallback = [](int selected) -> void {
        menuHandler::menuQueue = menuHandler::NoTimeoutLoraPicker;
        screen->runNow();
    };
    screen->showOverlayBanner(bannerOptions);
}

// Out-of-box US setup starts on LongTurbo rather than the region table's LongFast. Menu-only: the
// US entry in `regions[]` keeps LongFast, so no other route onto US changes. Anything that already
// states a preset - a pinned userpref, or a preset moved off the install default - outranks it.
meshtastic_Config_LoRaConfig_ModemPreset menuHandler::presetForRegionSelection(const meshtastic_Config_LoRaConfig &lora,
                                                                               meshtastic_Config_LoRaConfig_RegionCode selected)
{
#ifdef USERPREFS_LORACONFIG_MODEM_PRESET
    (void)selected; // the pinned preset wins outright; nothing to decide
#else
    if (lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET && selected == meshtastic_Config_LoRaConfig_RegionCode_US &&
        lora.use_preset && lora.modem_preset == meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST) {
        return meshtastic_Config_LoRaConfig_ModemPreset_LONG_TURBO;
    }
#endif
    return lora.modem_preset;
}

static void applyLoraRegion(meshtastic_Config_LoRaConfig_RegionCode region, bool isHam)
{
    // Decided first: it keys off the *outgoing* region being UNSET.
    const meshtastic_Config_LoRaConfig_ModemPreset selectionPreset = menuHandler::presetForRegionSelection(config.lora, region);
    if (selectionPreset != config.lora.modem_preset) {
        LOG_INFO("First region is %s, default preset to %s", getRegion(region)->name,
                 DisplayFormatters::getModemPresetDisplayName(selectionPreset, false, true));
        config.lora.modem_preset = selectionPreset;
    }

    config.lora.region = region;
    config.lora.channel_num = 0; // Reset to default channel

    // Reconcile the preset with the explicitly chosen region: a preset locked to another
    // region would leave config.lora invalid until applyModemConfig() repairs it with
    // error/critical-error side effects - or, for the swappable EU trio, the clamp would
    // flip the region right back. The user picked the region, so the preset follows it.
    const RegionInfo *newRegion = getRegion(region);
    if (config.lora.use_preset && !newRegion->supportsPreset(config.lora.modem_preset)) {
        LOG_INFO("Preset %s unavailable in %s, use default %s",
                 DisplayFormatters::getModemPresetDisplayName(config.lora.modem_preset, false, true), newRegion->name,
                 DisplayFormatters::getModemPresetDisplayName(newRegion->getDefaultPreset(), false, true));
        config.lora.modem_preset = newRegion->getDefaultPreset();
    }

    if (isHam && adminModule) {
        meshtastic_HamParameters hamParams = meshtastic_HamParameters_init_zero;
        strncpy(hamParams.call_sign, "N0CALL", sizeof(hamParams.call_sign) - 1);
        strncpy(hamParams.short_name, "N0CL", sizeof(hamParams.short_name));
        hamParams.tx_power = config.lora.tx_power;
        hamParams.frequency = config.lora.override_frequency;
        adminModule->handleSetHamMode(hamParams);
    }
    auto changes = SEGMENT_CONFIG;
#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
    // Minting the key moves our node num with it, and nothing reboots on this path to repair it later.
    if (nodeDB->ensurePkiIdentity()) {
        changes |= SEGMENT_DEVICESTATE | SEGMENT_NODEDATABASE;
    }
#endif
    initRegion();
    if (getEffectiveDutyCycle() < 100) {
        config.lora.ignore_mqtt = true;
    }
#if !MESHTASTIC_EXCLUDE_MQTT
    if (MQTT::applyRegionRootTopic(myRegion->name))
        changes |= SEGMENT_MODULECONFIG;
#endif
#if !MESHTASTIC_EXCLUDE_GPS
    // Enable gps if it was previously disabled due to region not being set
    if (gps != nullptr && !gps->isEnabled() && config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED)
        gps->enable();
#endif
    if (config.lora.region != meshtastic_Config_LoRaConfig_RegionCode_UNSET && !config.lora.tx_enabled && !owner.is_licensed) {
        LOG_WARN("Setting config.lora.tx_enabled to true");
        config.lora.tx_enabled = true;
    }
    service->reloadConfig(changes);
}

void menuHandler::LoraRegionPicker(uint32_t duration)
{
#ifdef HAS_HAM_2M_ONLY
    // Hardware is restricted to the amateur 2m band - offer only the 2m regions
    // so the user cannot pick a sub-GHz region the RF path cannot emit or receive.
    static const LoraRegionOption regionOptions[] = {
        {"Back", OptionsAction::Back},
        {"ITU1_2M (144-146)", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_ITU1_2M},
        {"ITU2_2M (144-148)", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_ITU2_2M},
        {"ITU3_2M (144-148)", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_ITU3_2M},
    };
#else
    static const LoraRegionOption regionOptions[] = {
        {"Back", OptionsAction::Back},
        {"US", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_US},
        {"EU_433", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_EU_433},
        {"EU_868", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_EU_868},
        {"EU_866", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_EU_866},
        {"EU_868_NARROW", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_EU_N_868},
        {"CN", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_CN},
        {"JP", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_JP},
        {"ANZ", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_ANZ},
        {"KR", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_KR},
        {"TW", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_TW},
        {"RU", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_RU},
        {"IN", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_IN},
        {"NZ_865", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_NZ_865},
        {"TH", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_TH},
        {"LORA_24", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_LORA_24},
        {"UA_433", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_UA_433},
        {"MY_433", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_MY_433},
        {"MY_919", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_MY_919},
        {"SG_923", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_SG_923},
        {"PH_433", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_PH_433},
        {"PH_868", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_PH_868},
        {"PH_915", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_PH_915},
        {"ANZ_433", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_ANZ_433},
        {"KZ_433", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_KZ_433},
        {"KZ_863", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_KZ_863},
        {"NP_865", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_NP_865},
        {"BR_902", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_BR_902},
        {"ITU1_2M (144-146)", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_ITU1_2M},
        {"ITU2_2M (144-148)", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_ITU2_2M},
        {"ITU3_2M (144-148)", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_ITU3_2M},
        {"ITU2_125CM (220-225)", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_ITU2_125CM},
        {"ITU1_70CM (430-440)", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_ITU1_70CM},
        {"ITU2_70CM (420-450)", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_ITU2_70CM},
        {"ITU3_70CM (430-450)", OptionsAction::Select, meshtastic_Config_LoRaConfig_RegionCode_ITU3_70CM},

    };
#endif

    constexpr size_t regionCount = sizeof(regionOptions) / sizeof(regionOptions[0]);
    static std::array<const char *, regionCount> regionLabels{};

    const char *bannerMessage = "Set the LoRa region";
    if (currentResolution == ScreenResolution::UltraLow) {
        bannerMessage = "LoRa Region";
    }

    auto bannerOptions =
        createStaticBannerOptions(bannerMessage, regionOptions, regionLabels, [](const LoraRegionOption &option, int) -> void {
            if (!option.hasValue) {
                return;
            }

            auto selectedRegion = option.value;
            if (config.lora.region == selectedRegion) {
                return;
            }

            const RegionInfo *selectedRegionInfo = getRegion(selectedRegion);
            bool hamMode = selectedRegionInfo->code == selectedRegion && selectedRegionInfo->profile &&
                           selectedRegionInfo->profile->licensedOnly;

            // Validate radio compatibility for a prospective Ham region before confirmation.
            auto candidateLora = config.lora;
            candidateLora.region = selectedRegion;
            char regionErr[160] = {};
            if (!RadioInterface::checkConfigRegion(candidateLora, regionErr, sizeof(regionErr), hamMode)) {
                LOG_WARN("Ignoring region selection: %s", regionErr);
                return;
            }

            if (hamMode) {
                LOG_INFO("User chose an amateur radio mode region");
                pendingRegion = selectedRegion;
                menuQueue = HamModeConfirm;
                screen->runNow();
            } else if (owner.is_licensed) {
                LOG_INFO("Licensed user chose non-ham region; prompt to revert licensed mode");
                pendingRegion = selectedRegion;
                menuQueue = LicensedToNormalConfirm;
                screen->runNow();
            } else {
                applyLoraRegion(selectedRegion, false);
            }
        });

    bannerOptions.durationMs = duration;

    int initialSelection = 0;
    for (size_t i = 0; i < regionCount; ++i) {
        if (regionOptions[i].hasValue && regionOptions[i].value == config.lora.region) {
            initialSelection = static_cast<int>(i);
            break;
        }
    }
    bannerOptions.InitialSelected = initialSelection;

    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::hamModeConfirmMenu()
{
    static const char *confirmOptions[] = {"No", "Yes"};
    BannerOverlayOptions confirmBanner;
    confirmBanner.message = "I confirm I am a\nlicensed amateur\nradio operator";
    confirmBanner.optionsArrayPtr = confirmOptions;
    confirmBanner.optionsCount = 2;
    confirmBanner.bannerCallback = [](int selected) {
        if (selected == 1)
            applyLoraRegion(pendingRegion, true);
    };
    screen->showOverlayBanner(confirmBanner);
}

void menuHandler::licensedToNormalConfirmMenu()
{
    static const char *confirmOptions[] = {"Keep licensed", "Revert to Normal"};
    BannerOverlayOptions confirmBanner;
    confirmBanner.message = "Revert licensed\nmode? This will\nre-enable encryption.";
    confirmBanner.optionsArrayPtr = confirmOptions;
    confirmBanner.optionsCount = 2;
    confirmBanner.bannerCallback = [](int selected) {
        if (selected == 1) {
            owner.is_licensed = false;
            config.lora.override_duty_cycle = false;
            service->reloadOwner(false);
        }
        applyLoraRegion(pendingRegion, false);
    };
    screen->showOverlayBanner(confirmBanner);
}

void menuHandler::deviceRolePicker()
{
    static const char *optionsArray[] = {"Back", "Client", "Client Mute", "Lost and Found", "Tracker"};
    enum optionsNumbers {
        Back = 0,
        devicerole_client = 1,
        devicerole_clientmute = 2,
        devicerole_lostandfound = 3,
        devicerole_tracker = 4
    };
    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Device Role";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = 5;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == Back) {
            menuHandler::menuQueue = menuHandler::LoraMenu;
            screen->runNow();
            return;
        } else if (selected == devicerole_client) {
            config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
        } else if (selected == devicerole_clientmute) {
            config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE;
        } else if (selected == devicerole_lostandfound) {
            config.device.role = meshtastic_Config_DeviceConfig_Role_LOST_AND_FOUND;
        } else if (selected == devicerole_tracker) {
            config.device.role = meshtastic_Config_DeviceConfig_Role_TRACKER;
        }
        service->reloadConfig(SEGMENT_CONFIG);
        rebootAtMsec = Time::timerEndsAtMillis(DEFAULT_REBOOT_SECONDS * 1000);
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::FrequencySlotPicker()
{

    enum ReplyOptions : int { Back = -1 };
    constexpr int MAX_CHANNEL_OPTIONS = 202;
    static const char *optionsArray[MAX_CHANNEL_OPTIONS];
    static int optionsEnumArray[MAX_CHANNEL_OPTIONS];
    static char channelText[MAX_CHANNEL_OPTIONS - 1][12];
    int options = 0;
    optionsArray[options] = "Back";
    optionsEnumArray[options++] = Back;
    optionsArray[options] = "Slot 0 (Auto)";
    optionsEnumArray[options++] = 0;

    // Calculate number of channels (copied from RadioInterface::applyModemConfig())

    meshtastic_Config_LoRaConfig &loraConfig = config.lora;
    double bw = loraConfig.use_preset ? modemPresetToBwKHz(loraConfig.modem_preset, myRegion->wideLora)
                                      : bwCodeToKHz(loraConfig.bandwidth);

    uint32_t numChannels = 0;
    if (myRegion) {
        // Match RadioInterface::applyModemConfig(): include padding, add spacing in numerator, and use round()
        const double spacing = myRegion->profile->spacing;
        const double padding = myRegion->profile->padding;
        const double channelBandwidthMHz = bw / 1000.0;
        const double numerator = (myRegion->freqEnd - myRegion->freqStart) + spacing;
        const double denominator = spacing + (padding * 2) + channelBandwidthMHz;
        if (denominator > 0.0) {
            numChannels = static_cast<uint32_t>(round(numerator / denominator));
        } else {
            LOG_WARN("Invalid region config: non-positive channel spacing/width");
        }
    } else {
        LOG_WARN("Region not set, can't calc channel count");
        return;
    }

    if (numChannels > (uint32_t)(MAX_CHANNEL_OPTIONS - 2))
        numChannels = (uint32_t)(MAX_CHANNEL_OPTIONS - 2);

    for (uint32_t ch = 1; ch <= numChannels; ch++) {
        snprintf(channelText[ch - 1], sizeof(channelText[ch - 1]), "Slot %lu", (unsigned long)ch);
        optionsArray[options] = channelText[ch - 1];
        optionsEnumArray[options++] = (int)ch;
    }

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Frequency Slot";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsEnumPtr = optionsEnumArray;
    bannerOptions.optionsCount = options;

    // Start highlight on current channel if possible, otherwise on "1"
    int initial = (int)config.lora.channel_num + 1;
    if (initial < 2 || initial > (int)numChannels + 1)
        initial = 1;
    bannerOptions.InitialSelected = initial;

    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == Back) {
            menuHandler::menuQueue = menuHandler::LoraMenu;
            screen->runNow();
            return;
        }

        config.lora.channel_num = selected;
        service->reloadConfig(SEGMENT_CONFIG);
    };

    screen->showOverlayBanner(bannerOptions);
}

// Maximum presets any region can have + 1 for Back
static constexpr int MAX_PRESET_OPTIONS = 16;

static BannerOverlayOptions buildRegionPresetBanner()
{
    // Static storage reused each call - safe because the banner is shown immediately after.
    static const char *optionsArray[MAX_PRESET_OPTIONS];
    static int optionsEnumArray[MAX_PRESET_OPTIONS];
    static char presetLabelBuf[MAX_PRESET_OPTIONS][12]; // scratch space for name copies
    int count = 0;

    optionsArray[count] = "Back";
    optionsEnumArray[count++] = -1;

    if (myRegion && myRegion->profile) {
        const meshtastic_Config_LoRaConfig_ModemPreset *presets = myRegion->getAvailablePresets();
        size_t numPresets = myRegion->getNumPresets();
        for (size_t i = 0; i < numPresets && count < MAX_PRESET_OPTIONS; ++i) {
            const char *name = DisplayFormatters::getModemPresetDisplayName(presets[i], false, true);
            strncpy(presetLabelBuf[count], name, sizeof(presetLabelBuf[count]) - 1);
            presetLabelBuf[count][sizeof(presetLabelBuf[count]) - 1] = '\0';
            optionsArray[count] = presetLabelBuf[count];
            optionsEnumArray[count++] = static_cast<int>(presets[i]);
        }
    }

    int initialSelection = 0;
    for (int i = 1; i < count; ++i) {
        if (optionsEnumArray[i] == static_cast<int>(config.lora.modem_preset)) {
            initialSelection = i;
            break;
        }
    }

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Radio Preset";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsEnumPtr = optionsEnumArray;
    bannerOptions.optionsCount = static_cast<uint8_t>(count);
    bannerOptions.InitialSelected = initialSelection;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == -1) {
            menuHandler::menuQueue = menuHandler::LoraMenu;
            screen->runNow();
            return;
        }
        config.lora.use_preset = true;
        config.lora.modem_preset = static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(selected);
        config.lora.channel_num = 0;        // Reset to default channel for the preset
        config.lora.override_frequency = 0; // Clear any custom frequency
        service->reloadConfig(SEGMENT_CONFIG);
    };
    return bannerOptions;
}

void menuHandler::radioPresetPicker()
{
    screen->showOverlayBanner(buildRegionPresetBanner());
}

void menuHandler::txEnabledMenu()
{
    static const char *optionsArray[] = {"Back", "Enabled", "Disabled"};
    enum optionsNumbers { Back = 0, Enabled = 1, Disabled = 2 };
    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Transmit Enabled";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = 3;
    bannerOptions.InitialSelected = config.lora.tx_enabled ? Enabled : Disabled;
    bannerOptions.bannerCallback = [](int selected) -> void {
        // -1 is the timeout/dismiss case; treat it like Back so we never write config.
        if (selected <= Back) {
            menuHandler::menuQueue = menuHandler::LoraMenu;
            screen->runNow();
            return;
        }
        bool wanted = (selected == Enabled);
        if (config.lora.tx_enabled == wanted)
            return;
        config.lora.tx_enabled = wanted;
        service->reloadConfig(SEGMENT_CONFIG);
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::twelveHourPicker()
{
    static const char *optionsArray[] = {"Back", "12-hour", "24-hour"};
    enum optionsNumbers { Back = 0, twelve = 1, twentyfour = 2 };
    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Time Format";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = 3;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == Back) {
            menuHandler::menuQueue = menuHandler::ClockMenu;
            screen->runNow();
        } else if (selected == twelve) {
            config.display.use_12h_clock = true;
        } else {
            config.display.use_12h_clock = false;
        }
        service->reloadConfig(SEGMENT_CONFIG);
    };
    screen->showOverlayBanner(bannerOptions);
}

// Reusable confirmation prompt function
void menuHandler::showConfirmationBanner(const char *message, std::function<void()> onConfirm)
{
    static const char *confirmOptions[] = {"No", "Yes"};
    static std::function<void()> pendingConfirm;
    pendingConfirm = std::move(onConfirm);
    BannerOverlayOptions confirmBanner;
    confirmBanner.message = message;
    confirmBanner.optionsArrayPtr = confirmOptions;
    confirmBanner.optionsCount = 2;
    confirmBanner.bannerCallback = [](int confirmSelected) -> void {
        // Take it out first: the handler may open another confirmation and reassign pendingConfirm.
        std::function<void()> fn = std::move(pendingConfirm);
        pendingConfirm = nullptr;
        if (confirmSelected == 1 && fn) {
            fn();
        }
    };
    screen->showOverlayBanner(confirmBanner);
}

void menuHandler::clockFacePicker()
{
    static const ClockFaceOption clockFaceOptions[] = {
        {"Back", OptionsAction::Back},
        {"Digital", OptionsAction::Select, false},
        {"Analog", OptionsAction::Select, true},
    };

    constexpr size_t clockFaceCount = sizeof(clockFaceOptions) / sizeof(clockFaceOptions[0]);
    static std::array<const char *, clockFaceCount> clockFaceLabels{};

    auto bannerOptions = createStaticBannerOptions("Which Face?", clockFaceOptions, clockFaceLabels,
                                                   [](const ClockFaceOption &option, int) -> void {
                                                       if (option.action == OptionsAction::Back) {
                                                           menuHandler::menuQueue = menuHandler::ClockMenu;
                                                           screen->runNow();
                                                           return;
                                                       }

                                                       if (!option.hasValue) {
                                                           return;
                                                       }

                                                       if (uiconfig.is_clockface_analog == option.value) {
                                                           return;
                                                       }

                                                       uiconfig.is_clockface_analog = option.value;
                                                       saveUIConfig();
                                                       screen->setFrames(Screen::FOCUS_CLOCK);
                                                   });

    bannerOptions.InitialSelected = uiconfig.is_clockface_analog ? 2 : 1;
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::TZPicker()
{
    static const TimezoneOption timezoneOptions[] = {
        {"Back", OptionsAction::Back},
        {"US/Hawaii", OptionsAction::Select, "HST10"},
        {"US/Alaska", OptionsAction::Select, "AKST9AKDT,M3.2.0,M11.1.0"},
        {"US/Pacific", OptionsAction::Select, "PST8PDT,M3.2.0,M11.1.0"},
        {"US/Arizona", OptionsAction::Select, "MST7"},
        {"US/Mountain", OptionsAction::Select, "MST7MDT,M3.2.0,M11.1.0"},
        {"US/Central", OptionsAction::Select, "CST6CDT,M3.2.0,M11.1.0"},
        {"US/Eastern", OptionsAction::Select, "EST5EDT,M3.2.0,M11.1.0"},
        {"BR/Brasilia", OptionsAction::Select, "BRT3"},
        {"UTC", OptionsAction::Select, "UTC0"},
        {"EU/Western", OptionsAction::Select, "GMT0BST,M3.5.0/1,M10.5.0"},
        {"EU/Central", OptionsAction::Select, "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"EU/Eastern", OptionsAction::Select, "EET-2EEST,M3.5.0/3,M10.5.0/4"},
        {"Asia/Kolkata", OptionsAction::Select, "IST-5:30"},
        {"Asia/Hong_Kong", OptionsAction::Select, "HKT-8"},
        {"AU/AWST", OptionsAction::Select, "AWST-8"},
        {"AU/ACST", OptionsAction::Select, "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
        {"AU/AEST", OptionsAction::Select, "AEST-10AEDT,M10.1.0,M4.1.0/3"},
        {"Pacific/NZ", OptionsAction::Select, "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    };

    constexpr size_t timezoneCount = sizeof(timezoneOptions) / sizeof(timezoneOptions[0]);
    static std::array<const char *, timezoneCount> timezoneLabels{};

    auto bannerOptions = createStaticBannerOptions(
        "Pick Timezone", timezoneOptions, timezoneLabels, [](const TimezoneOption &option, int) -> void {
            if (option.action == OptionsAction::Back) {
                menuHandler::menuQueue = menuHandler::ClockMenu;
                screen->runNow();
                return;
            }

            if (!option.hasValue) {
                return;
            }

            if (strncmp(config.device.tzdef, option.value, sizeof(config.device.tzdef)) == 0) {
                return;
            }

            strncpy(config.device.tzdef, option.value, sizeof(config.device.tzdef));
            config.device.tzdef[sizeof(config.device.tzdef) - 1] = '\0';

            setenv("TZ", config.device.tzdef, 1);
            service->reloadConfig(SEGMENT_CONFIG);
        });

    int initialSelection = 0;
    for (size_t i = 0; i < timezoneCount; ++i) {
        if (timezoneOptions[i].hasValue &&
            strncmp(config.device.tzdef, timezoneOptions[i].value, sizeof(config.device.tzdef)) == 0) {
            initialSelection = static_cast<int>(i);
            break;
        }
    }
    bannerOptions.InitialSelected = initialSelection;

    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::clockMenu()
{
    enum optionsNumbers { Back = 0, Clock, Time, Timezone };
#if defined(OLED_TINY)
    static const char *optionsArray[] = {"Back", "Time Format", "Timezone"};
    static const int optionsEnumArray[] = {Back, Time, Timezone};
#else
    static const char *optionsArray[] = {"Back", "Clock Face", "Time Format", "Timezone"};
    static const int optionsEnumArray[] = {Back, Clock, Time, Timezone};
#endif
    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Clock Action";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsEnumPtr = optionsEnumArray;
    bannerOptions.optionsCount = sizeof(optionsArray) / sizeof(optionsArray[0]);
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == Clock) {
            menuHandler::menuQueue = menuHandler::ClockFacePicker;
            screen->runNow();
        } else if (selected == Time) {
            menuHandler::menuQueue = menuHandler::TwelveHourPicker;
            screen->runNow();
        } else if (selected == Timezone) {
            menuHandler::menuQueue = menuHandler::TzPicker;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
}
void menuHandler::messageResponseMenu()
{
    enum optionsNumbers { Back = 0, ViewMode, MessageOrder, DeleteMenu, ReplyMenu, MuteChannel, Aloud, enumEnd };

    static const char *optionsArray[enumEnd];
    static int optionsEnumArray[enumEnd];
    int options = 0;

    auto mode = graphics::MessageRenderer::getThreadMode();
    int threadChannel = graphics::MessageRenderer::getThreadChannel();

    optionsArray[options] = "Back";
    optionsEnumArray[options++] = Back;

    // New Reply submenu (replaces Preset and Freetext directly in this menu)
    optionsArray[options] = "Reply";
    optionsEnumArray[options++] = ReplyMenu;

    optionsArray[options] = "View Chats";
    optionsEnumArray[options++] = ViewMode;

    optionsArray[options] = "Message Order";
    optionsEnumArray[options++] = MessageOrder;

    // If viewing ALL chats, hide “Mute Chat”
    if (mode != graphics::MessageRenderer::ThreadMode::ALL && mode != graphics::MessageRenderer::ThreadMode::DIRECT) {
        const uint8_t chIndex = (threadChannel != 0) ? (uint8_t)threadChannel : channels.getPrimaryIndex();
        const auto &chan = channels.getByIndex(chIndex);

        optionsArray[options] = chan.settings.module_settings.is_muted ? "Unmute Channel" : "Mute Channel";
        optionsEnumArray[options++] = MuteChannel;
    }

    // Delete submenu
    optionsArray[options] = "Delete";
    optionsEnumArray[options++] = DeleteMenu;

#ifdef MESHTASTIC_ENABLE_TTS
    optionsArray[options] = "Read Aloud";
    optionsEnumArray[options++] = Aloud;
#endif

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Message Action";
    if (currentResolution == ScreenResolution::UltraLow) {
        bannerOptions.message = "Message";
    }
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsEnumPtr = optionsEnumArray;
    bannerOptions.optionsCount = options;
    bannerOptions.bannerCallback = [](int selected) -> void {
        LOG_DEBUG("messageResponseMenu: selected %d", selected);

        auto mode = graphics::MessageRenderer::getThreadMode();
        int ch = graphics::MessageRenderer::getThreadChannel();
        uint32_t peer = graphics::MessageRenderer::getThreadPeer();

        LOG_DEBUG("[ReplyCtx] mode=%d ch=%d peer=0x%08x", (int)mode, ch, (unsigned int)peer);

        if (selected == ViewMode) {
            menuHandler::menuQueue = menuHandler::MessageViewModeMenu;
            screen->runNow();

        } else if (selected == MessageOrder) {
            menuHandler::menuQueue = menuHandler::MessageOrderMenu;
            screen->runNow();

            // Reply submenu
        } else if (selected == ReplyMenu) {
            menuHandler::menuQueue = menuHandler::ReplyMenu;
            screen->runNow();

        } else if (selected == MuteChannel) {
            const uint8_t chIndex = (ch != 0) ? (uint8_t)ch : channels.getPrimaryIndex();
            auto &chan = channels.getByIndex(chIndex);
            if (chan.settings.has_module_settings) {
                chan.settings.module_settings.is_muted = !chan.settings.module_settings.is_muted;
                nodeDB->saveToDisk();
            }

        } else if (selected == DeleteMenu) {
            menuHandler::menuQueue = menuHandler::DeleteMessagesMenu;
            screen->runNow();

#ifdef MESHTASTIC_ENABLE_TTS
        } else if (selected == Aloud) {
            if (const StoredMessage *latest = getNewestMessageForActiveThread()) {
                const char *msg = MessageStore::getText(*latest);
                if (msg && msg[0]) {
#if defined(HAS_I2S)
                    audioThread->readAloud(msg);
#elif defined(USE_SDL_AUDIO)
                    portduino_audio::readAloud(msg);
#endif
                }
            }
#endif
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::replyMenu()
{
    enum replyOptions { Back = 0, ReplyPreset, ReplyFreetext, enumEnd };

    static const char *optionsArray[enumEnd];
    static int optionsEnumArray[enumEnd];
    int options = 0;

    // Back
    optionsArray[options] = "Back";
    optionsEnumArray[options++] = Back;

    // Preset reply
    optionsArray[options] = "With Preset";
    optionsEnumArray[options++] = ReplyPreset;

    // Freetext reply (only when the device can enter text)
    if (freetextAvailable()) {
        optionsArray[options] = "With Freetext";
        optionsEnumArray[options++] = ReplyFreetext;
    }

    BannerOverlayOptions bannerOptions;

    // Dynamic title based on thread mode
    auto mode = graphics::MessageRenderer::getThreadMode();
    if (mode == graphics::MessageRenderer::ThreadMode::CHANNEL) {
        bannerOptions.message = "Reply to Channel";
    } else if (mode == graphics::MessageRenderer::ThreadMode::DIRECT) {
        bannerOptions.message = "Reply to DM";
    } else {
        // View All
        bannerOptions.message = "Reply to Last Msg";
    }

    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsEnumPtr = optionsEnumArray;
    bannerOptions.optionsCount = options;
    bannerOptions.InitialSelected = 1;

    bannerOptions.bannerCallback = [](int selected) -> void {
        auto mode = graphics::MessageRenderer::getThreadMode();
        int ch = graphics::MessageRenderer::getThreadChannel();
        uint32_t peer = graphics::MessageRenderer::getThreadPeer();

        if (selected == Back) {
            menuHandler::menuQueue = menuHandler::MessageResponseMenu;
            screen->runNow();
            return;
        }

        // Preset reply
        if (selected == ReplyPreset) {
            if (mode == graphics::MessageRenderer::ThreadMode::CHANNEL) {
                cannedMessageModule->LaunchWithDestination(NODENUM_BROADCAST, ch);
            } else if (mode == graphics::MessageRenderer::ThreadMode::DIRECT) {
                cannedMessageModule->LaunchWithDestination(peer);
            } else if (const StoredMessage *latest = getNewestMessageForActiveThread()) {
                launchReplyForMessage(*latest, false);
            }

            return;
        }

        // Freetext reply
        if (selected == ReplyFreetext) {
            if (mode == graphics::MessageRenderer::ThreadMode::CHANNEL) {
                cannedMessageModule->LaunchFreetextWithDestination(NODENUM_BROADCAST, ch);
            } else if (mode == graphics::MessageRenderer::ThreadMode::DIRECT) {
                cannedMessageModule->LaunchFreetextWithDestination(peer);
            } else if (const StoredMessage *latest = getNewestMessageForActiveThread()) {
                launchReplyForMessage(*latest, true);
            }

            return;
        }
    };
    screen->showOverlayBanner(bannerOptions);
}
void menuHandler::deleteMessagesMenu()
{
    enum optionsNumbers { Back = 0, DeleteOldest, DeleteThis, DeleteAll, enumEnd };

    static const char *optionsArray[enumEnd];
    static int optionsEnumArray[enumEnd];
    int options = 0;

    auto mode = graphics::MessageRenderer::getThreadMode();

    optionsArray[options] = "Back";
    optionsEnumArray[options++] = Back;

    optionsArray[options] = "Delete Oldest";
    optionsEnumArray[options++] = DeleteOldest;

    // If viewing ALL chats → hide “Delete This Chat”
    if (mode != graphics::MessageRenderer::ThreadMode::ALL) {
        optionsArray[options] = "Delete This Chat";
        optionsEnumArray[options++] = DeleteThis;
    }
    if (currentResolution == ScreenResolution::UltraLow) {
        optionsArray[options] = "Delete All";
    } else {
        optionsArray[options] = "Delete All Chats";
    }
    optionsEnumArray[options++] = DeleteAll;

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Delete Messages";

    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsEnumPtr = optionsEnumArray;
    bannerOptions.optionsCount = options;
    bannerOptions.bannerCallback = [](int selected) -> void {
        auto mode = graphics::MessageRenderer::getThreadMode();
        int ch = graphics::MessageRenderer::getThreadChannel();
        uint32_t peer = graphics::MessageRenderer::getThreadPeer();

        if (selected == Back) {
            menuHandler::menuQueue = menuHandler::MessageResponseMenu;
            screen->runNow();
            return;
        }

        if (selected == DeleteAll) {
            LOG_INFO("Deleting all messages");
            messageStore.clearAllMessages();
            graphics::MessageRenderer::clearThreadRegistries();
            graphics::MessageRenderer::clearMessageCache();
            return;
        }

        if (selected == DeleteOldest) {
            LOG_INFO("Deleting oldest message");

            if (mode == graphics::MessageRenderer::ThreadMode::ALL) {
                messageStore.deleteOldestMessage();
            } else if (mode == graphics::MessageRenderer::ThreadMode::CHANNEL) {
                messageStore.deleteOldestMessageInChannel(ch);
            } else if (mode == graphics::MessageRenderer::ThreadMode::DIRECT) {
                messageStore.deleteOldestMessageWithPeer(peer);
            }
            return;
        }

        // This only appears in non-ALL modes
        if (selected == DeleteThis) {
            LOG_INFO("Deleting all messages in thread");

            if (mode == graphics::MessageRenderer::ThreadMode::CHANNEL) {
                messageStore.deleteAllMessagesInChannel(ch);
            } else if (mode == graphics::MessageRenderer::ThreadMode::DIRECT) {
                messageStore.deleteAllMessagesWithPeer(peer);
            }
            return;
        }
    };

    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::messageViewModeMenu()
{
    auto encodeChannelId = [](int ch) -> int { return 100 + ch; };

    static std::vector<std::string> labels;
    static std::vector<int> ids;
    static std::vector<uint32_t> idToPeer; // DM lookup

    labels.clear();
    ids.clear();
    idToPeer.clear();

    labels.push_back("Back");
    ids.push_back(-1);
    labels.push_back("View All Chats");
    ids.push_back(-2);

    // Same conversations, in the same order, as the message screen's tabs.
    for (const auto &t : graphics::MessageRenderer::getActiveThreads()) {
        if (t.mode == graphics::MessageRenderer::ThreadMode::CHANNEL) {
            char buf[40];
            const char *cname = channels.getName(t.channel);
            snprintf(buf, sizeof(buf), cname && cname[0] ? "#%s" : "#Ch%d", cname ? cname : "", t.channel);
            labels.push_back(buf);
            ids.push_back(encodeChannelId(t.channel));
        } else if (t.mode == graphics::MessageRenderer::ThreadMode::DIRECT) {
            const auto *node = nodeDB->getMeshNode(t.peer);
            std::string name;
            if (nodeInfoLiteHasUser(node))
                name = sanitizeString(node->long_name).substr(0, 15);
            else {
                char buf[20];
                snprintf(buf, sizeof(buf), "Node !%08x", (unsigned int)t.peer);
                name = buf;
            }
            labels.push_back("@" + name);
            ids.push_back(1000 + (int)idToPeer.size());
            idToPeer.push_back(t.peer);
        }
    }

    // Active ID
    int activeId = -2;
    auto mode = graphics::MessageRenderer::getThreadMode();
    if (mode == graphics::MessageRenderer::ThreadMode::CHANNEL)
        activeId = encodeChannelId(graphics::MessageRenderer::getThreadChannel());
    else if (mode == graphics::MessageRenderer::ThreadMode::DIRECT) {
        uint32_t cur = graphics::MessageRenderer::getThreadPeer();
        for (size_t i = 0; i < idToPeer.size(); ++i)
            if (idToPeer[i] == cur) {
                activeId = 1000 + (int)i;
                break;
            }
    }

    LOG_DEBUG("messageViewModeMenu: Active thread id=%d", activeId);

    // Build banner
    static std::vector<const char *> options;
    static std::vector<int> optionIds;
    options.clear();
    optionIds.clear();

    int initialIndex = 0;
    for (size_t i = 0; i < labels.size(); i++) {
        options.push_back(labels[i].c_str());
        optionIds.push_back(ids[i]);
        if (ids[i] == activeId)
            initialIndex = (int)i;
    }

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Select Conversation";
    bannerOptions.optionsArrayPtr = options.data();
    bannerOptions.optionsEnumPtr = optionIds.data();
    bannerOptions.optionsCount = options.size();
    bannerOptions.InitialSelected = initialIndex;

    bannerOptions.bannerCallback = [](int selected) -> void {
        LOG_DEBUG("messageViewModeMenu: selected=%d", selected);
        if (selected == -1) {
            menuHandler::menuQueue = menuHandler::MessageResponseMenu;
            screen->runNow();
        } else if (selected == -2) {
            graphics::MessageRenderer::setThreadMode(graphics::MessageRenderer::ThreadMode::ALL);
        } else if (selected >= 100 && selected < 200) {
            int ch = selected - 100;
            graphics::MessageRenderer::setThreadMode(graphics::MessageRenderer::ThreadMode::CHANNEL, ch);
        } else if (selected >= 1000) {
            int idx = selected - 1000;
            if (idx >= 0 && (size_t)idx < idToPeer.size()) {
                uint32_t peer = idToPeer[idx];
                graphics::MessageRenderer::setThreadMode(graphics::MessageRenderer::ThreadMode::DIRECT, -1, peer);
            }
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::homeBaseMenu()
{
    enum optionsNumbers { Back, Mute, Backlight, Position, Preset, Freetext, Sleep, enumEnd };

    static const char *optionsArray[enumEnd] = {"Back"};
    static int optionsEnumArray[enumEnd] = {Back};
    int options = 1;

    if (moduleConfig.external_notification.enabled && externalNotificationModule &&
        config.device.buzzer_mode != meshtastic_Config_DeviceConfig_BuzzerMode_DISABLED) {
        if (!externalNotificationModule->getMute()) {
            optionsArray[options] = "Temporarily Mute";
        } else {
            optionsArray[options] = "Unmute";
        }
        optionsEnumArray[options++] = Mute;
    }
#if HAS_BACKLIGHT && defined(USE_EINK) // a frontlight is optional; a TFT is unreadable without its backlight
    optionsArray[options] = "Toggle Backlight";
    optionsEnumArray[options++] = Backlight;
#else
    optionsArray[options] = "Sleep Screen";
    optionsEnumArray[options++] = Sleep;
#endif
    if (config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED) {
        optionsArray[options] = "Send Position";
    } else {
        optionsArray[options] = "Send Node Info";
    }
    optionsEnumArray[options++] = Position;

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Home Action";
    if (currentResolution == ScreenResolution::UltraLow) {
        bannerOptions.message = "Home";
    }
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsEnumPtr = optionsEnumArray;
    bannerOptions.optionsCount = options;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == Mute) {
            if (moduleConfig.external_notification.enabled && externalNotificationModule) {
                externalNotificationModule->setMute(!externalNotificationModule->getMute());
                IF_SCREEN(if (!externalNotificationModule->getMute()) externalNotificationModule->stopNow();)
            }
        } else if (selected == Backlight) {
#if HAS_BACKLIGHT && defined(USE_EINK)
            graphics::backlightToggle();
            saveUIConfig();
#endif
        } else if (selected == Sleep) {
            screen->setOn(false);
        } else if (selected == Position) {
            service->refreshLocalMeshNode();
            if (service->trySendPosition(NODENUM_BROADCAST, true)) {
                IF_SCREEN(screen->showSimpleBanner("Position\nSent", 3000));
            } else {
                IF_SCREEN(screen->showSimpleBanner("Node Info\nSent", 3000));
            }
        } else if (selected == Preset) {
            cannedMessageModule->LaunchWithDestination(NODENUM_BROADCAST);
        } else if (selected == Freetext) {
            cannedMessageModule->LaunchFreetextWithDestination(NODENUM_BROADCAST);
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::textMessageMenu()
{
    cannedMessageModule->LaunchWithDestination(NODENUM_BROADCAST);
}

// Flips which end of the message list the newest message lives at. The renderer keys its cached
// layout on this, so it relaminates on the next draw; resetScrollState() re-anchors the view on the
// newest message at whichever end that now is, rather than leaving it parked mid-history.
void menuHandler::messageOrderMenu()
{
    enum optionsNumbers { Back, NewestFirst, NewestLast };

    static const char *optionsArray[] = {"Back", "Newest on Top", "Newest on Bottom"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Message Order";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = 3;
    bannerOptions.InitialSelected =
        (config.display.message_order == meshtastic_Config_DisplayConfig_MessageOrder_NEWEST_LAST) ? 2 : 1;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == NewestFirst) {
            config.display.message_order = meshtastic_Config_DisplayConfig_MessageOrder_NEWEST_FIRST;
            graphics::MessageRenderer::resetScrollState();
            service->reloadConfig(SEGMENT_CONFIG);
            LOG_INFO("Message order: newest first");
        } else if (selected == NewestLast) {
            config.display.message_order = meshtastic_Config_DisplayConfig_MessageOrder_NEWEST_LAST;
            graphics::MessageRenderer::resetScrollState();
            service->reloadConfig(SEGMENT_CONFIG);
            LOG_INFO("Message order: newest last");
        } else {
            menuHandler::menuQueue = menuHandler::MessageResponseMenu;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::textMessageBaseMenu()
{
    enum optionsNumbers { Back, Preset, Freetext, enumEnd };

    static const char *optionsArray[enumEnd] = {"Back"};
    static int optionsEnumArray[enumEnd] = {Back};
    int options = 1;
    optionsArray[options] = "New Preset Msg";
    optionsEnumArray[options++] = Preset;
    if (freetextAvailable()) {
        optionsArray[options] = "New Freetext Msg";
        optionsEnumArray[options++] = Freetext;
    }

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Message Action";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsEnumPtr = optionsEnumArray;
    bannerOptions.optionsCount = options;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == Preset) {
            cannedMessageModule->LaunchWithDestination(NODENUM_BROADCAST);
        } else if (selected == Freetext) {
            cannedMessageModule->LaunchFreetextWithDestination(NODENUM_BROADCAST);
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::systemBaseMenu()
{
    enum optionsNumbers { Back, Notifications, ScreenOptions, Bluetooth, WiFiToggle, PowerMenu, Test, enumEnd };
    static const char *optionsArray[enumEnd] = {"Back"};
    static int optionsEnumArray[enumEnd] = {Back};
    int options = 1;

    optionsArray[options] = "Notifications";
    optionsEnumArray[options++] = Notifications;

    optionsArray[options] = "Display Options";
    optionsEnumArray[options++] = ScreenOptions;

    if (currentResolution == ScreenResolution::UltraLow) {
        optionsArray[options] = "Bluetooth";
    } else {
        optionsArray[options] = "Bluetooth Toggle";
    }
    optionsEnumArray[options++] = Bluetooth;
#if HAS_WIFI && !defined(ARCH_PORTDUINO)
    optionsArray[options] = "WiFi Toggle";
    optionsEnumArray[options++] = WiFiToggle;
#endif

    if (currentResolution == ScreenResolution::UltraLow) {
        optionsArray[options] = "Power";
    } else {
        optionsArray[options] = "Reboot/Shutdown";
    }
    optionsEnumArray[options++] = PowerMenu;

    if (test_enabled) {
        optionsArray[options] = "Test Menu";
        optionsEnumArray[options++] = Test;
    }

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "System Action";
    if (currentResolution == ScreenResolution::UltraLow) {
        bannerOptions.message = "System";
    }
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = options;
    bannerOptions.optionsEnumPtr = optionsEnumArray;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == Notifications) {
            menuHandler::menuQueue = menuHandler::BuzzerModeMenuPicker;
            screen->runNow();
        } else if (selected == ScreenOptions) {
            menuHandler::menuQueue = menuHandler::ScreenOptionsMenu;
            screen->runNow();
        } else if (selected == PowerMenu) {
            menuHandler::menuQueue = menuHandler::PowerMenu;
            screen->runNow();
        } else if (selected == Test) {
            menuHandler::menuQueue = menuHandler::TestMenu;
            screen->runNow();
        } else if (selected == Bluetooth) {
            menuQueue = BluetoothToggleMenu;
            screen->runNow();
#if HAS_WIFI && !defined(ARCH_PORTDUINO)
        } else if (selected == WiFiToggle) {
            wifiToggleReturn = SystemBaseMenu;
            menuQueue = WifiToggleMenu;
            screen->runNow();
#endif
        } else if (selected == Back && !test_enabled) {
            test_count++;
            if (test_count > 4) {
                test_enabled = true;
            }
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::favoriteBaseMenu()
{
    enum optionsNumbers { Back, Preset, Freetext, GoToChat, Remove, TraceRoute, enumEnd };

    static const char *optionsArray[enumEnd] = {"Back"};
    static int optionsEnumArray[enumEnd] = {Back};
    int options = 1;

    // Only show "View Conversation" if a message exists with this node
    uint32_t peer = graphics::UIRenderer::currentFavoriteNodeNum;
    bool hasConversation = false;
    for (const auto &m : messageStore.getMessages()) {
        if ((m.sender == peer || m.dest == peer)) {
            hasConversation = true;
            break;
        }
    }
    if (hasConversation) {
        optionsArray[options] = "Go To Chat";
        optionsEnumArray[options++] = GoToChat;
    }
    if (currentResolution == ScreenResolution::UltraLow) {
        optionsArray[options] = "New Preset";
    } else {
        optionsArray[options] = "New Preset Msg";
    }
    optionsEnumArray[options++] = Preset;

    if (freetextAvailable()) {
        optionsArray[options] = "New Freetext Msg";
        optionsEnumArray[options++] = Freetext;
    }

    if (currentResolution != ScreenResolution::UltraLow) {
        optionsArray[options] = "Trace Route";
        optionsEnumArray[options++] = TraceRoute;
    }
    optionsArray[options] = "Remove Favorite";
    optionsEnumArray[options++] = Remove;

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Favorites Action";
    if (currentResolution == ScreenResolution::UltraLow) {
        bannerOptions.message = "Favorites";
    }
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsEnumPtr = optionsEnumArray;
    bannerOptions.optionsCount = options;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == Preset) {
            cannedMessageModule->LaunchWithDestination(graphics::UIRenderer::currentFavoriteNodeNum);
        } else if (selected == Freetext) {
            cannedMessageModule->LaunchFreetextWithDestination(graphics::UIRenderer::currentFavoriteNodeNum);
        }
        // Handle new Go To Thread action
        else if (selected == GoToChat) {
            // Switch thread to direct conversation with this node
            graphics::MessageRenderer::setThreadMode(graphics::MessageRenderer::ThreadMode::DIRECT, -1,
                                                     graphics::UIRenderer::currentFavoriteNodeNum);

            // Manually create and send a UIFrameEvent to trigger the jump
            UIFrameEvent evt;
            evt.action = UIFrameEvent::Action::SWITCH_TO_TEXTMESSAGE;
            screen->handleUIFrameEvent(&evt);
        } else if (selected == Remove) {
            menuHandler::menuQueue = menuHandler::RemoveFavorite;
            screen->runNow();
        } else if (selected == TraceRoute) {
            if (traceRouteModule) {
                traceRouteModule->launch(graphics::UIRenderer::currentFavoriteNodeNum);
            }
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::positionBaseMenu()
{
    enum class PositionAction {
        GpsToggle,
        GpsFormat,
        CompassMenu,
        CompassCalibrate,
        GPSSmartPosition,
        GPSUpdateInterval,
        GPSPositionBroadcast
    };

    static const PositionMenuOption baseOptions[] = {
        {"Back", OptionsAction::Back},
        {"On/Off Toggle", OptionsAction::Select, static_cast<int>(PositionAction::GpsToggle)},
        {"Format", OptionsAction::Select, static_cast<int>(PositionAction::GpsFormat)},
        {"Smart Position", OptionsAction::Select, static_cast<int>(PositionAction::GPSSmartPosition)},
        {"Update Interval", OptionsAction::Select, static_cast<int>(PositionAction::GPSUpdateInterval)},
        {"Broadcast Interval", OptionsAction::Select, static_cast<int>(PositionAction::GPSPositionBroadcast)},
        {"Compass", OptionsAction::Select, static_cast<int>(PositionAction::CompassMenu)},
    };

    static const PositionMenuOption calibrateOptions[] = {
        {"Back", OptionsAction::Back},
        {"On/Off Toggle", OptionsAction::Select, static_cast<int>(PositionAction::GpsToggle)},
        {"Format", OptionsAction::Select, static_cast<int>(PositionAction::GpsFormat)},
        {"Smart Position", OptionsAction::Select, static_cast<int>(PositionAction::GPSSmartPosition)},
        {"Update Interval", OptionsAction::Select, static_cast<int>(PositionAction::GPSUpdateInterval)},
        {"Broadcast Interval", OptionsAction::Select, static_cast<int>(PositionAction::GPSPositionBroadcast)},
        {"Compass", OptionsAction::Select, static_cast<int>(PositionAction::CompassMenu)},
        {"Compass Calibrate", OptionsAction::Select, static_cast<int>(PositionAction::CompassCalibrate)},
    };

    constexpr size_t baseCount = sizeof(baseOptions) / sizeof(baseOptions[0]);
    static std::array<const char *, baseCount> baseLabels{};
#if !MESHTASTIC_EXCLUDE_ACCELEROMETER
    constexpr size_t calibrateCount = sizeof(calibrateOptions) / sizeof(calibrateOptions[0]);
    static std::array<const char *, calibrateCount> calibrateLabels{};
#endif

    auto onSelection = [](const PositionMenuOption &option, int) -> void {
        if (option.action == OptionsAction::Back) {
            return;
        }

        if (!option.hasValue) {
            return;
        }

        auto action = static_cast<PositionAction>(option.value);
        switch (action) {
        case PositionAction::GpsToggle:
            menuQueue = GpsToggleMenu;
            screen->runNow();
            break;
        case PositionAction::GpsFormat:
            menuQueue = GpsFormatMenu;
            screen->runNow();
            break;
        case PositionAction::CompassMenu:
            menuQueue = CompassPointNorthMenu;
            screen->runNow();
            break;
        case PositionAction::CompassCalibrate:
#if !MESHTASTIC_EXCLUDE_ACCELEROMETER
            if (accelerometerThread) {
                accelerometerThread->calibrate(30);
            }
#endif
#if !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C && !MESHTASTIC_EXCLUDE_MAGNETOMETER
            if (magnetometerThread) {
                magnetometerThread->calibrate(30);
            }
#endif
            break;
        case PositionAction::GPSSmartPosition:
            menuQueue = GpsSmartPositionMenu;
            screen->runNow();
            break;
        case PositionAction::GPSUpdateInterval:
            menuQueue = GpsUpdateIntervalMenu;
            screen->runNow();
            break;
        case PositionAction::GPSPositionBroadcast:
            menuQueue = GpsPositionBroadcastMenu;
            screen->runNow();
            break;
        }
    };

    BannerOverlayOptions bannerOptions;
#if !MESHTASTIC_EXCLUDE_ACCELEROMETER
    if (accelerometerThread) {
        bannerOptions = createStaticBannerOptions("GPS Action", calibrateOptions, calibrateLabels, onSelection);
    } else {
        bannerOptions = createStaticBannerOptions("GPS Action", baseOptions, baseLabels, onSelection);
    }
#else
    bannerOptions = createStaticBannerOptions("GPS Action", baseOptions, baseLabels, onSelection);
#endif

    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::environmentTelemetryMenu()
{
#if HAS_TELEMETRY && HAS_SENSOR && !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR
    enum optionsNumbers { Back, Source, enumEnd };

    static const char *optionsArray[] = {"Back", "Source"};
    static int optionsEnumArray[] = {Back, Source};

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Environment";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsEnumPtr = optionsEnumArray;
    bannerOptions.optionsCount = enumEnd;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == Source) {
            menuQueue = EnvironmentTelemetrySourceMenu;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
#endif
}

void menuHandler::environmentTelemetrySourceMenu()
{
#if HAS_TELEMETRY && HAS_SENSOR && !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR
    enum optionsNumbers { Back, LocalSensor, Mesh, FavoritesOnly, enumEnd };
    static const char *optionsArray[enumEnd] = {"Back", "Local Sensor", "Mesh", "Favorite Nodes Only"};
    static int optionsEnumArray[enumEnd] = {Back, LocalSensor, Mesh, FavoritesOnly};

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Source";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsEnumPtr = optionsEnumArray;
    bannerOptions.optionsCount = enumEnd;

    switch (EnvironmentTelemetryModule::getDisplaySource()) {
    case EnvironmentTelemetryModule::DisplaySource::LocalSensor:
        bannerOptions.InitialSelected = LocalSensor;
        break;
    case EnvironmentTelemetryModule::DisplaySource::FavoriteNodesOnly:
        bannerOptions.InitialSelected = FavoritesOnly;
        break;
    case EnvironmentTelemetryModule::DisplaySource::Mesh:
    default:
        bannerOptions.InitialSelected = Mesh;
        break;
    }

    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == Back) {
            menuQueue = EnvironmentTelemetryMenu;
            screen->runNow();
            return;
        }

        if (selected == LocalSensor) {
            EnvironmentTelemetryModule::setDisplaySource(EnvironmentTelemetryModule::DisplaySource::LocalSensor);
        } else if (selected == Mesh) {
            EnvironmentTelemetryModule::setDisplaySource(EnvironmentTelemetryModule::DisplaySource::Mesh);
        } else if (selected == FavoritesOnly) {
            EnvironmentTelemetryModule::setDisplaySource(EnvironmentTelemetryModule::DisplaySource::FavoriteNodesOnly);
        }

        screen->runNow();
    };
    screen->showOverlayBanner(bannerOptions);
#endif
}

void menuHandler::nodeListMenu()
{
    enum optionsNumbers { Back, NodePicker, TraceRoute, Verify, Reset, NodeNameLength, enumEnd };
    static const char *optionsArray[enumEnd] = {"Back"};
    static int optionsEnumArray[enumEnd] = {Back};
    int options = 1;

#if defined(OLED_TINY)
    optionsArray[options] = "Node Action";
#else
    optionsArray[options] = "Node Actions / Settings";
#endif
    optionsEnumArray[options++] = NodePicker;

    if (currentResolution != ScreenResolution::UltraLow) {
        optionsArray[options] = "Show Long/Short Name";
        optionsEnumArray[options++] = NodeNameLength;
    }
    optionsArray[options] = "Reset NodeDB";
    optionsEnumArray[options++] = Reset;

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Node Action";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = options;
    bannerOptions.optionsEnumPtr = optionsEnumArray;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == NodePicker) {
            menuQueue = NodePickerMenu;
            screen->runNow();
        } else if (selected == Reset) {
            menuQueue = ResetNodeDbMenu;
            screen->runNow();
        } else if (selected == NodeNameLength) {
            menuHandler::menuQueue = menuHandler::NodeNameLengthMenu;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::NodePicker()
{
    const char *NODE_PICKER_TITLE;
    if (currentResolution == ScreenResolution::UltraLow) {
        NODE_PICKER_TITLE = "Pick Node";
    } else {
        NODE_PICKER_TITLE = "Pick A Node";
    }
    screen->showNodePicker(NODE_PICKER_TITLE, 30000, [](uint32_t nodenum) -> void {
        LOG_INFO("Nodenum: %u", nodenum);
        // Store the selection so the Manage Node menu knows which node to operate on
        menuHandler::pickedNodeNum = nodenum;
        // Keep UI favorite context in sync (used elsewhere for some node-based actions)
        graphics::UIRenderer::currentFavoriteNodeNum = nodenum;
        menuQueue = ManageNodeMenu;
        screen->runNow();
    });
}

void menuHandler::manageNodeMenu()
{
    // If we don't have a node selected yet, go fast exit
    auto node = nodeDB->getMeshNode(menuHandler::pickedNodeNum);
    if (!node) {
        return;
    }
    enum optionsNumbers { Back, Favorite, Mute, TraceRoute, KeyVerification, Ignore, enumEnd };
    static const char *optionsArray[enumEnd] = {"Back"};
    static int optionsEnumArray[enumEnd] = {Back};
    int options = 1;

    if (nodeInfoLiteIsFavorite(node)) {
        optionsArray[options] = "Unfavorite";
    } else {
        optionsArray[options] = "Favorite";
    }
    optionsEnumArray[options++] = Favorite;

    bool isMuted = nodeInfoLiteIsMuted(node);
    if (isMuted) {
        optionsArray[options] = "Unmute Notifications";
    } else {
        optionsArray[options] = "Mute Notifications";
    }
    optionsEnumArray[options++] = Mute;

    optionsArray[options] = "Trace Route";
    optionsEnumArray[options++] = TraceRoute;

    optionsArray[options] = "Key Verification";
    optionsEnumArray[options++] = KeyVerification;

    if (nodeInfoLiteIsIgnored(node)) {
        optionsArray[options] = "Unignore Node";
    } else {
        optionsArray[options] = "Ignore Node";
    }
    optionsEnumArray[options++] = Ignore;

    BannerOverlayOptions bannerOptions;

    std::string title = "";
    if (nodeInfoLiteHasUser(node) && node->long_name[0]) {
        title += sanitizeString(node->long_name).substr(0, 15);
    } else {
        char buf[20];
        snprintf(buf, sizeof(buf), "!%08x", (unsigned int)node->num);
        title += buf;
    }
    bannerOptions.message = title.c_str();
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = options;
    bannerOptions.optionsEnumPtr = optionsEnumArray;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == Back) {
            menuQueue = NodeBaseMenu;
            screen->runNow();
            return;
        }

        if (selected == Favorite) {
            const auto *n = nodeDB->getMeshNode(menuHandler::pickedNodeNum);
            if (!n) {
                return;
            }
            if (nodeInfoLiteIsFavorite(n)) {
                LOG_INFO("Removing node 0x%08x from favorites", menuHandler::pickedNodeNum);
                nodeDB->set_favorite(false, menuHandler::pickedNodeNum);
            } else {
                LOG_INFO("Adding node 0x%08x to favorites", menuHandler::pickedNodeNum);
                // set_favorite() already logs PROTECTED_CAP_WARN_FMT on a cap refusal; don't double-log here.
                nodeDB->set_favorite(true, menuHandler::pickedNodeNum);
            }
            screen->setFrames(graphics::Screen::FOCUS_PRESERVE);
            return;
        }

        if (selected == Mute) {
            // No lookup or null check here: toggleNodeMuted() resolves the node itself and returns
            // without writing if it is unknown.
            menuHandler::toggleNodeMuted(menuHandler::pickedNodeNum);
            screen->setFrames(graphics::Screen::FOCUS_PRESERVE);
            return;
        }

        if (selected == TraceRoute) {
            LOG_INFO("Starting traceroute to 0x%08x", menuHandler::pickedNodeNum);
            if (traceRouteModule) {
                traceRouteModule->startTraceRoute(menuHandler::pickedNodeNum);
            }
            return;
        }

        if (selected == KeyVerification) {
            LOG_INFO("Initiating key verification with 0x%08x", menuHandler::pickedNodeNum);
            if (keyVerificationModule) {
                keyVerificationModule->sendInitialRequest(menuHandler::pickedNodeNum);
            }
            return;
        }

        if (selected == Ignore) {
            auto n = nodeDB->getMeshNode(menuHandler::pickedNodeNum);
            if (!n) {
                return;
            }

            bool changed = false;
            if (nodeInfoLiteIsIgnored(n)) {
                nodeInfoLiteSetBit(n, NODEINFO_BITFIELD_IS_IGNORED_MASK, false);
                LOG_INFO("Unignoring node 0x%08x", menuHandler::pickedNodeNum);
                changed = true;
            } else if (nodeDB->setProtectedFlag(n, NODEINFO_BITFIELD_IS_IGNORED_MASK, true)) {
                LOG_INFO("Ignoring node 0x%08x", menuHandler::pickedNodeNum);
                changed = true;
            } else {
                LOG_WARN(NodeDB::PROTECTED_CAP_WARN_FMT, "ignore", menuHandler::pickedNodeNum, MAX_NUM_NODES - 2);
            }
            // Only persist/notify when the ignore bit actually moved; a cap
            // refusal changed nothing and shouldn't trigger a prefs save.
            if (changed) {
                nodeDB->notifyObservers(true);
                nodeDB->saveToDisk();
            }
            screen->setFrames(graphics::Screen::FOCUS_PRESERVE);
            return;
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::nodeNameLengthMenu()
{
    static const NodeNameOption nodeNameOptions[] = {
        {"Back", OptionsAction::Back},
        {"Long", OptionsAction::Select, true},
        {"Short", OptionsAction::Select, false},
    };

    constexpr size_t nodeNameCount = sizeof(nodeNameOptions) / sizeof(nodeNameOptions[0]);
    static std::array<const char *, nodeNameCount> nodeNameLabels{};

    auto bannerOptions = createStaticBannerOptions("Node Name Length", nodeNameOptions, nodeNameLabels,
                                                   [](const NodeNameOption &option, int) -> void {
                                                       if (option.action == OptionsAction::Back) {
                                                           menuQueue = NodeBaseMenu;
                                                           screen->runNow();
                                                           return;
                                                       }

                                                       if (!option.hasValue) {
                                                           return;
                                                       }

                                                       if (config.display.use_long_node_name == option.value) {
                                                           return;
                                                       }

                                                       config.display.use_long_node_name = option.value;
                                                       saveUIConfig();
                                                       service->reloadConfig(SEGMENT_CONFIG);
                                                       LOG_INFO("Setting names to %s", option.value ? "long" : "short");
                                                   });

    int initialSelection = config.display.use_long_node_name ? 1 : 2;
    bannerOptions.InitialSelected = initialSelection;

    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::resetNodeDBMenu()
{
    static const char *optionsArray[] = {"Back", "Reset All", "Preserve Favorites"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Confirm Reset NodeDB";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = 3;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == 1 || selected == 2) {
            screen->setFrames(Screen::FOCUS_DEFAULT);
        }
        if (selected == 1) {
            LOG_INFO("Initiate node-db reset");
            nodeDB->resetNodes();
            disableBluetooth();
            rebootAtMsec = Time::timerEndsAtMillis(DEFAULT_REBOOT_SECONDS * 1000);
        } else if (selected == 2) {
            LOG_INFO("Initiate node-db reset, keep favorites");
            nodeDB->resetNodes(1);
            disableBluetooth();
            rebootAtMsec = Time::timerEndsAtMillis(DEFAULT_REBOOT_SECONDS * 1000);
        } else if (selected == 0) {
            menuQueue = NodeBaseMenu;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::compassNorthMenu()
{
    static const CompassOption compassOptions[] = {
        {"Back", OptionsAction::Back},
        {"Dynamic", OptionsAction::Select, meshtastic_CompassMode_DYNAMIC},
        {"Fixed Ring", OptionsAction::Select, meshtastic_CompassMode_FIXED_RING},
        {"Freeze Heading", OptionsAction::Select, meshtastic_CompassMode_FREEZE_HEADING},
    };

    constexpr size_t compassCount = sizeof(compassOptions) / sizeof(compassOptions[0]);
    static std::array<const char *, compassCount> compassLabels{};

    auto bannerOptions = createStaticBannerOptions("North Directions?", compassOptions, compassLabels,
                                                   [](const CompassOption &option, int) -> void {
                                                       if (option.action == OptionsAction::Back) {
                                                           menuQueue = PositionBaseMenu;
                                                           screen->runNow();
                                                           return;
                                                       }

                                                       if (!option.hasValue) {
                                                           return;
                                                       }

                                                       if (uiconfig.compass_mode == option.value) {
                                                           return;
                                                       }

                                                       uiconfig.compass_mode = option.value;
                                                       saveUIConfig();
                                                       screen->setFrames(graphics::Screen::FOCUS_PRESERVE);
                                                   });

    int initialSelection = 0;
    for (size_t i = 0; i < compassCount; ++i) {
        if (compassOptions[i].hasValue && uiconfig.compass_mode == compassOptions[i].value) {
            initialSelection = static_cast<int>(i);
            break;
        }
    }
    bannerOptions.InitialSelected = initialSelection;

    screen->showOverlayBanner(bannerOptions);
}

#if !MESHTASTIC_EXCLUDE_GPS
void menuHandler::GPSToggleMenu()
{
    static const GPSToggleOption gpsToggleOptions[] = {
        {"Back", OptionsAction::Back},
        {"Enabled", OptionsAction::Select, meshtastic_Config_PositionConfig_GpsMode_ENABLED},
        {"Disabled", OptionsAction::Select, meshtastic_Config_PositionConfig_GpsMode_DISABLED},
    };

    constexpr size_t toggleCount = sizeof(gpsToggleOptions) / sizeof(gpsToggleOptions[0]);
    static std::array<const char *, toggleCount> toggleLabels{};

    auto bannerOptions =
        createStaticBannerOptions("Toggle GPS", gpsToggleOptions, toggleLabels, [](const GPSToggleOption &option, int) -> void {
            if (option.action == OptionsAction::Back) {
                menuQueue = PositionBaseMenu;
                screen->runNow();
                return;
            }

            if (!option.hasValue) {
                return;
            }

            if (config.position.gps_mode == option.value) {
                return;
            }

            config.position.gps_mode = option.value;
            if (option.value == meshtastic_Config_PositionConfig_GpsMode_ENABLED) {
                playGPSEnableBeep();
                gps->enable();
            } else {
                playGPSDisableBeep();
                gps->disable();
            }
            service->reloadConfig(SEGMENT_CONFIG);
        });

    int initialSelection = 0;
    for (size_t i = 0; i < toggleCount; ++i) {
        if (gpsToggleOptions[i].hasValue && config.position.gps_mode == gpsToggleOptions[i].value) {
            initialSelection = static_cast<int>(i);
            break;
        }
    }
    bannerOptions.InitialSelected = initialSelection;

    screen->showOverlayBanner(bannerOptions);
}
void menuHandler::GPSFormatMenu()
{
    static const GPSFormatOption formatOptionsHigh[] = {
        {"Back", OptionsAction::Back},
        {"Decimal Degrees", OptionsAction::Select, meshtastic_DeviceUIConfig_GpsCoordinateFormat_DEC},
        {"Degrees Minutes Seconds", OptionsAction::Select, meshtastic_DeviceUIConfig_GpsCoordinateFormat_DMS},
        {"Universal Transverse Mercator", OptionsAction::Select, meshtastic_DeviceUIConfig_GpsCoordinateFormat_UTM},
        {"Military Grid Reference System", OptionsAction::Select, meshtastic_DeviceUIConfig_GpsCoordinateFormat_MGRS},
        {"Open Location Code", OptionsAction::Select, meshtastic_DeviceUIConfig_GpsCoordinateFormat_OLC},
        {"Ordnance Survey Grid Ref", OptionsAction::Select, meshtastic_DeviceUIConfig_GpsCoordinateFormat_OSGR},
        {"Maidenhead Locator", OptionsAction::Select, meshtastic_DeviceUIConfig_GpsCoordinateFormat_MLS},
    };

    static const GPSFormatOption formatOptionsLow[] = {
        {"Back", OptionsAction::Back},
        {"DEC", OptionsAction::Select, meshtastic_DeviceUIConfig_GpsCoordinateFormat_DEC},
        {"DMS", OptionsAction::Select, meshtastic_DeviceUIConfig_GpsCoordinateFormat_DMS},
        {"UTM", OptionsAction::Select, meshtastic_DeviceUIConfig_GpsCoordinateFormat_UTM},
        {"MGRS", OptionsAction::Select, meshtastic_DeviceUIConfig_GpsCoordinateFormat_MGRS},
        {"OLC", OptionsAction::Select, meshtastic_DeviceUIConfig_GpsCoordinateFormat_OLC},
        {"OSGR", OptionsAction::Select, meshtastic_DeviceUIConfig_GpsCoordinateFormat_OSGR},
        {"MLS", OptionsAction::Select, meshtastic_DeviceUIConfig_GpsCoordinateFormat_MLS},
    };

    constexpr size_t formatCount = sizeof(formatOptionsHigh) / sizeof(formatOptionsHigh[0]);
    static std::array<const char *, formatCount> formatLabelsHigh{};
    static std::array<const char *, formatCount> formatLabelsLow{};

    auto onSelection = [](const GPSFormatOption &option, int) -> void {
        if (option.action == OptionsAction::Back) {
            menuQueue = PositionBaseMenu;
            screen->runNow();
            return;
        }

        if (!option.hasValue) {
            return;
        }

        if (uiconfig.gps_format == option.value) {
            return;
        }

        uiconfig.gps_format = option.value;
        saveUIConfig();
        service->reloadConfig(SEGMENT_CONFIG);
    };

    BannerOverlayOptions bannerOptions;
    int initialSelection = 0;

    if (currentResolution == ScreenResolution::High) {
        bannerOptions = createStaticBannerOptions("GPS Format", formatOptionsHigh, formatLabelsHigh, onSelection);
        for (size_t i = 0; i < formatCount; ++i) {
            if (formatOptionsHigh[i].hasValue && uiconfig.gps_format == formatOptionsHigh[i].value) {
                initialSelection = static_cast<int>(i);
                break;
            }
        }
    } else {
        bannerOptions = createStaticBannerOptions("GPS Format", formatOptionsLow, formatLabelsLow, onSelection);
        for (size_t i = 0; i < formatCount; ++i) {
            if (formatOptionsLow[i].hasValue && uiconfig.gps_format == formatOptionsLow[i].value) {
                initialSelection = static_cast<int>(i);
                break;
            }
        }
    }

    bannerOptions.InitialSelected = initialSelection;
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::GPSSmartPositionMenu()
{
    static const char *optionsArray[] = {"Back", "Enabled", "Disabled"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Toggle Smart Position";
    if (currentResolution == ScreenResolution::UltraLow) {
        bannerOptions.message = "Smrt Postn";
    }
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = 3;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == 0) {
            menuQueue = PositionBaseMenu;
            screen->runNow();
        } else if (selected == 1) {
            config.position.position_broadcast_smart_enabled = true;
            saveUIConfig();
            service->reloadConfig(SEGMENT_CONFIG);
            rebootAtMsec = Time::timerEndsAtMillis(DEFAULT_REBOOT_SECONDS * 1000);
        } else if (selected == 2) {
            config.position.position_broadcast_smart_enabled = false;
            saveUIConfig();
            service->reloadConfig(SEGMENT_CONFIG);
            rebootAtMsec = Time::timerEndsAtMillis(DEFAULT_REBOOT_SECONDS * 1000);
        }
    };
    bannerOptions.InitialSelected = config.position.position_broadcast_smart_enabled ? 1 : 2;
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::GPSUpdateIntervalMenu()
{
    static const char *optionsArray[] = {"Back",      "8 seconds", "20 seconds", "40 seconds",  "1 minute",   "80 seconds",
                                         "2 minutes", "5 minutes", "10 minutes", "15 minutes",  "30 minutes", "1 hour",
                                         "6 hours",   "12 hours",  "24 hours",   "At Boot Only"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Update Interval";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = 16;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == 0) {
            menuQueue = PositionBaseMenu;
            screen->runNow();
        } else if (selected == 1) {
            config.position.gps_update_interval = 8;
        } else if (selected == 2) {
            config.position.gps_update_interval = 20;
        } else if (selected == 3) {
            config.position.gps_update_interval = 40;
        } else if (selected == 4) {
            config.position.gps_update_interval = 60;
        } else if (selected == 5) {
            config.position.gps_update_interval = 80;
        } else if (selected == 6) {
            config.position.gps_update_interval = 120;
        } else if (selected == 7) {
            config.position.gps_update_interval = 300;
        } else if (selected == 8) {
            config.position.gps_update_interval = 600;
        } else if (selected == 9) {
            config.position.gps_update_interval = 900;
        } else if (selected == 10) {
            config.position.gps_update_interval = 1800;
        } else if (selected == 11) {
            config.position.gps_update_interval = 3600;
        } else if (selected == 12) {
            config.position.gps_update_interval = 21600;
        } else if (selected == 13) {
            config.position.gps_update_interval = 43200;
        } else if (selected == 14) {
            config.position.gps_update_interval = 86400;
        } else if (selected == 15) {
            config.position.gps_update_interval = 2147483647; // At Boot Only
        }

        if (selected != 0) {
            saveUIConfig();
            service->reloadConfig(SEGMENT_CONFIG);
            rebootAtMsec = Time::timerEndsAtMillis(DEFAULT_REBOOT_SECONDS * 1000);
        }
    };

    if (config.position.gps_update_interval == 8) {
        bannerOptions.InitialSelected = 1;
    } else if (config.position.gps_update_interval == 20) {
        bannerOptions.InitialSelected = 2;
    } else if (config.position.gps_update_interval == 40) {
        bannerOptions.InitialSelected = 3;
    } else if (config.position.gps_update_interval == 60) {
        bannerOptions.InitialSelected = 4;
    } else if (config.position.gps_update_interval == 80) {
        bannerOptions.InitialSelected = 5;
    } else if (config.position.gps_update_interval == 120) {
        bannerOptions.InitialSelected = 6;
    } else if (config.position.gps_update_interval == 300) {
        bannerOptions.InitialSelected = 7;
    } else if (config.position.gps_update_interval == 600) {
        bannerOptions.InitialSelected = 8;
    } else if (config.position.gps_update_interval == 900) {
        bannerOptions.InitialSelected = 9;
    } else if (config.position.gps_update_interval == 1800) {
        bannerOptions.InitialSelected = 10;
    } else if (config.position.gps_update_interval == 3600) {
        bannerOptions.InitialSelected = 11;
    } else if (config.position.gps_update_interval == 21600) {
        bannerOptions.InitialSelected = 12;
    } else if (config.position.gps_update_interval == 43200) {
        bannerOptions.InitialSelected = 13;
    } else if (config.position.gps_update_interval == 86400) {
        bannerOptions.InitialSelected = 14;
    } else if (config.position.gps_update_interval == 2147483647) { // At Boot Only
        bannerOptions.InitialSelected = 15;
    } else {
        bannerOptions.InitialSelected = 0;
    }
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::GPSPositionBroadcastMenu()
{
    static const char *optionsArray[] = {"Back",     "1 minute", "90 seconds", "5 minutes", "15 minutes", "1 hour",
                                         "2 hours",  "3 hours",  "4 hours",    "5 hours",   "6 hours",    "12 hours",
                                         "18 hours", "24 hours", "36 hours",   "48 hours",  "72 hours"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Broadcast Interval";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = 17;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == 0) {
            menuQueue = PositionBaseMenu;
            screen->runNow();
        } else if (selected == 1) {
            config.position.position_broadcast_secs = 60;
        } else if (selected == 2) {
            config.position.position_broadcast_secs = 90;
        } else if (selected == 3) {
            config.position.position_broadcast_secs = 300;
        } else if (selected == 4) {
            config.position.position_broadcast_secs = 900;
        } else if (selected == 5) {
            config.position.position_broadcast_secs = 3600;
        } else if (selected == 6) {
            config.position.position_broadcast_secs = 7200;
        } else if (selected == 7) {
            config.position.position_broadcast_secs = 10800;
        } else if (selected == 8) {
            config.position.position_broadcast_secs = 14400;
        } else if (selected == 9) {
            config.position.position_broadcast_secs = 18000;
        } else if (selected == 10) {
            config.position.position_broadcast_secs = 21600;
        } else if (selected == 11) {
            config.position.position_broadcast_secs = 43200;
        } else if (selected == 12) {
            config.position.position_broadcast_secs = 64800;
        } else if (selected == 13) {
            config.position.position_broadcast_secs = 86400;
        } else if (selected == 14) {
            config.position.position_broadcast_secs = 129600;
        } else if (selected == 15) {
            config.position.position_broadcast_secs = 172800;
        } else if (selected == 16) {
            config.position.position_broadcast_secs = 259200;
        }

        if (selected != 0) {
            saveUIConfig();
            service->reloadConfig(SEGMENT_CONFIG);
            rebootAtMsec = Time::timerEndsAtMillis(DEFAULT_REBOOT_SECONDS * 1000);
        }
    };

    if (config.position.position_broadcast_secs == 60) {
        bannerOptions.InitialSelected = 1;
    } else if (config.position.position_broadcast_secs == 90) {
        bannerOptions.InitialSelected = 2;
    } else if (config.position.position_broadcast_secs == 300) {
        bannerOptions.InitialSelected = 3;
    } else if (config.position.position_broadcast_secs == 900) {
        bannerOptions.InitialSelected = 4;
    } else if (config.position.position_broadcast_secs == 3600) {
        bannerOptions.InitialSelected = 5;
    } else if (config.position.position_broadcast_secs == 7200) {
        bannerOptions.InitialSelected = 6;
    } else if (config.position.position_broadcast_secs == 10800) {
        bannerOptions.InitialSelected = 7;
    } else if (config.position.position_broadcast_secs == 14400) {
        bannerOptions.InitialSelected = 8;
    } else if (config.position.position_broadcast_secs == 18000) {
        bannerOptions.InitialSelected = 9;
    } else if (config.position.position_broadcast_secs == 21600) {
        bannerOptions.InitialSelected = 10;
    } else if (config.position.position_broadcast_secs == 43200) {
        bannerOptions.InitialSelected = 11;
    } else if (config.position.position_broadcast_secs == 64800) {
        bannerOptions.InitialSelected = 12;
    } else if (config.position.position_broadcast_secs == 86400) {
        bannerOptions.InitialSelected = 13;
    } else if (config.position.position_broadcast_secs == 129600) {
        bannerOptions.InitialSelected = 14;
    } else if (config.position.position_broadcast_secs == 172800) {
        bannerOptions.InitialSelected = 15;
    } else if (config.position.position_broadcast_secs == 259200) {
        bannerOptions.InitialSelected = 16;
    } else {
        bannerOptions.InitialSelected = 0;
    }
    screen->showOverlayBanner(bannerOptions);
}

#endif

void menuHandler::bluetoothToggleMenu()
{
    static const char *optionsArray[] = {"Back", "Enabled", "Disabled"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Toggle Bluetooth";
    if (currentResolution == ScreenResolution::UltraLow) {
        bannerOptions.message = "Bluetooth";
    }
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = 3;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == 0) {
            menuQueue = SystemBaseMenu;
            screen->runNow();
            return;
        } else if (selected != (config.bluetooth.enabled ? 1 : 2)) {
            InputEvent event = {.inputEvent = (input_broker_event)170, .kbchar = 170, .touchX = 0, .touchY = 0};
            inputBroker->injectInputEvent(&event);
        }
    };
    bannerOptions.InitialSelected = config.bluetooth.enabled ? 1 : 2;
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::BuzzerModeMenu()
{
    static const char *optionsArray[] = {"All Enabled", "All Disabled", "Notifications", "System Only", "DMs Only"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Notification Sounds";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = 5;
    bannerOptions.bannerCallback = [](int selected) -> void {
        config.device.buzzer_mode = (meshtastic_Config_DeviceConfig_BuzzerMode)selected;
        service->reloadConfig(SEGMENT_CONFIG);
    };
    bannerOptions.InitialSelected = config.device.buzzer_mode;
    screen->showOverlayBanner(bannerOptions);
}

// Variants may override these in variant.h.
#ifndef SCREEN_BRIGHTNESS_LEVEL_MEDIUM
#define SCREEN_BRIGHTNESS_LEVEL_MEDIUM 64
#endif
#ifndef SCREEN_BRIGHTNESS_LEVEL_HIGH
#define SCREEN_BRIGHTNESS_LEVEL_HIGH 128
#endif
#ifndef SCREEN_BRIGHTNESS_LEVEL_VERY_HIGH
#define SCREEN_BRIGHTNESS_LEVEL_VERY_HIGH 255
#endif

void menuHandler::BrightnessPickerMenu()
{
    static const char *optionsArray[] = {"Back", "Low", "Medium", "High"};

    // Get current brightness level to set initial selection
    int currentSelection = 1; // Default to Medium
    if (uiconfig.screen_brightness >= SCREEN_BRIGHTNESS_LEVEL_VERY_HIGH) {
        currentSelection = 3; // Very High
    } else if (uiconfig.screen_brightness >= SCREEN_BRIGHTNESS_LEVEL_HIGH) {
        currentSelection = 2; // High
    } else {
        currentSelection = 1; // Medium
    }

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Brightness";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = 4;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == 1) { // Medium
            uiconfig.screen_brightness = SCREEN_BRIGHTNESS_LEVEL_MEDIUM;
        } else if (selected == 2) { // High
            uiconfig.screen_brightness = SCREEN_BRIGHTNESS_LEVEL_HIGH;
        } else if (selected == 3) { // Very High
            uiconfig.screen_brightness = SCREEN_BRIGHTNESS_LEVEL_VERY_HIGH;
        }

        if (selected == 0) { // Back
            menuHandler::menuQueue = menuHandler::ScreenOptionsMenu;
            screen->runNow();
            return;
        }
        if (selected != 0) { // Not "Back"
            // Through Screen, so its own copy of the level is updated too: every wake re-applies that, and
            // setting the panel directly here left the next wake restoring the level from before the pick.
            screen->applyBrightness(uiconfig.screen_brightness);

            // Save to device
            saveUIConfig();

            LOG_INFO("Screen brightness set to %d", uiconfig.screen_brightness);
        }
    };
    bannerOptions.InitialSelected = currentSelection;
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::switchToMUIMenu()
{
    static const char *optionsArray[] = {"No", "Yes"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Switch to MUI?";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = 2;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == 1) {
            config.display.displaymode = meshtastic_Config_DisplayConfig_DisplayMode_COLOR;
            config.bluetooth.enabled = false;
            service->reloadConfig(SEGMENT_CONFIG);
            rebootAtMsec = Time::timerEndsAtMillis(DEFAULT_REBOOT_SECONDS * 1000);
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::rebootMenu()
{
    static const char *optionsArray[] = {"Back", "Confirm"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Reboot Device?";
    if (currentResolution == ScreenResolution::UltraLow) {
        bannerOptions.message = "Reboot";
    }
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = 2;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == 1) {
            IF_SCREEN(screen->showSimpleBanner("Rebooting...", 0));
            nodeDB->saveToDisk();
            messageStore.saveToFlash();
            rebootAtMsec = Time::timerEndsAtMillis(DEFAULT_REBOOT_SECONDS * 1000);
        } else {
            menuQueue = PowerMenu;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::shutdownMenu()
{
    static const char *optionsArray[] = {"Back", "Confirm"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Shutdown Device?";
    if (currentResolution == ScreenResolution::UltraLow) {
        bannerOptions.message = "Shutdown";
    }
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = 2;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == 1) {
            InputEvent event = {.inputEvent = (input_broker_event)INPUT_BROKER_SHUTDOWN, .kbchar = 0, .touchX = 0, .touchY = 0};
            inputBroker->injectInputEvent(&event);
        } else {
            menuQueue = PowerMenu;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::removeFavoriteMenu()
{

    static const char *optionsArray[] = {"Back", "Yes"};
    BannerOverlayOptions bannerOptions;
    std::string message = "Unfavorite This Node?\n";
    const auto *node = nodeDB->getMeshNode(graphics::UIRenderer::currentFavoriteNodeNum);
    if (nodeInfoLiteHasUser(node)) {
        message += sanitizeString(node->long_name).substr(0, 15);
    }
    bannerOptions.message = message.c_str();
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = 2;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == 1) {
            LOG_INFO("Removing %x as favorite node", graphics::UIRenderer::currentFavoriteNodeNum);
            nodeDB->set_favorite(false, graphics::UIRenderer::currentFavoriteNodeNum);
            screen->setFrames(graphics::Screen::FOCUS_DEFAULT);
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::waypointBaseMenu()
{
    enum optionsNumbers { Back, GeofenceAlerts, RemoveWaypoint, NewHere };
#if BASEUI_WAYPOINT_EDITOR
    static const char *optionsArray[] = {"Back", "Geofence Alerts", "Remove Waypoint", "New Waypoint Here"};
#else
    static const char *optionsArray[] = {"Back", "Geofence Alerts", "Remove Waypoint"};
#endif

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Waypoint Action";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = sizeof(optionsArray) / sizeof(optionsArray[0]);
    bannerOptions.bannerCallback = [](int selected) -> void {
#if BASEUI_WAYPOINT_EDITOR
        if (selected == NewHere) {
            newWaypointHere();
            return;
        }
#endif
        if (selected == GeofenceAlerts) {
            menuQueue = GeofenceWaypointMenu;
            screen->runNow();
        } else if (selected == RemoveWaypoint) {
            menuQueue = RemoveWaypointMenu;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

#if BASEUI_WAYPOINT_EDITOR
namespace
{
struct ExpiryChoice {
    const char *label;
    uint32_t seconds;
};
const ExpiryChoice kWaypointExpiries[] = {{"Never", 0},          {"1 hour", 3600},      {"8 hours", 8 * 3600},  {"1 day", 86400},
                                          {"3 days", 3 * 86400}, {"1 week", 7 * 86400}, {"30 days", 30 * 86400}};

constexpr uint32_t kWaypointPushpin = 0x1F4CD; // the emote set's pushpin, and the default pin

const char *expiryLabel(uint32_t seconds)
{
    for (const auto &choice : kWaypointExpiries)
        if (choice.seconds == seconds)
            return choice.label;
    return "Never";
}
} // namespace

void menuHandler::newWaypointHere(bool fromMap)
{
    double lat = 0, lng = 0;
#if BASEUI_MAP_NAVIGATION
    const bool atCenter = fromMap && graphics::MapRenderer::pannedCenter(lat, lng);
#else
    const bool atCenter = false;
#endif
    if (!atCenter && localPosition.latitude_i == 0 && localPosition.longitude_i == 0) {
        queueNotice("No GPS position yet");
        return;
    }
    memset(&waypointDraft, 0, sizeof(waypointDraft));
    waypointDraft.atMapCenter = atCenter;
    waypointDraft.latitudeI = (int32_t)lround(lat * 1e7);
    waypointDraft.longitudeI = (int32_t)lround(lng * 1e7);
    waypointDraft.icon = kWaypointPushpin;
    waypointDraft.expireSecs = 86400; // a day: long enough to be useful, short enough not to litter the map
    menuQueue = WaypointEditorMenu;
    screen->runNow();
}

// Each row shows what it is set to; picking one edits it and comes back here.
void menuHandler::waypointEditorMenu()
{
    enum Row { Back, Send, Name, Note, Pin, Expires, RowCount };
    static char nameLabel[48], noteLabel[48], expiryLabelText[24], pinLabel[16];
    static const char *labels[RowCount];
    snprintf(nameLabel, sizeof(nameLabel), "Name: %.28s", waypointDraft.name[0] ? waypointDraft.name : "(none)");
    snprintf(noteLabel, sizeof(noteLabel), "Note: %.28s", waypointDraft.description[0] ? waypointDraft.description : "(none)");
    snprintf(expiryLabelText, sizeof(expiryLabelText), "Expires: %s", expiryLabel(waypointDraft.expireSecs));
    labels[Back] = "Back";
    labels[Send] = "Send";
    labels[Name] = nameLabel;
    labels[Note] = noteLabel;
    // The pin itself, drawn as its emote by the menu.
    snprintf(pinLabel, sizeof(pinLabel), "Pin: %s",
             waypointDraft.icon ? WaypointUtils::utf8FromCodepoint(waypointDraft.icon).c_str() : "none");
    labels[Pin] = pinLabel;
    labels[Expires] = expiryLabelText;

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = waypointDraft.atMapCenter ? "Waypoint at Map Center" : "New Waypoint";
    bannerOptions.optionsArrayPtr = labels;
    bannerOptions.optionsCount = RowCount;
    bannerOptions.InitialSelected = Send;
    bannerOptions.bannerCallback = [](int selected) -> void {
        switch (selected) {
        case Name:
            menuQueue = WaypointNamePrompt;
            break;
        case Note:
            menuQueue = WaypointNotePrompt;
            break;
        case Pin:
            menuQueue = WaypointPinPicker;
            break;
        case Expires:
            menuQueue = WaypointExpiryMenu;
            break;
        case Send: {
            if (!waypointDraft.atMapCenter && localPosition.latitude_i == 0 && localPosition.longitude_i == 0) {
                queueNotice("No GPS position yet");
                return;
            }
            uint32_t expire = 0;
            if (waypointDraft.expireSecs) {
                // An expiry is a date, so it needs the clock: a guess would expire it at once, or never.
                const uint32_t now = getValidTime(RTCQualityDevice);
                if (!now) {
                    queueNotice("Clock not set: pick Never");
                    return;
                }
                expire = now + waypointDraft.expireSecs;
            }
            meshtastic_Waypoint wp = meshtastic_Waypoint_init_zero;
            wp.id = (uint32_t)random(1, 0x7FFFFFFF);
            wp.has_latitude_i = true;
            wp.latitude_i = waypointDraft.atMapCenter ? waypointDraft.latitudeI : localPosition.latitude_i;
            wp.has_longitude_i = true;
            wp.longitude_i = waypointDraft.atMapCenter ? waypointDraft.longitudeI : localPosition.longitude_i;
            wp.expire = expire;
            wp.icon = waypointDraft.icon;
            strncpy(wp.name, waypointDraft.name[0] ? waypointDraft.name : "Waypoint", sizeof(wp.name) - 1);
            strncpy(wp.description, waypointDraft.description, sizeof(wp.description) - 1);
            queueNotice(waypointModule && waypointModule->broadcastNew(wp) ? "Waypoint sent" : "Couldn't send the waypoint");
            return;
        }
        default:
            return; // Back: the draft is dropped
        }
        screen->runNow();
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::waypointExpiryMenu()
{
    constexpr int kCount = sizeof(kWaypointExpiries) / sizeof(kWaypointExpiries[0]);
    static const char *labels[kCount + 1];
    labels[0] = "Back";
    int current = 1;
    for (int i = 0; i < kCount; i++) {
        labels[i + 1] = kWaypointExpiries[i].label;
        if (kWaypointExpiries[i].seconds == waypointDraft.expireSecs)
            current = i + 1;
    }
    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Expires";
    bannerOptions.optionsArrayPtr = labels;
    bannerOptions.optionsCount = kCount + 1;
    bannerOptions.InitialSelected = current;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected >= 1 && selected <= kCount)
            waypointDraft.expireSecs = kWaypointExpiries[selected - 1].seconds;
        menuQueue = WaypointEditorMenu;
        screen->runNow();
    };
    screen->showOverlayBanner(bannerOptions);
}
#endif

void menuHandler::geofenceWaypointMenu()
{
#if MESHTASTIC_EXCLUDE_WAYPOINT
    menuQueue = MenuNone;
#else
    static const char *optionsArray[WAYPOINT_HISTORY_LIMIT + 1];
    static uint32_t waypointIds[WAYPOINT_HISTORY_LIMIT + 1];
    static std::string labelStorage[WAYPOINT_HISTORY_LIMIT + 1];

    optionsArray[0] = "Back";
    int options = 1;
    for (const StoredWaypoint &entry : waypointStore.getWaypoints()) {
        if (options > WAYPOINT_HISTORY_LIMIT || !GeofenceModule::hasGeofence(entry.waypoint))
            continue;
        std::string name = sanitizeString(entry.waypoint.name);
        if (name.empty())
            name = "Unnamed Geofence";
        labelStorage[options] = name.substr(0, 20);
        optionsArray[options] = labelStorage[options].c_str();
        waypointIds[options] = entry.waypoint.id;
        options++;
    }

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = options > 1 ? "Geofence Alerts" : "No Geofences";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = options;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == 0) {
            menuQueue = WaypointBaseMenu;
        } else {
            selectedGeofenceWaypointId = waypointIds[selected];
            menuQueue = GeofenceOptionsMenu;
        }
        screen->runNow();
    };
    screen->showOverlayBanner(bannerOptions);
#endif
}

void menuHandler::geofenceOptionsMenu()
{
#if MESHTASTIC_EXCLUDE_WAYPOINT
    menuQueue = MenuNone;
#else
    const StoredWaypoint *entry = waypointStore.findWaypoint(selectedGeofenceWaypointId);
    if (!entry) {
        menuQueue = GeofenceWaypointMenu;
        screen->runNow();
        return;
    }

    static std::string labels[4];
    static const char *optionsArray[4];
    labels[0] = "Back";
    labels[1] = std::string("Enter Alerts: ") + (entry->notificationEnabled(WAYPOINT_NOTIFY_ENTER) ? "On" : "Off");
    labels[2] = std::string("Exit Alerts: ") + (entry->notificationEnabled(WAYPOINT_NOTIFY_EXIT) ? "On" : "Off");
    labels[3] = std::string("Favorites Only: ") + (entry->notificationEnabled(WAYPOINT_NOTIFY_FAVORITES_ONLY) ? "On" : "Off");
    for (size_t i = 0; i < 4; ++i)
        optionsArray[i] = labels[i].c_str();

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Geofence Alerts";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = 4;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == 0) {
            menuQueue = GeofenceWaypointMenu;
        } else {
            const StoredWaypoint *current = waypointStore.findWaypoint(selectedGeofenceWaypointId);
            if (current) {
                const WaypointNotificationPreference preference =
                    selected == 1 ? WAYPOINT_NOTIFY_ENTER
                                  : (selected == 2 ? WAYPOINT_NOTIFY_EXIT : WAYPOINT_NOTIFY_FAVORITES_ONLY);
                waypointStore.setNotificationPreference(selectedGeofenceWaypointId, preference,
                                                        !current->notificationEnabled(preference));
            }
            menuQueue = GeofenceOptionsMenu;
        }
        screen->runNow();
    };
    screen->showOverlayBanner(bannerOptions);
#endif
}

void menuHandler::removeWaypointMenu()
{
#if MESHTASTIC_EXCLUDE_WAYPOINT
    menuQueue = MenuNone;
#else
    static const char *optionsArray[WAYPOINT_HISTORY_LIMIT + 1];
    static uint32_t waypointIds[WAYPOINT_HISTORY_LIMIT + 1];
    static std::string labelStorage[WAYPOINT_HISTORY_LIMIT + 1];

    optionsArray[0] = "Back";
    int options = 1;

    for (const auto &entry : waypointStore.getWaypoints()) {
        if (options > WAYPOINT_HISTORY_LIMIT)
            break;
        std::string name = sanitizeString(entry.waypoint.name);
        if (name.empty())
            name = "Unnamed Waypoint";
        labelStorage[options] = name.substr(0, 20);
        optionsArray[options] = labelStorage[options].c_str();
        waypointIds[options] = entry.waypoint.id;
        options++;
    }

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Remove Waypoint";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = options;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == 0) {
            menuQueue = WaypointBaseMenu;
            screen->runNow();
            return;
        }
        const uint32_t waypointId = waypointIds[selected];
        LOG_INFO("Removing waypoint 0x%08x", waypointId);
        if (waypointModule)
            waypointModule->broadcastDelete(waypointId);
        else
            waypointStore.removeWaypoint(waypointId);
        screen->setFrames(graphics::Screen::FOCUS_DEFAULT);
    };
    screen->showOverlayBanner(bannerOptions);
#endif
}

void menuHandler::traceRouteMenu()
{
    screen->showNodePicker("Node to Trace", 30000, [](uint32_t nodenum) -> void {
        LOG_INFO("Menu: Node picker selected 0x%08x, traceRouteModule=%p", nodenum, traceRouteModule);
        if (traceRouteModule) {
            traceRouteModule->startTraceRoute(nodenum);
        }
    });
}

void menuHandler::testMenu()
{

    enum optionsNumbers { Back, NumberPicker, ShowChirpy, HostPowerOff };
    static const char *optionsArray[5] = {"Back"};
    static int optionsEnumArray[5] = {Back};
    int options = 1;

    optionsArray[options] = "Number Picker";
    optionsEnumArray[options++] = NumberPicker;

    optionsArray[options] = screen->isFrameHidden("chirpy") ? "Show Chirpy" : "Hide Chirpy";
    optionsEnumArray[options++] = ShowChirpy;
#if HAS_HOST_POWEROFF
    // Halts the computer meshtasticd runs on, not just the node. See hostPowerOffMenu().
    optionsArray[options] = "Power Off Host";
    optionsEnumArray[options++] = HostPowerOff;
#endif

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Hidden Test Menu";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = options;
    bannerOptions.optionsEnumPtr = optionsEnumArray;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == NumberPicker) {
            menuQueue = NumberTest;
            screen->runNow();
        } else if (selected == ShowChirpy) {
            screen->toggleFrameVisibility("chirpy");
            screen->setFrames(Screen::FOCUS_SYSTEM);

        } else if (selected == HostPowerOff) {
#if HAS_HOST_POWEROFF
            menuQueue = HostPowerOffMenu;
            screen->runNow();
#endif
        } else {
            menuQueue = SystemBaseMenu;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

// Separate from shutdownMenu(): that one ends the node, this one ends the machine. Worth its own
// confirmation because on a headless node nothing else will bring the host back.
void menuHandler::hostPowerOffMenu()
{
#if HAS_HOST_POWEROFF
    static const char *optionsArray[] = {"Back", "Confirm"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Power Off Host?";
    if (currentResolution == ScreenResolution::UltraLow) {
        bannerOptions.message = "Power Off?";
    }
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = 2;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == 1) {
            // Raise the flag first, then run the ordinary shutdown so the NodeDB and message store
            // are saved exactly as they would be for a normal one; Power.cpp halts the host at the
            // end instead of just exiting.
            hostPowerOffRequested = true;
            InputEvent event = {.inputEvent = (input_broker_event)INPUT_BROKER_SHUTDOWN, .kbchar = 0, .touchX = 0, .touchY = 0};
            inputBroker->injectInputEvent(&event);
        } else {
            menuQueue = TestMenu;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
#endif
}

void menuHandler::numberTest()
{
    screen->showNumberPicker("Verify Nodenum:\n ", 30000, 8, true, [](uint32_t number_picked) -> void {
        LOG_DEBUG("Nodenum: 0x%08x", number_picked);
        keyVerificationModule->sendInitialRequest(number_picked);
    });
}

void menuHandler::wifiBaseMenu()
{
    enum optionsNumbers { Back, Wifi_toggle, Networks, Saved };

    static const char *optionsArray[4];
    static int optionsEnumArray[4];
    int count = 0;
    optionsArray[count] = "Back";
    optionsEnumArray[count++] = Back;
#if BASEUI_WIFI_MANAGER
    if (config.network.wifi_enabled) {
        optionsArray[count] = "Networks";
        optionsEnumArray[count++] = Networks;
    }
    optionsArray[count] = "Saved Networks";
    optionsEnumArray[count++] = Saved;
#endif
    optionsArray[count] = "WiFi Toggle";
    optionsEnumArray[count++] = Wifi_toggle;

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "WiFi Menu";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsEnumPtr = optionsEnumArray;
    bannerOptions.optionsCount = count;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == Wifi_toggle) {
#if BASEUI_WIFI_MANAGER
            wifiToggleReturn = WifiBaseMenu;
#else
            wifiToggleReturn = MenuNone; // the WiFi menu is opened straight from the frame, with no queue entry
#endif
            menuQueue = WifiToggleMenu;
            screen->runNow();
#if BASEUI_WIFI_MANAGER
        } else if (selected == Networks) {
            menuQueue = WifiScanStart;
            screen->runNow();
        } else if (selected == Saved) {
            menuQueue = WifiSavedMenu;
            screen->runNow();
#endif
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

#if BASEUI_WIFI_MANAGER
void menuHandler::pollWifiScan()
{
    if (!wifiScanAwaited)
        return;
    const WiFiNetworks::ScanState state = WiFiNetworks::scanState();
    if (state != WiFiNetworks::ScanState::Done && state != WiFiNetworks::ScanState::Failed)
        return;
    wifiScanAwaited = false;
    menuQueue = WifiScanResultsMenu;
    screen->runNow();
}

// What a scan found, strongest first: signal, whether it's saved (*), open, or the one in use (>).
void menuHandler::wifiScanResultsMenu()
{
    static char labels[20][48];
    static const char *optionsArray[21];
    static WiFiNetworks::ScanResult found[20];
    static int foundCount = 0;

    const bool failed = WiFiNetworks::scanState() == WiFiNetworks::ScanState::Failed;
    foundCount = std::min(WiFiNetworks::scanCount(), 20);
    for (int i = 0; i < foundCount; i++)
        found[i] = WiFiNetworks::scanResult(i);
    WiFiNetworks::clearScan();
    if (failed || foundCount == 0) {
        screen->showSimpleBanner(failed ? "Scan failed" : "No networks found", 3000);
        return;
    }

    const bool connected = WiFi.status() == WL_CONNECTED;
    optionsArray[0] = "Back";
    for (int i = 0; i < foundCount; i++) {
        const int quality = std::max(0, std::min(100, 2 * (found[i].rssi + 100)));
        const bool current = connected && strcmp(found[i].ssid, config.network.wifi_ssid) == 0;
        snprintf(labels[i], sizeof(labels[i]), "%s%s %d%%%s%s", current ? "> " : "", found[i].ssid, quality,
                 WiFiNetworks::knownPsk(found[i].ssid) ? " *" : "", found[i].secured ? "" : " open");
        optionsArray[i + 1] = labels[i];
    }

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Networks";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = foundCount + 1;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected <= 0 || selected > foundCount) {
            menuQueue = WifiBaseMenu;
            screen->runNow();
            return;
        }
        const WiFiNetworks::ScanResult &net = found[selected - 1];
        if (WiFi.status() == WL_CONNECTED && strcmp(net.ssid, config.network.wifi_ssid) == 0) {
            queueNotice("Already connected");
            return;
        }
        if (const char *psk = WiFiNetworks::knownPsk(net.ssid)) {
            joinWifi(net.ssid, psk, true);
        } else if (!net.secured) {
            joinWifi(net.ssid, "", true);
        } else {
            strncpy(wifiPendingSsid, net.ssid, sizeof(wifiPendingSsid) - 1);
            wifiPendingSsid[sizeof(wifiPendingSsid) - 1] = '\0';
            menuQueue = WifiPasswordPrompt; // the prompt can't open from inside this callback
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::wifiSavedMenu()
{
    static char labels[WiFiNetworks::kMaxKnown][48];
    static const char *optionsArray[WiFiNetworks::kMaxKnown + 1];
    const int count = WiFiNetworks::knownCount();
    optionsArray[0] = "Back";
    for (int i = 0; i < count; i++) {
        const char *ssid = WiFiNetworks::known(i).ssid;
        snprintf(labels[i], sizeof(labels[i]), "%s%s", strcmp(ssid, config.network.wifi_ssid) == 0 ? "> " : "", ssid);
        optionsArray[i + 1] = labels[i];
    }

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = count ? "Saved Networks" : "None saved yet";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = count + 1;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected <= 0 || selected > WiFiNetworks::knownCount()) {
            menuQueue = WifiBaseMenu;
            screen->runNow();
            return;
        }
        strncpy(wifiPendingSsid, WiFiNetworks::known(selected - 1).ssid, sizeof(wifiPendingSsid) - 1);
        wifiPendingSsid[sizeof(wifiPendingSsid) - 1] = '\0';
        menuQueue = WifiSavedActionsMenu;
        screen->runNow();
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::wifiSavedActionsMenu()
{
    enum optionsNumbers { Back, Connect, Forget };
    static const char *optionsArray[] = {"Back", "Connect", "Forget"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message = wifiPendingSsid;
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = 3;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == Connect) {
            if (const char *psk = WiFiNetworks::knownPsk(wifiPendingSsid))
                joinWifi(wifiPendingSsid, psk, true);
        } else if (selected == Forget) {
            WiFiNetworks::forget(wifiPendingSsid);
            queueNotice("Forgotten");
        } else {
            menuQueue = WifiSavedMenu;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
}
#endif

void menuHandler::wifiToggleMenu()
{
    enum optionsNumbers { Back, Wifi_disable, Wifi_enable };

    static const char *optionsArray[] = {"Back", "WiFi Disabled", "WiFi Enabled"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "WiFi Actions";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = 3;
    if (config.network.wifi_enabled == true)
        bannerOptions.InitialSelected = 2;
    else
        bannerOptions.InitialSelected = 1;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == Wifi_disable) {
            config.network.wifi_enabled = false;
            config.bluetooth.enabled = true;
            service->reloadConfig(SEGMENT_CONFIG);
            rebootAtMsec = Time::timerEndsAtMillis(DEFAULT_REBOOT_SECONDS * 1000);
        } else if (selected == Wifi_enable) {
            config.network.wifi_enabled = true;
            config.bluetooth.enabled = false;
            service->reloadConfig(SEGMENT_CONFIG);
            rebootAtMsec = Time::timerEndsAtMillis(DEFAULT_REBOOT_SECONDS * 1000);
        } else if (wifiToggleReturn != MenuNone) {
            menuQueue = wifiToggleReturn;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::screenOptionsMenu()
{
    // Whether the backlight can be stepped at all - see BASEUI_HAS_BRIGHTNESS_CONTROL, which keeps the
    // per-panel test (and the T-Deck's opt-in) in one place rather than here.
    const bool hasSupportBrightness = BASEUI_HAS_BRIGHTNESS_CONTROL;

    enum optionsNumbers { Back, Brightness, FrameToggles, DisplayUnits, MessageBubbles, Theme, CalibrateTouch, PanelVcom };
    static const char *optionsArray[8] = {"Back"};
    static int optionsEnumArray[8] = {Back};
    int options = 1;

    // Only show brightness for B&W displays
    if (hasSupportBrightness) {
        optionsArray[options] = "Brightness";
        optionsEnumArray[options++] = Brightness;
    }

    optionsArray[options] = "Frame Visibility";
    optionsEnumArray[options++] = FrameToggles;

    optionsArray[options] = "Display Units";
    optionsEnumArray[options++] = DisplayUnits;

    optionsArray[options] = "Message Bubbles";
    optionsEnumArray[options++] = MessageBubbles;

#if GRAPHICS_TFT_COLORING_ENABLED
    optionsArray[options] = "Theme";
    optionsEnumArray[options++] = Theme;
#endif

#if BASEUI_HAS_TOUCH_CALIBRATION
    // Runtime-gated as well as compile-gated: a variant can be built for a panel with touch and
    // then run on a unit that has none, in which case there is nothing to calibrate.
    if (TFTDisplay::hasTouch()) {
        optionsArray[options] = "Touch Calibration";
        optionsEnumArray[options++] = CalibrateTouch;
    }
#endif
#if TFT_HAS_PANEL_VCOM && BASEUI_PANEL_VCOM_TUNING
    optionsArray[options] = "Panel VCOM";
    optionsEnumArray[options++] = PanelVcom;
#endif

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Display Options";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = options;
    bannerOptions.optionsEnumPtr = optionsEnumArray;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == Brightness) {
            menuHandler::menuQueue = menuHandler::BrightnessPicker;
            screen->runNow();
        } else if (selected == FrameToggles) {
            menuHandler::menuQueue = menuHandler::FrameToggles;
            screen->runNow();
        } else if (selected == DisplayUnits) {
            menuHandler::menuQueue = menuHandler::DisplayUnits;
            screen->runNow();
        } else if (selected == MessageBubbles) {
            menuHandler::menuQueue = menuHandler::MessageBubblesMenu;
            screen->runNow();
        } else if (selected == Theme) {
            menuHandler::menuQueue = menuHandler::ThemeMenu;
            screen->runNow();
#if BASEUI_HAS_TOUCH_CALIBRATION
        } else if (selected == CalibrateTouch) {
            menuHandler::menuQueue = menuHandler::TouchCalibrationMenu;
            screen->runNow();
#endif
        } else if (selected == PanelVcom) {
            menuHandler::menuQueue = menuHandler::PanelVcomMenu;
            screen->runNow();
        } else {
            menuQueue = SystemBaseMenu;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

#if BASEUI_HAS_TOUCH_CALIBRATION
void menuHandler::touchCalibrationMenu()
{
    enum optionsNumbers { Back, Calibrate, Reset };
    static const char *optionsArray[] = {"Back", "Calibrate", "Reset"};
    static int optionsEnumArray[] = {Back, Calibrate, Reset};

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Touch Calibration";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = 3;
    bannerOptions.optionsEnumPtr = optionsEnumArray;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == Calibrate) {
            // Deferred rather than run from here: this callback fires from inside the overlay's
            // draw pass, and the calibration takes over the panel directly for as long as it takes
            // the user to tap four corners.
            menuHandler::menuQueue = menuHandler::RunTouchCalibration;
            screen->runNow();
        } else if (selected == Reset) {
            uiconfig.calibration_data.size = 0;
            memset(uiconfig.calibration_data.bytes, 0, sizeof(uiconfig.calibration_data.bytes));
            saveUIConfig();
            TFTDisplay::clearTouchCalibration();
            screen->showSimpleBanner("Touch Calibration\nReset", 3000);
        } else {
            menuQueue = ScreenOptionsMenu;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::runTouchCalibration()
{
    uint16_t parameters[8] = {0};

    // Blocks until all four corners have been tapped, or until it gives up. Everything else on this
    // thread - the mesh loop included - is stopped for the duration, the same trade device-ui makes.
    const bool done = TFTDisplay::calibrateTouch(parameters);

    if (done) {
        // Same field, same layout device-ui uses, so this calibration is picked up by MUI too.
        static_assert(sizeof(parameters) <= sizeof(uiconfig.calibration_data.bytes),
                      "DeviceUIConfig.calibration_data is too small for 8 calibration parameters");
        memcpy(uiconfig.calibration_data.bytes, parameters, sizeof(parameters));
        uiconfig.calibration_data.size = sizeof(parameters);
        saveUIConfig();
    }

    // The routine drew straight to the panel, bypassing the framebuffer, so force a full repaint.
    screen->forceDisplay(true);
    screen->showSimpleBanner(done ? "Touch Calibration\nSaved" : "Touch Calibration\nCancelled", 3000);
}
#endif

void menuHandler::powerMenu()
{

    enum optionsNumbers { Back, Reboot, Shutdown, MUI };
    static const char *optionsArray[4] = {"Back"};
    static int optionsEnumArray[4] = {Back};
    int options = 1;

    optionsArray[options] = "Reboot";
    optionsEnumArray[options++] = Reboot;

    optionsArray[options] = "Shutdown";
    optionsEnumArray[options++] = Shutdown;

#if HAS_TFT
    optionsArray[options] = "Switch to MUI";
    optionsEnumArray[options++] = MUI;
#endif

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Reboot / Shutdown";
    if (currentResolution == ScreenResolution::UltraLow) {
        bannerOptions.message = "Power";
    }
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = options;
    bannerOptions.optionsEnumPtr = optionsEnumArray;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == Reboot) {
            menuHandler::menuQueue = menuHandler::RebootMenu;
            screen->runNow();
        } else if (selected == Shutdown) {
            menuHandler::menuQueue = menuHandler::ShutdownMenu;
            screen->runNow();
        } else if (selected == MUI) {
            menuHandler::menuQueue = menuHandler::MuiPicker;
            screen->runNow();
        } else {
            menuQueue = SystemBaseMenu;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::keyVerificationInitMenu()
{
    screen->showNodePicker("Node to Verify", 30000,
                           [](uint32_t selected) -> void { keyVerificationModule->sendInitialRequest(selected); });
}

void menuHandler::keyVerificationFinalPrompt()
{
    char message[40] = {0};
    memset(message, 0, sizeof(message));
    sprintf(message, "Verification: \n");
    keyVerificationModule->generateVerificationCode(message + 15); // send the toPhone packet

    if (screen) {
        static const char *optionsArray[] = {"Reject", "Accept"};
        graphics::BannerOverlayOptions options;
        options.message = message;
        options.durationMs = 30000;
        options.optionsArrayPtr = optionsArray;
        options.optionsCount = 2;
        options.notificationType = graphics::notificationTypeEnum::selection_picker;
        options.bannerCallback = [](int selected) {
            if (selected == 1) {
                keyVerificationModule->commitVerifiedRemoteNode();
            }
        };
        screen->showOverlayBanner(options);
    }
}

void menuHandler::frameTogglesMenu()
{
    enum optionsNumbers {
        Finish,
        nodelist_nodes,
        nodelist_location,
        nodelist_lastheard,
        nodelist_hopsignal,
        nodelist_distance,
        nodelist_bearings,
        gps_position,
        lora,
        clock,
        show_favorites,
        show_env_telemetry,
        show_aq_telemetry,
        show_power,
        enumEnd
    };
    static const char *optionsArray[enumEnd] = {"Finish"};
    static int optionsEnumArray[enumEnd] = {Finish};
    int options = 1;

    // Track last selected index (not enum value!)
    static int lastSelectedIndex = 0;
    static int optionCount = 0;

#ifndef USE_EINK
    optionsArray[options] = screen->isFrameHidden("nodelist_nodes") ? "Show Node Lists" : "Hide Node Lists";
    optionsEnumArray[options++] = nodelist_nodes;
#else
    optionsArray[options] = screen->isFrameHidden("nodelist_lastheard") ? "Show NL - Last Heard" : "Hide NL - Last Heard";
    optionsEnumArray[options++] = nodelist_lastheard;
    optionsArray[options] = screen->isFrameHidden("nodelist_hopsignal") ? "Show NL - Hops/Signal" : "Hide NL - Hops/Signal";
    optionsEnumArray[options++] = nodelist_hopsignal;
#endif

#if HAS_GPS
#ifndef USE_EINK
    optionsArray[options] = screen->isFrameHidden("nodelist_location") ? "Show Position Lists" : "Hide Position Lists";
    optionsEnumArray[options++] = nodelist_location;
#else
    optionsArray[options] = screen->isFrameHidden("nodelist_distance") ? "Show NL - Distance" : "Hide NL - Distance";
    optionsEnumArray[options++] = nodelist_distance;
    optionsArray[options] = screen->isFrameHidden("nodelist_bearings") ? "Show NL - Bearings" : "Hide NL - Bearings";
    optionsEnumArray[options++] = nodelist_bearings;
#endif

    optionsArray[options] = screen->isFrameHidden("gps") ? "Show Position" : "Hide Position";
    optionsEnumArray[options++] = gps_position;
#endif

    optionsArray[options] = screen->isFrameHidden("lora") ? "Show LoRa" : "Hide LoRa";
    optionsEnumArray[options++] = lora;

    optionsArray[options] = screen->isFrameHidden("clock") ? "Show Clock" : "Hide Clock";
    optionsEnumArray[options++] = clock;

    optionsArray[options] = screen->isFrameHidden("show_favorites") ? "Show Favorites" : "Hide Favorites";
    optionsEnumArray[options++] = show_favorites;

    optionsArray[options] = moduleConfig.telemetry.environment_screen_enabled ? "Hide Env. Telemetry" : "Show Env. Telemetry";
    optionsEnumArray[options++] = show_env_telemetry;

    optionsArray[options] = moduleConfig.telemetry.air_quality_screen_enabled ? "Hide AQ Telemetry" : "Show AQ Telemetry";
    optionsEnumArray[options++] = show_aq_telemetry;

    optionsArray[options] = moduleConfig.telemetry.power_screen_enabled ? "Hide Power" : "Show Power";
    optionsEnumArray[options++] = show_power;

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Show/Hide Frames";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = options;
    bannerOptions.optionsEnumPtr = optionsEnumArray;
    bannerOptions.InitialSelected = lastSelectedIndex; // Use index, not enum value

    optionCount = options;
    bannerOptions.bannerCallback = [](int selected) -> void {
        // Find the index of selected in optionsEnumArray
        int idx = 0;
        for (; idx < optionCount; ++idx) {
            if (optionsEnumArray[idx] == selected)
                break;
        }
        lastSelectedIndex = idx;

        if (selected == Finish) {
            screen->setFrames(Screen::FOCUS_DEFAULT);
        } else if (selected == nodelist_nodes) {
            screen->toggleFrameVisibility("nodelist_nodes");
            menuHandler::menuQueue = menuHandler::FrameToggles;
            screen->runNow();
        } else if (selected == nodelist_location) {
            screen->toggleFrameVisibility("nodelist_location");
            menuHandler::menuQueue = menuHandler::FrameToggles;
            screen->runNow();
        } else if (selected == nodelist_lastheard) {
            screen->toggleFrameVisibility("nodelist_lastheard");
            menuHandler::menuQueue = menuHandler::FrameToggles;
            screen->runNow();
        } else if (selected == nodelist_hopsignal) {
            screen->toggleFrameVisibility("nodelist_hopsignal");
            menuHandler::menuQueue = menuHandler::FrameToggles;
            screen->runNow();
        } else if (selected == nodelist_distance) {
            screen->toggleFrameVisibility("nodelist_distance");
            menuHandler::menuQueue = menuHandler::FrameToggles;
            screen->runNow();
        } else if (selected == nodelist_bearings) {
            screen->toggleFrameVisibility("nodelist_bearings");
            menuHandler::menuQueue = menuHandler::FrameToggles;
            screen->runNow();
        } else if (selected == gps_position) {
            screen->toggleFrameVisibility("gps");
            menuHandler::menuQueue = menuHandler::FrameToggles;
            screen->runNow();
        } else if (selected == lora) {
            screen->toggleFrameVisibility("lora");
            menuHandler::menuQueue = menuHandler::FrameToggles;
            screen->runNow();
        } else if (selected == clock) {
            screen->toggleFrameVisibility("clock");
            menuHandler::menuQueue = menuHandler::FrameToggles;
            screen->runNow();
        } else if (selected == show_favorites) {
            screen->toggleFrameVisibility("show_favorites");
            menuHandler::menuQueue = menuHandler::FrameToggles;
            screen->runNow();
        } else if (selected == show_env_telemetry) {
            moduleConfig.telemetry.environment_screen_enabled = !moduleConfig.telemetry.environment_screen_enabled;
            menuHandler::menuQueue = menuHandler::FrameToggles;
            screen->runNow();
        } else if (selected == show_aq_telemetry) {
            moduleConfig.telemetry.air_quality_screen_enabled = !moduleConfig.telemetry.air_quality_screen_enabled;
            menuHandler::menuQueue = menuHandler::FrameToggles;
            screen->runNow();
        } else if (selected == show_power) {
            moduleConfig.telemetry.power_screen_enabled = !moduleConfig.telemetry.power_screen_enabled;
            menuHandler::menuQueue = menuHandler::FrameToggles;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::displayUnitsMenu()
{
    enum optionsNumbers { Back, MetricUnits, ImperialUnits };

    static const char *optionsArray[] = {"Back", "Metric", "Imperial"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message = " Select display units";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = 3;
    if (config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL)
        bannerOptions.InitialSelected = 2;
    else
        bannerOptions.InitialSelected = 1;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == MetricUnits) {
            config.display.units = meshtastic_Config_DisplayConfig_DisplayUnits_METRIC;
            service->reloadConfig(SEGMENT_CONFIG);
        } else if (selected == ImperialUnits) {
            config.display.units = meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL;
            service->reloadConfig(SEGMENT_CONFIG);
        } else {
            menuHandler::menuQueue = menuHandler::ScreenOptionsMenu;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::messageBubblesMenu()
{
    enum optionsNumbers { Back, ShowBubbles, HideBubbles };

    static const char *optionsArray[] = {"Back", "Show Bubbles", "Hide Bubbles"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Message Bubbles";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = 3;
    bannerOptions.InitialSelected = config.display.enable_message_bubbles ? 1 : 2;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == ShowBubbles) {
            config.display.enable_message_bubbles = true;
            service->reloadConfig(SEGMENT_CONFIG);
            LOG_INFO("Message bubbles enabled");
        } else if (selected == HideBubbles) {
            config.display.enable_message_bubbles = false;
            service->reloadConfig(SEGMENT_CONFIG);
            LOG_INFO("Message bubbles disabled");
        } else {
            menuHandler::menuQueue = menuHandler::ScreenOptionsMenu;
            screen->runNow();
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

#if HAS_LORA_FEM
void menuHandler::LoRaFEMLNAToggleMenu()
{
    static const LoRaFEMLNAToggleOption femToggleOptions[] = {
        {"Back", OptionsAction::Back},
        {"Enabled", OptionsAction::Select, meshtastic_Config_LoRaConfig_FEM_LNA_Mode_ENABLED},
        {"Disabled", OptionsAction::Select, meshtastic_Config_LoRaConfig_FEM_LNA_Mode_DISABLED},
    };
    constexpr size_t toggleCount = sizeof(femToggleOptions) / sizeof(femToggleOptions[0]);
    static std::array<const char *, toggleCount> toggleLabels{};

    auto bannerOptions = createStaticBannerOptions(
        "FEM LNA", femToggleOptions, toggleLabels, [](const LoRaFEMLNAToggleOption &option, int) -> void {
            if (option.action == OptionsAction::Back) {
                menuQueue = LoraMenu;
                screen->runNow();
                return;
            }

            if (!option.hasValue || config.lora.fem_lna_mode == option.value) {
                return;
            }

            const bool enabled = option.value != meshtastic_Config_LoRaConfig_FEM_LNA_Mode_DISABLED;
            config.lora.fem_lna_mode = option.value;
            loraFEMInterface.setLNAEnable(enabled);
            service->reloadConfig(SEGMENT_CONFIG);
            LOG_INFO("FEM LNA %s", enabled ? "enabled" : "disabled");
        });

    int initialSelection = 0;
    for (size_t i = 0; i < toggleCount; ++i) {
        if (femToggleOptions[i].hasValue && config.lora.fem_lna_mode == femToggleOptions[i].value) {
            initialSelection = static_cast<int>(i);
            break;
        }
    }
    bannerOptions.InitialSelected = initialSelection;

    screen->showOverlayBanner(bannerOptions);
}
#endif

void menuHandler::themeMenu()
{
    // Build menu dynamically from the theme table.
    // Only visible themes appear!
    // Slot budget: 1 for "Back" + up to kMaxThemesInMenu visible themes.
    // Bump kMaxThemesInMenu if you add more themes than will fit here.
    constexpr size_t kMaxThemesInMenu = 15;
    const size_t visibleCount = getVisibleThemeCount();
    static const char *optionsArray[kMaxThemesInMenu + 1] = {"Back"};
    const size_t shownCount = (visibleCount < kMaxThemesInMenu) ? visibleCount : kMaxThemesInMenu;
    const int options = static_cast<int>(shownCount) + 1; // +1 for Back

    for (size_t i = 0; i < shownCount; i++) {
        optionsArray[i + 1] = getVisibleThemeByIndex(i).name;
    }

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Theme";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = options;

    // Highlight the currently active theme (visible index + 1 for the Back
    // offset).  If the active theme is hidden, leave selection on "Back".
    const size_t activeVisible = getActiveVisibleThemeIndex();
    bannerOptions.InitialSelected = (activeVisible == SIZE_MAX) ? 0 : static_cast<int>(activeVisible) + 1;

    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == 0) {
            // Back
            menuHandler::menuQueue = menuHandler::ScreenOptionsMenu;
            screen->runNow();
        } else {
            // Selection is an index into the VISIBLE themes (1-based, slot 0 is Back).
            const size_t visibleIdx = static_cast<size_t>(selected - 1);
            if (visibleIdx < getVisibleThemeCount()) {
                // Persist the theme's uniqueIdentifier so boot-time
                // resolveThemeIndex() can restore this theme on next startup.
                uiconfig.screen_rgb_color = COLOR565(255, 255, (getVisibleThemeByIndex(visibleIdx).uniqueIdentifier & 0x1F) << 3);
                loadThemeDefaults();
                saveUIConfig();
                screen->runNow();
            }
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::panelVcomMenu()
{
#if TFT_HAS_PANEL_VCOM
    // The ST7789's low end up to LovyanGFX's 0x28, finest around the M9's 0x18. Applied live and kept in
    // uiconfig: the right value is a property of the individual panel, not the model, so it outlives the
    // build it was found in rather than having to be baked back into ST7789_VCOMS.
    static constexpr uint8_t kValues[] = {0x00, 0x04, 0x08, 0x0A, 0x0C, 0x0E, 0x10, 0x11, 0x12, 0x13, 0x14,
                                          0x15, 0x16, 0x17, 0x18, 0x1A, 0x1C, 0x20, 0x24, 0x28, 0x30};
    constexpr size_t kCount = sizeof(kValues) / sizeof(kValues[0]);
    static char labels[kCount][16];
    static const char *optionsArray[kCount + 1];
    optionsArray[0] = "Back";
    int initial = 0;
    for (size_t i = 0; i < kCount; i++) {
        snprintf(labels[i], sizeof(labels[i]), "0x%02X %umV", (unsigned)kValues[i], 100 + 25 * (unsigned)kValues[i]);
        optionsArray[i + 1] = labels[i];
        if (kValues[i] == TFTDisplay::panelVcom())
            initial = (int)i + 1;
    }

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Panel VCOM";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = kCount + 1;
    bannerOptions.InitialSelected = initial;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected <= 0) {
            menuHandler::menuQueue = menuHandler::ScreenOptionsMenu;
            screen->runNow();
            return;
        }
        TFTDisplay::setPanelVcom(kValues[selected - 1]);
        uiconfig.has_panel_vcom = true;
        uiconfig.panel_vcom = kValues[selected - 1];
        saveUIConfig();
    };
    screen->showOverlayBanner(bannerOptions);
#endif
}

void menuHandler::handleMenuSwitch(OLEDDisplay *display)
{
    if (menuQueue != MenuNone)
        test_count = 0;
    switch (menuQueue) {
    case MenuNone:
        break;
    case LoraMenu:
        loraMenu();
        break;
    case LoraPicker:
        LoraRegionPicker();
        break;
    case DeviceRolePicker:
        deviceRolePicker();
        break;
    case RadioPresetPicker:
        radioPresetPicker();
        break;
    case TXEnabledMenu:
        txEnabledMenu();
        break;
    case FrequencySlot:
        FrequencySlotPicker();
        break;
    case NoTimeoutLoraPicker:
        LoraRegionPicker(0);
        break;
    case TzPicker:
        TZPicker();
        break;
    case TwelveHourPicker:
        twelveHourPicker();
        break;
    case ClockFacePicker:
        clockFacePicker();
        break;
    case ClockMenu:
        clockMenu();
        break;
    case SystemBaseMenu:
        systemBaseMenu();
        break;
    case PositionBaseMenu:
        positionBaseMenu();
        break;
    case NodeBaseMenu:
        nodeListMenu();
        break;
#if !MESHTASTIC_EXCLUDE_GPS
    case GpsToggleMenu:
        GPSToggleMenu();
        break;
    case GpsFormatMenu:
        GPSFormatMenu();
        break;
    case GpsSmartPositionMenu:
        GPSSmartPositionMenu();
        break;
    case GpsUpdateIntervalMenu:
        GPSUpdateIntervalMenu();
        break;
    case GpsPositionBroadcastMenu:
        GPSPositionBroadcastMenu();
        break;
#endif
    case CompassPointNorthMenu:
        compassNorthMenu();
        break;
    case ResetNodeDbMenu:
        resetNodeDBMenu();
        break;
    case BuzzerModeMenuPicker:
        BuzzerModeMenu();
        break;
    case MuiPicker:
        switchToMUIMenu();
        break;
    case BrightnessPicker:
        BrightnessPickerMenu();
        break;
    case NodeNameLengthMenu:
        nodeNameLengthMenu();
        break;
    case RebootMenu:
        rebootMenu();
        break;
    case ShutdownMenu:
        shutdownMenu();
        break;
    case NodePickerMenu:
        NodePicker();
        break;
    case ManageNodeMenu:
        manageNodeMenu();
        break;
    case RemoveFavorite:
        removeFavoriteMenu();
        break;
    case WaypointBaseMenu:
        waypointBaseMenu();
        break;
    case GeofenceWaypointMenu:
        geofenceWaypointMenu();
        break;
    case GeofenceOptionsMenu:
        geofenceOptionsMenu();
        break;
    case RemoveWaypointMenu:
        removeWaypointMenu();
        break;
    case TraceRouteMenu:
        traceRouteMenu();
        break;
    case HostPowerOffMenu:
        hostPowerOffMenu();
        break;
    case TestMenu:
        testMenu();
        break;
    case NumberTest:
        numberTest();
        break;
    case EnvironmentTelemetryMenu:
        environmentTelemetryMenu();
        break;
    case EnvironmentTelemetrySourceMenu:
        environmentTelemetrySourceMenu();
        break;
    case WifiToggleMenu:
        wifiToggleMenu();
        break;
    case KeyVerificationInit:
        keyVerificationInitMenu();
        break;
    case KeyVerificationFinalPrompt:
        keyVerificationFinalPrompt();
        break;
    case BluetoothToggleMenu:
        bluetoothToggleMenu();
        break;
    case ScreenOptionsMenu:
        screenOptionsMenu();
        break;
    case PowerMenu:
        powerMenu();
        break;
    case FrameToggles:
        frameTogglesMenu();
        break;
    case DisplayUnits:
        displayUnitsMenu();
        break;
    case ThrottleMessage:
        screen->showSimpleBanner("Too Many Attempts\nTry again in 60 seconds.", 5000);
        break;
    case MessageResponseMenu:
        messageResponseMenu();
        break;
    case ReplyMenu:
        replyMenu();
        break;
    case DeleteMessagesMenu:
        deleteMessagesMenu();
        break;
    case MessageViewModeMenu:
        messageViewModeMenu();
        break;
    case MessageOrderMenu:
        messageOrderMenu();
        break;
    case MessageBubblesMenu:
        messageBubblesMenu();
        break;
#if GRAPHICS_TFT_COLORING_ENABLED // the Theme option only exists with TFT coloring
    case ThemeMenu:
        themeMenu();
        break;
#endif
    case PanelVcomMenu:
        panelVcomMenu();
        break;
#if BASEUI_HAS_TOUCH_CALIBRATION
    case TouchCalibrationMenu:
        touchCalibrationMenu();
        break;
    case RunTouchCalibration:
        runTouchCalibration();
        break;
#endif
#if BASEUI_MAP_ROUTING
    case NavSavedRoutesStart:
        if (MapNavigation::listSavedRoutes())
            screen->showSimpleBanner("Reading card...", 10000);
        break;
    case NavSavedRoutesMenu:
        navSavedRoutesMenu();
        break;
    case NavSavedRouteActions:
        navSavedRouteActions();
        break;
#endif
#if BASEUI_WAYPOINT_EDITOR
    case WaypointEditorMenu:
        waypointEditorMenu();
        break;
    case WaypointNamePrompt:
    case WaypointNotePrompt: {
        const bool isName = menuQueue == WaypointNamePrompt;
        char *field = isName ? waypointDraft.name : waypointDraft.description;
        const uint8_t room = isName ? sizeof(waypointDraft.name) - 1 : sizeof(waypointDraft.description) - 1;
        auto backToEditor = []() {
            menuQueue = WaypointEditorMenu;
            screen->runNow();
        };
        screen->showTextPrompt(
            isName ? "Waypoint name" : "Waypoint note", field, room,
            [field, room, backToEditor](const std::string &text) {
                strncpy(field, text.c_str(), room);
                field[room] = '\0';
                backToEditor();
            },
            backToEditor);
        break;
    }
    case WaypointPinPicker:
        if (cannedMessageModule) {
            cannedMessageModule->pickEmote(
                [](const char *label) {
                    if (label)
                        waypointDraft.icon = WaypointUtils::codepointFromUtf8(label);
                    menuQueue = WaypointEditorMenu;
                    screen->runNow();
                },
                waypointDraft.icon ? waypointDraft.icon : kWaypointPushpin); // opens on the current pin
        } else {
            menuQueue = WaypointEditorMenu; // no emote grid on this build: the pin stays the default
            screen->runNow();
        }
        break;
    case WaypointExpiryMenu:
        waypointExpiryMenu();
        break;
#endif
    case HamModeConfirm:
        hamModeConfirmMenu();
        break;
    case LicensedToNormalConfirm:
        licensedToNormalConfirmMenu();
        break;
#if BASEUI_HAS_MAP
    case MapBaseMenu:
        mapBaseMenu();
        break;
#if !MESHTASTIC_EXCLUDE_WAYPOINT
    case MapWaypointsMenu:
        mapWaypointsMenu();
        break;
#endif
#endif
#if BASEUI_HAS_MAP && !BASEUI_MAP_ONSCREEN_CONTROLS
    case MapFollowMeMenu:
        mapFollowMeMenu();
        break;
    case MapZoomLevelMenu:
        mapZoomLevelMenu();
        break;
    case MapPanMenu:
        mapPanMenu();
        break;
#endif
#if BASEUI_MAP_PNG_TILES
    case MapStyleMenu:
        mapStyleMenu();
        break;
#endif
#if BASEUI_MAP_ONLINE_TILES
    case MapSourceMenu:
        mapSourceMenu();
        break;
#endif
#if BASEUI_MAP_NAVIGATION
    case NavigateMenu:
        navigateMenu();
        break;
    case NavNodePickerMenu:
        screen->showNodePicker("Navigate To", 30000, [](uint32_t nodeNum) -> void {
            if (!MapNavigation::navigateToNode(nodeNum))
                queueNotice("That node has no position");
        });
        break;
    case NavWaypointMenu:
        navWaypointMenu();
        break;
    case NavCoordinatesPrompt:
        screen->showTextPrompt("Lat, Lon", "", 40, [](const std::string &text) -> void {
            double lat, lon;
            if (MapCoordinateParse::parse(text.c_str(), lat, lon))
                MapNavigation::navigateToLocation(lat, lon, nullptr);
            else
                screen->showSimpleBanner("Couldn't read that position", 3000);
        });
        break;
#endif
#if BASEUI_MAP_NAVIGATION || BASEUI_WIFI_MANAGER || BASEUI_WAYPOINT_EDITOR
    case NoticeMenu:
        screen->showSimpleBanner(pendingNotice, 3000);
        break;
#endif
#if BASEUI_WIFI_MANAGER
    case WifiBaseMenu:
        wifiBaseMenu();
        break;
    case WifiScanStart:
        if (WiFiNetworks::startScan()) {
            wifiScanAwaited = true;
            screen->showSimpleBanner("Scanning...", 15000);
        } else {
            screen->showSimpleBanner("Can't scan now", 3000);
        }
        break;
    case WifiScanResultsMenu:
        wifiScanResultsMenu();
        break;
    case WifiPasswordPrompt: {
        static char title[48];
        snprintf(title, sizeof(title), "Password: %s", wifiPendingSsid);
        screen->showTextPrompt(title, "", 63, [](const std::string &psk) -> void {
            if (!psk.empty())
                joinWifi(wifiPendingSsid, psk.c_str(), false);
        });
        break;
    }
    case WifiSavedMenu:
        wifiSavedMenu();
        break;
    case WifiSavedActionsMenu:
        wifiSavedActionsMenu();
        break;
#endif
#if BASEUI_MAP_ADDRESS_SEARCH
    case NavAddressPrompt:
        MapNavigation::beginAddressSuggestions();
        screen->showTextPrompt(
            "Address", "", 60,
            [](const std::string &text) -> void {
                MapNavigation::endAddressSuggestions();
                if (text.empty())
                    return;
                if (MapNavigation::startAddressSearch(text.c_str()))
                    screen->showSimpleBanner("Searching...", 20000);
                else
                    screen->showSimpleBanner("Needs WiFi", 3000);
            },
            []() { MapNavigation::endAddressSuggestions(); }, [](const std::string &text) { MapNavigation::addressTyped(text); },
            [](int index) { MapNavigation::pickAddressSuggestion(index); });
        break;
    case NavSearchResultsMenu:
        navSearchResultsMenu();
        break;
#endif
#if HAS_LORA_FEM
    case LoraFemLnaToggleMenu:
        LoRaFEMLNAToggleMenu();
        break;
#endif
    }
    menuQueue = MenuNone;
}

#if BASEUI_HAS_MAP
void menuHandler::mapBaseMenu()
{
    enum class MapAction {
        PanMode,
        FollowMe,
        ZoomLevel,
        Style,
        Source,
        Navigate,
        Waypoints,
    };

    // Pan, zoom and Follow Me are buttons on the frame where there are on-screen controls, so the menu leaves them out.
    static const MapMenuOption baseOptions[] = {
        {"Back", OptionsAction::Back},
#if BASEUI_MAP_NAVIGATION
        {"Navigate", OptionsAction::Select, static_cast<int>(MapAction::Navigate)},
#endif
#if !MESHTASTIC_EXCLUDE_WAYPOINT
        {"Waypoints", OptionsAction::Select, static_cast<int>(MapAction::Waypoints)},
#endif
#if !BASEUI_MAP_ONSCREEN_CONTROLS
        {"Pan", OptionsAction::Select, static_cast<int>(MapAction::PanMode)},
#if !BASEUI_MAP_UPDOWN_ZOOMS // up/down zoom directly on the Map frame instead (see Screen::handleInputEvent)
        {"Zoom", OptionsAction::Select, static_cast<int>(MapAction::ZoomLevel)},
#endif
        {"Follow Me", OptionsAction::Select, static_cast<int>(MapAction::FollowMe)},
#endif
#if BASEUI_MAP_PNG_TILES
        {"Style", OptionsAction::Select, static_cast<int>(MapAction::Style)},
#endif
#if BASEUI_MAP_ONLINE_TILES
        {"Source", OptionsAction::Select, static_cast<int>(MapAction::Source)},
#endif
    };
    constexpr size_t baseCount = sizeof(baseOptions) / sizeof(baseOptions[0]);
    static std::array<const char *, baseCount> baseLabels{};

    auto bannerOptions = createStaticBannerOptions("Map", baseOptions, baseLabels, [](const MapMenuOption &option, int) -> void {
        if (option.action == OptionsAction::Back || !option.hasValue)
            return;

        switch (static_cast<MapAction>(option.value)) {
#if !BASEUI_MAP_ONSCREEN_CONTROLS
        case MapAction::PanMode:
#if HAS_DIRECTIONAL_INPUT
            // Entered directly, held until Back is pressed on the Map frame itself (see
            // Screen::handleInputEvent) - not a picker like Follow Me.
            graphics::MapRenderer::setPanModeEnabled(true);
#else
            // No joystick/keyboard to hold a direction on - offer each
            // direction as its own menu option instead (see mapPanMenu()).
            menuQueue = MapPanMenu;
            screen->runNow();
#endif
            break;
        case MapAction::FollowMe:
            menuQueue = MapFollowMeMenu;
            screen->runNow();
            break;
        case MapAction::ZoomLevel:
#if HAS_DIRECTIONAL_INPUT
            // Entered directly; up/down adjust zoom and a ruler is drawn until Back is
            // pressed (see Screen::handleInputEvent) - not a discrete-level picker.
            graphics::MapRenderer::setZoomModeEnabled(true);
#else
            // No up/down to hold on a two-button device - pick a level directly instead (see
            // mapZoomLevelMenu()).
            menuQueue = MapZoomLevelMenu;
            screen->runNow();
#endif
            break;
#endif
#if BASEUI_MAP_PNG_TILES
        case MapAction::Style:
            menuQueue = MapStyleMenu;
            screen->runNow();
            break;
#endif
#if BASEUI_MAP_ONLINE_TILES
        case MapAction::Source:
            menuQueue = MapSourceMenu;
            screen->runNow();
            break;
#endif
#if BASEUI_MAP_NAVIGATION
        case MapAction::Navigate:
            menuQueue = NavigateMenu;
            screen->runNow();
            break;
#endif
#if !MESHTASTIC_EXCLUDE_WAYPOINT
        case MapAction::Waypoints:
            menuQueue = MapWaypointsMenu;
            screen->runNow();
            break;
#endif
        default:
            break;
        }
    });

    screen->showOverlayBanner(bannerOptions);
}

#if !MESHTASTIC_EXCLUDE_WAYPOINT
// Map > Waypoints: making one here, and the waypoint frame's own actions, reachable from the map.
void menuHandler::mapWaypointsMenu()
{
    enum Choice { Back, NewHere, GeofenceAlerts, RemoveWaypoint, ChoiceCount };
    static const char *labels[ChoiceCount];
    static int choices[ChoiceCount];
    int count = 0;
    labels[count] = "Back";
    choices[count++] = Back;
#if BASEUI_WAYPOINT_EDITOR
#if BASEUI_MAP_NAVIGATION
    double centerLat = 0, centerLng = 0;
    const bool panned = graphics::MapRenderer::pannedCenter(centerLat, centerLng);
#else
    constexpr bool panned = false;
#endif
    labels[count] = panned ? "New Waypoint at Center" : "New Waypoint Here";
    choices[count++] = NewHere;
#endif
    labels[count] = "Geofence Alerts";
    choices[count++] = GeofenceAlerts;
    labels[count] = "Remove Waypoint";
    choices[count++] = RemoveWaypoint;

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Waypoints";
    bannerOptions.optionsArrayPtr = labels;
    bannerOptions.optionsEnumPtr = choices;
    bannerOptions.optionsCount = count;
    bannerOptions.bannerCallback = [](int selected) -> void {
        switch (selected) {
#if BASEUI_WAYPOINT_EDITOR
        case NewHere:
            newWaypointHere(true);
            return;
#endif
        case GeofenceAlerts:
            menuQueue = GeofenceWaypointMenu;
            break;
        case RemoveWaypoint:
            menuQueue = RemoveWaypointMenu;
            break;
        default:
            menuQueue = MapBaseMenu;
            break;
        }
        screen->runNow();
    };
    screen->showOverlayBanner(bannerOptions);
}
#endif
#endif // BASEUI_HAS_MAP

#if BASEUI_MAP_PNG_TILES
void menuHandler::mapStyleMenu()
{
    // Labels point at MapRenderer's style names, which stay put until the next rescan (this menu's own).
    static const char *labels[graphics::MapRenderer::kMaxMapStyles + 2];
    const int count = graphics::MapRenderer::refreshMapStyles();
    labels[0] = "Back";
    for (int i = 0; i < count; i++)
        labels[i + 1] = graphics::MapRenderer::mapStyleLabel(i);

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = count > 0 ? "Map Style" : "No maps found";
    bannerOptions.optionsArrayPtr = labels;
    bannerOptions.optionsCount = count + 1;
    bannerOptions.InitialSelected = graphics::MapRenderer::activeMapStyle() + 1;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected <= 0) {
            menuQueue = MapBaseMenu;
            screen->runNow();
            return;
        }
        graphics::MapRenderer::setMapStyle(selected - 1);
        saveUIConfig();
    };
    screen->showOverlayBanner(bannerOptions);
}
#endif

#if BASEUI_MAP_ONLINE_TILES
// Whether a tile the card doesn't have is fetched over WiFi. Offline is the shipped default: online costs
// radio time and talks to whatever provider the style's .url names.
void menuHandler::mapSourceMenu()
{
    static const MapToggleOption options[] = {
        {"Back", OptionsAction::Back},
        {"Online", OptionsAction::Select, true},
        {"Offline", OptionsAction::Select, false},
    };
    constexpr size_t count = sizeof(options) / sizeof(options[0]);
    static std::array<const char *, count> labels{};

    auto bannerOptions = createStaticBannerOptions("Map Source", options, labels, [](const MapToggleOption &option, int) -> void {
        if (option.action == OptionsAction::Back) {
            menuQueue = MapBaseMenu;
            screen->runNow();
            return;
        }
        if (!option.hasValue)
            return;
        uiconfig.has_map_data = true;
        uiconfig.map_data.online_tiles = option.value;
        saveUIConfig();
    });

    const bool online = uiconfig.has_map_data && uiconfig.map_data.online_tiles;
    for (size_t i = 0; i < count; ++i) {
        if (options[i].hasValue && options[i].value == online) {
            bannerOptions.InitialSelected = i;
            break;
        }
    }

    screen->showOverlayBanner(bannerOptions);
}
#endif

#if BASEUI_HAS_MAP && !BASEUI_MAP_ONSCREEN_CONTROLS
void menuHandler::mapFollowMeMenu()
{
    static const MapToggleOption options[] = {
        {"Back", OptionsAction::Back},
        {"Enabled", OptionsAction::Select, true},
        {"Disabled", OptionsAction::Select, false},
    };
    constexpr size_t count = sizeof(options) / sizeof(options[0]);
    static std::array<const char *, count> labels{};

    auto bannerOptions = createStaticBannerOptions("Follow Me", options, labels, [](const MapToggleOption &option, int) -> void {
        if (option.action == OptionsAction::Back) {
            menuQueue = MapBaseMenu;
            screen->runNow();
            return;
        }
        if (!option.hasValue)
            return;
        graphics::MapRenderer::setFollowMeEnabled(option.value);
    });

    for (size_t i = 0; i < count; ++i) {
        if (options[i].hasValue && options[i].value == graphics::MapRenderer::isFollowMeEnabled()) {
            bannerOptions.InitialSelected = i;
            break;
        }
    }

    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::mapZoomLevelMenu()
{
    // One literal entry per level rather than generating labels at runtime, matching every other
    // menu in this file - kept in sync with MapRenderer's actual [kMinZoom, kMaxZoom] range by the
    // static_assert below, so a future change to those bounds fails loudly here instead of quietly
    // drifting out of sync with the Zoom Level menu it's meant to fully cover.
    static_assert(graphics::MapRenderer::kMinZoom == 0 && graphics::MapRenderer::kMaxZoom == 18,
                  "zoomOptions below must be regenerated to match MapRenderer::kMinZoom/kMaxZoom");
    static const MapMenuOption zoomOptions[] = {
        {"Back", OptionsAction::Back},      {"Z0", OptionsAction::Select, 0},   {"Z1", OptionsAction::Select, 1},
        {"Z2", OptionsAction::Select, 2},   {"Z3", OptionsAction::Select, 3},   {"Z4", OptionsAction::Select, 4},
        {"Z5", OptionsAction::Select, 5},   {"Z6", OptionsAction::Select, 6},   {"Z7", OptionsAction::Select, 7},
        {"Z8", OptionsAction::Select, 8},   {"Z9", OptionsAction::Select, 9},   {"Z10", OptionsAction::Select, 10},
        {"Z11", OptionsAction::Select, 11}, {"Z12", OptionsAction::Select, 12}, {"Z13", OptionsAction::Select, 13},
        {"Z14", OptionsAction::Select, 14}, {"Z15", OptionsAction::Select, 15}, {"Z16", OptionsAction::Select, 16},
        {"Z17", OptionsAction::Select, 17}, {"Z18", OptionsAction::Select, 18},
    };
    constexpr size_t zoomCount = sizeof(zoomOptions) / sizeof(zoomOptions[0]);
    static std::array<const char *, zoomCount> zoomLabels{};

    auto bannerOptions =
        createStaticBannerOptions("Zoom Level", zoomOptions, zoomLabels, [](const MapMenuOption &option, int) -> void {
            if (option.action == OptionsAction::Back) {
                menuQueue = MapBaseMenu;
                screen->runNow();
                return;
            }
            if (!option.hasValue)
                return;
            graphics::MapRenderer::setZoom(option.value);
        });

    const int cur = graphics::MapRenderer::zoom();
    for (size_t i = 0; i < zoomCount; ++i) {
        if (zoomOptions[i].hasValue && zoomOptions[i].value == cur) {
            bannerOptions.InitialSelected = i;
            break;
        }
    }

    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::mapPanMenu()
{
    enum class PanDirection { Up, Down, Left, Right };

    static const MapMenuOption panOptions[] = {
        {"Back", OptionsAction::Back},
        {"Pan Up", OptionsAction::Select, static_cast<int>(PanDirection::Up)},
        {"Pan Down", OptionsAction::Select, static_cast<int>(PanDirection::Down)},
        {"Pan Left", OptionsAction::Select, static_cast<int>(PanDirection::Left)},
        {"Pan Right", OptionsAction::Select, static_cast<int>(PanDirection::Right)},
    };
    constexpr size_t panCount = sizeof(panOptions) / sizeof(panOptions[0]);
    static std::array<const char *, panCount> panLabels{};

    // Remembers the last direction picked (as an index into panOptions) so reopening the menu
    // below highlights it again - repeated panning in the same direction is then just "press
    // SELECT again", not "reselect the direction from the top every time".
    static size_t lastSelected = 0;

    auto bannerOptions =
        createStaticBannerOptions("Pan", panOptions, panLabels, [](const MapMenuOption &option, int selected) -> void {
            if (option.action == OptionsAction::Back) {
                menuQueue = MapBaseMenu;
                screen->runNow();
                return;
            }
            if (!option.hasValue)
                return;

            switch (static_cast<PanDirection>(option.value)) {
            case PanDirection::Up:
                graphics::MapRenderer::panUp();
                break;
            case PanDirection::Down:
                graphics::MapRenderer::panDown();
                break;
            case PanDirection::Left:
                graphics::MapRenderer::panLeft();
                break;
            case PanDirection::Right:
                graphics::MapRenderer::panRight();
                break;
            }

            lastSelected = (size_t)selected;
            // No joystick to hold a direction on - reopen so another tap keeps panning without
            // re-navigating from the Map's base menu for every single step.
            menuQueue = MapPanMenu;
            screen->runNow();
        });

    bannerOptions.InitialSelected = (int8_t)lastSelected;

    screen->showOverlayBanner(bannerOptions);
}
#endif // BASEUI_HAS_MAP && !BASEUI_MAP_ONSCREEN_CONTROLS

// Flips the mute bit on a node and persists. Returns without writing if the node is unknown, so a
// stale pickedNodeNum can't cause a pointless flash write.
void menuHandler::toggleNodeMuted(uint32_t nodeNum)
{
    meshtastic_NodeInfoLite *n = nodeDB->getMeshNode(nodeNum);
    if (!n)
        return;

    const bool wasMuted = nodeInfoLiteIsMuted(n);
    nodeInfoLiteSetBit(n, NODEINFO_BITFIELD_IS_MUTED_MASK, !wasMuted);
    LOG_INFO(wasMuted ? "Unmuted node 0x%08x" : "Muted node 0x%08x", nodeNum);
    nodeDB->notifyObservers(true);
    nodeDB->saveToDisk();
}

#if BASEUI_MAP_ROUTING
// The Navigate menu's rows, so an open one can be told apart from any other menu, and where its Stop tile download
// row sits (-1 when it has none). A rebuild under the user passes the cursor through navigateReselect.
static const char **navigateLabels = nullptr;
static int navigateStopRow = -1, navigateReselect = -1, navigateInitial = -1;

void menuHandler::refreshNavigateMenu()
{
    namespace Fetch = NicheGraphics::MapTiles::Fetch;
    if (navigateStopRow < 0 || !NotificationRenderer::isOverlayBannerShowing() ||
        NotificationRenderer::optionsArrayPtr != navigateLabels)
        return;
    const Fetch::DownloadState state = Fetch::downloadProgress().state;
    if (state == Fetch::DownloadState::Queued || state == Fetch::DownloadState::Running)
        return;
    int cursor = NotificationRenderer::curSelected;
    if (cursor > navigateStopRow)
        cursor--; // the rows below move up into its place
    navigateReselect = cursor;
    navigateStopRow = -1;
    menuQueue = NavigateMenu;
    screen->runNow();
}
#endif

#if BASEUI_MAP_NAVIGATION
// Map > Navigate: resume or stop the current target, or pick a new one.
void menuHandler::navigateMenu()
{
    enum Choice { Back, Saved, Download, Stop, Node, Waypoint, Coordinates, Address, MapCenter, Travel, View, ChoiceCount };
    static const char *labels[ChoiceCount];
    static int choices[ChoiceCount];
    static const char *const kTravelLabels[] = {"Travel: Car", "Travel: Bike", "Travel: Walk"};
    static const char *const kViewLabels[] = {"View: North up", "View: Heading up", "View: 3D"};
    static int reopenOn = -1; // the row just cycled, so the menu comes back with it still selected
    int count = 0;
    labels[count] = "Back";
    choices[count++] = Back;
#if BASEUI_MAP_ROUTING
    labels[count] = "Saved Destinations";
    choices[count++] = Saved;
    // Progress is on the map itself; the menu only offers to stop a download that is running.
    {
        namespace Fetch = NicheGraphics::MapTiles::Fetch;
        const Fetch::DownloadState state = Fetch::downloadProgress().state;
        navigateStopRow = -1;
        if (state == Fetch::DownloadState::Queued || state == Fetch::DownloadState::Running) {
            navigateStopRow = count;
            labels[count] = "Stop tile download";
            choices[count++] = Download;
        }
        navigateLabels = labels;
        if (navigateReselect >= 0) { // rebuilt under the user: keep the cursor where it was
            reopenOn = -1;
            navigateInitial = navigateReselect;
            navigateReselect = -1;
        }
    }
#endif
    if (MapNavigation::isActive()) {
        labels[count] = "Stop Navigation";
        choices[count++] = Stop;
    }
    labels[count] = "To Node";
    choices[count++] = Node;
#if !MESHTASTIC_EXCLUDE_WAYPOINT
    labels[count] = "To Waypoint";
    choices[count++] = Waypoint;
#endif
    labels[count] = "To Coordinates";
    choices[count++] = Coordinates;
#if BASEUI_MAP_ADDRESS_SEARCH
    labels[count] = "To Address";
    choices[count++] = Address;
#endif
    double centerLat, centerLng; // only while panned: following, the centre is where we already are
    const bool panned = graphics::MapRenderer::pannedCenter(centerLat, centerLng);
    if (panned) {
        labels[count] = "To Map Center";
        choices[count++] = MapCenter;
    }
#if BASEUI_MAP_ROUTING
    labels[count] = kTravelLabels[std::min<int>(MapNavigation::travelMode(), 2)];
    choices[count++] = Travel;
#endif
#if BASEUI_MAP_PNG_TILES
    labels[count] = kViewLabels[std::min<int>(MapNavigation::viewMode(), 2)];
    choices[count++] = View;
#endif

    BannerOverlayOptions bannerOptions;
    for (int i = 0; i < count; i++) {
        if (choices[i] == reopenOn)
            bannerOptions.InitialSelected = i;
    }
    reopenOn = -1;
#if BASEUI_MAP_ROUTING
    if (navigateInitial >= 0) {
        bannerOptions.InitialSelected = std::min(navigateInitial, count - 1);
        navigateInitial = -1;
    }
#endif
    bannerOptions.message = MapNavigation::isActive() ? MapNavigation::targetName() : "Navigate";
    bannerOptions.optionsArrayPtr = labels;
    bannerOptions.optionsEnumPtr = choices;
    bannerOptions.optionsCount = count;
    bannerOptions.bannerCallback = [](int selected) -> void {
        switch (selected) {
        case Back:
            menuQueue = MapBaseMenu;
            screen->runNow();
            break;
#if BASEUI_MAP_ROUTING
        case Saved:
            menuQueue = NavSavedRoutesStart; // the card is read on the fetch task; the list opens once it is in
            screen->runNow();
            break;
        case Download: {
            namespace Fetch = NicheGraphics::MapTiles::Fetch;
            const Fetch::DownloadState state = Fetch::downloadProgress().state;
            if (state == Fetch::DownloadState::Queued || state == Fetch::DownloadState::Running) {
                Fetch::cancelDownload();
                queueNotice("Tile download stopped");
            }
            break;
        }
#endif
        case Stop:
            MapNavigation::stop();
            break;
        case Node:
            menuQueue = NavNodePickerMenu;
            screen->runNow();
            break;
        case Waypoint:
            menuQueue = NavWaypointMenu;
            screen->runNow();
            break;
        case Coordinates:
            menuQueue = NavCoordinatesPrompt;
            screen->runNow();
            break;
#if BASEUI_MAP_ADDRESS_SEARCH
        case Address:
            menuQueue = NavAddressPrompt;
            screen->runNow();
            break;
#endif
        case Travel: // each press steps to the next, and the menu comes back to show it
            MapNavigation::setTravelMode((meshtastic_NavTravelMode)((MapNavigation::travelMode() + 1) % 3));
            reopenOn = Travel;
            menuQueue = NavigateMenu;
            screen->runNow();
            break;
        case MapCenter: {
            double lat, lng;
            if (graphics::MapRenderer::pannedCenter(lat, lng))
                MapNavigation::navigateToLocation(lat, lng, nullptr); // named by its coordinates
            break;
        }
        case View:
            MapNavigation::setViewMode((meshtastic_MapViewMode)((MapNavigation::viewMode() + 1) % 3));
            reopenOn = View;
            menuQueue = NavigateMenu;
            screen->runNow();
            break;
        }
    };
    screen->showOverlayBanner(bannerOptions);
}

#if BASEUI_MAP_ROUTING
// Saved Routes: newest first, each with its length. Copied out of the fetcher, which may list again meanwhile.
static NicheGraphics::MapTiles::Fetch::SavedRoute savedRoutes[NicheGraphics::MapTiles::Fetch::kRouteSlots];
static int savedRouteCount = 0;
static int savedRoutePicked = -1; // index into savedRoutes

void menuHandler::navSavedRoutesMenu()
{
    namespace Fetch = NicheGraphics::MapTiles::Fetch;
    static char labels[Fetch::kRouteSlots][48];
    static const char *optionsArray[Fetch::kRouteSlots + 1];

    const Fetch::SavedRoute *found = nullptr;
    savedRouteCount = std::min(Fetch::savedRoutes(found), (int)Fetch::kRouteSlots);
    for (int i = 0; i < savedRouteCount; i++)
        savedRoutes[i] = found[i];
    Fetch::clearList();

    // Each is a place to go back to: labelled by how far it is from here, or by its route's length without a fix.
    const bool haveFix = localPosition.latitude_i != 0 || localPosition.longitude_i != 0;
    optionsArray[0] = "Back";
    for (int i = 0; i < savedRouteCount; i++) {
        char distance[16];
        const auto &h = savedRoutes[i].header;
        const float km = haveFix ? GeoCoord::latLongToMeter(localPosition.latitude_i * 1e-7, localPosition.longitude_i * 1e-7,
                                                            h.targetLatE7 * 1e-7, h.targetLonE7 * 1e-7) /
                                       1000.0f
                                 : h.lengthKm;
        if (config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL)
            snprintf(distance, sizeof(distance), "%.1f mi", km * 0.621371f);
        else
            snprintf(distance, sizeof(distance), "%.1f km", km);
        snprintf(labels[i], sizeof(labels[i]), "%.28s  %s", savedRoutes[i].header.name, distance);
        optionsArray[i + 1] = labels[i];
    }

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = savedRouteCount ? "Saved Destinations" : "No saved destinations";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = savedRouteCount + 1;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected <= 0 || selected > savedRouteCount) {
            menuQueue = NavigateMenu;
            screen->runNow();
            return;
        }
        savedRoutePicked = selected - 1;
        menuQueue = NavSavedRouteActions;
        screen->runNow();
    };
    screen->showOverlayBanner(bannerOptions);
}

void menuHandler::navSavedRouteActions()
{
    namespace Fetch = NicheGraphics::MapTiles::Fetch;
    if (savedRoutePicked < 0 || savedRoutePicked >= savedRouteCount)
        return;
    enum optionsNumbers { Back, Navigate, DownloadTiles, Delete };
    static const char *optionsArray[] = {"Back", "Navigate", "Download tiles on route", "Delete"};
    BannerOverlayOptions bannerOptions;
    bannerOptions.message = savedRoutes[savedRoutePicked].header.name;
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = 4;
    bannerOptions.bannerCallback = [](int selected) -> void {
        const Fetch::SavedRoute &route = savedRoutes[savedRoutePicked];
        switch (selected) {
        case Navigate:
            MapNavigation::navigateSaved(route.slot, route.header);
            break;
        case DownloadTiles:
            // The fetch task checks the style and its tile server; a refusal shows next time the menu opens.
            queueNotice(Fetch::startRouteDownload(route.slot) ? "Downloading tiles along the route"
                                                              : "A download is already running");
            break;
        case Delete:
            Fetch::deleteRoute(route.slot);
            queueNotice("Destination deleted");
            break;
        default:
            menuQueue = NavSavedRoutesStart;
            screen->runNow();
            break;
        }
    };
    screen->showOverlayBanner(bannerOptions);
}
#endif

void menuHandler::navWaypointMenu()
{
#if MESHTASTIC_EXCLUDE_WAYPOINT
    menuQueue = MenuNone;
#else
    static const char *optionsArray[WAYPOINT_HISTORY_LIMIT + 1];
    static uint32_t waypointIds[WAYPOINT_HISTORY_LIMIT + 1];
    static std::string labelStorage[WAYPOINT_HISTORY_LIMIT + 1];

    optionsArray[0] = "Back";
    int options = 1;
    for (const StoredWaypoint &entry : waypointStore.getWaypoints()) {
        const meshtastic_Waypoint &wp = entry.waypoint;
        if (options > WAYPOINT_HISTORY_LIMIT || WaypointStore::isExpired(entry) || !wp.has_latitude_i || !wp.has_longitude_i)
            continue;
        std::string name = sanitizeString(wp.name);
        if (name.empty())
            name = "Unnamed Waypoint";
        labelStorage[options] = name.substr(0, 20);
        optionsArray[options] = labelStorage[options].c_str();
        waypointIds[options] = wp.id;
        options++;
    }

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = options > 1 ? "Navigate To" : "No Waypoints";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = options;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected == 0) {
            menuQueue = NavigateMenu;
            screen->runNow();
            return;
        }
        if (!MapNavigation::navigateToWaypoint(waypointIds[selected]))
            queueNotice("That waypoint is gone");
    };
    screen->showOverlayBanner(bannerOptions);
#endif
}
#endif // BASEUI_MAP_NAVIGATION

#if BASEUI_MAP_ADDRESS_SEARCH
// Opened by MapNavigation once the search it sent has an answer.
void menuHandler::navSearchResultsMenu()
{
    namespace Fetch = NicheGraphics::MapTiles::Fetch;
    namespace Geocode = NicheGraphics::MapTiles::Geocode;
    // Copied out: the fetcher's results only last until its state is cleared, below.
    static Geocode::Result places[Geocode::kMaxResults];
    static std::string labelStorage[Geocode::kMaxResults];
    static const char *optionsArray[Geocode::kMaxResults + 1];

    const bool failed = Fetch::searchState() == Fetch::SearchState::Failed;
    const Geocode::Result *found = nullptr;
    const int count = Fetch::searchResults(found);
    for (int i = 0; i < count; i++)
        places[i] = found[i];
    Fetch::clearSearch();

    if (failed || count == 0) {
        screen->showSimpleBanner(failed ? "Search failed" : "No places found", 3000);
        return;
    }

    // Nearest first, each with how far it is, once there is a fix to measure from; otherwise the provider's order.
    const bool haveFix = localPosition.latitude_i != 0 || localPosition.longitude_i != 0;
    float meters[Geocode::kMaxResults] = {};
    if (haveFix) {
        const double selfLat = localPosition.latitude_i * 1e-7, selfLon = localPosition.longitude_i * 1e-7;
        for (int i = 0; i < count; i++)
            meters[i] = GeoCoord::latLongToMeter(selfLat, selfLon, places[i].lat, places[i].lon);
        for (int i = 1; i < count; i++) // a handful of places: insertion sort, keeping places and distances paired
            for (int j = i; j > 0 && meters[j] < meters[j - 1]; j--) {
                std::swap(meters[j], meters[j - 1]);
                std::swap(places[j], places[j - 1]);
            }
    }

    optionsArray[0] = "Back";
    for (int i = 0; i < count; i++) {
        char distance[16] = "";
        if (haveFix) {
            const bool imperial = config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL;
            const float units = imperial ? meters[i] / 1609.344f : meters[i] / 1000.0f;
            const char *unit = imperial ? "mi" : "km";
            if (!imperial && meters[i] < 1000.0f)
                snprintf(distance, sizeof(distance), "  %d m", (int)lroundf(meters[i]));
            else if (units < 100.0f)
                snprintf(distance, sizeof(distance), "  %.1f %s", units, unit);
            else
                snprintf(distance, sizeof(distance), "  %d %s", (int)lroundf(units), unit);
        }
        labelStorage[i] = sanitizeString(places[i].name).substr(0, haveFix ? 22 : 28) + distance;
        optionsArray[i + 1] = labelStorage[i].c_str();
    }

    BannerOverlayOptions bannerOptions;
    bannerOptions.message = "Navigate To";
    bannerOptions.optionsArrayPtr = optionsArray;
    bannerOptions.optionsCount = count + 1;
    bannerOptions.bannerCallback = [](int selected) -> void {
        if (selected <= 0) {
            menuQueue = NavigateMenu;
            screen->runNow();
            return;
        }
        // A full address is too long for a label: keep up to the second comma ("12, Main Street").
        const Geocode::Result &place = places[selected - 1];
        char name[sizeof(meshtastic_NavTarget::name)];
        strncpy(name, sanitizeString(place.name).c_str(), sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        if (char *first = strchr(name, ','))
            if (char *second = strchr(first + 1, ','))
                *second = '\0';
        MapNavigation::navigateToLocation(place.lat, place.lon, name);
    };
    screen->showOverlayBanner(bannerOptions);
}
#endif

void menuHandler::saveUIConfig()
{
    nodeDB->saveProto("/prefs/uiconfig.proto", meshtastic_DeviceUIConfig_size, &meshtastic_DeviceUIConfig_msg, &uiconfig);
}

} // namespace graphics

#endif
