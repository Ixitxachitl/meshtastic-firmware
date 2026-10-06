#pragma once
#if HAS_SCREEN
#include "configuration.h"
namespace graphics
{

class menuHandler
{
  public:
    enum screenMenus {
        MenuNone,
        LoraMenu,
        LoraPicker,
        DeviceRolePicker,
        RadioPresetPicker,
        TXEnabledMenu,
        FrequencySlot,
        NoTimeoutLoraPicker,
        TzPicker,
        TwelveHourPicker,
        ClockFacePicker,
        ClockMenu,
        PositionBaseMenu,
        NodeBaseMenu,
        GpsToggleMenu,
        GpsFormatMenu,
        GpsSmartPositionMenu,
        GpsUpdateIntervalMenu,
        GpsPositionBroadcastMenu,
        CompassPointNorthMenu,
        ResetNodeDbMenu,
        BuzzerModeMenuPicker,
        MuiPicker,
        BrightnessPicker,
        RebootMenu,
        ShutdownMenu,
        NodePickerMenu,
        ManageNodeMenu,
        RemoveFavorite,
        WaypointBaseMenu,
        GeofenceWaypointMenu,
        GeofenceOptionsMenu,
        RemoveWaypointMenu,
        TestMenu,
        HostPowerOffMenu,
        NumberTest,
        EnvironmentTelemetryMenu,
        EnvironmentTelemetrySourceMenu,
        WifiToggleMenu,
        BluetoothToggleMenu,
        ScreenOptionsMenu,
        PowerMenu,
        SystemBaseMenu,
        KeyVerificationInit,
        KeyVerificationFinalPrompt,
        TraceRouteMenu,
        ThrottleMessage,
        MessageResponseMenu,
        MessageViewModeMenu,
        MessageOrderMenu,
        ReplyMenu,
        DeleteMessagesMenu,
        NodeNameLengthMenu,
        FrameToggles,
        DisplayUnits,
        MessageBubblesMenu,
        ThemeMenu,
        PanelVcomMenu,
#if BASEUI_HAS_TOUCH_CALIBRATION
        TouchCalibrationMenu,
        RunTouchCalibration,
#endif
        HamModeConfirm,
        LicensedToNormalConfirm,
#if HAS_LORA_FEM
        LoraFemLnaToggleMenu,
#endif
#if BASEUI_HAS_MAP
        MapBaseMenu,
#if !MESHTASTIC_EXCLUDE_WAYPOINT
        MapWaypointsMenu,
#endif
#endif
#if BASEUI_HAS_MAP && !BASEUI_MAP_ONSCREEN_CONTROLS
        MapFollowMeMenu,
        MapZoomLevelMenu,
        MapPanMenu,
#endif
#if BASEUI_MAP_PNG_TILES
        MapStyleMenu,
#endif
#if BASEUI_MAP_ONLINE_TILES
        MapSourceMenu,
#endif
#if BASEUI_MAP_NAVIGATION
        NavigateMenu,
        NavNodePickerMenu,
        NavWaypointMenu,
        NavCoordinatesPrompt,
#endif
#if BASEUI_MAP_NAVIGATION || BASEUI_WIFI_MANAGER || BASEUI_WAYPOINT_EDITOR
        NoticeMenu,
#endif
#if BASEUI_WIFI_MANAGER
        WifiBaseMenu,
        WifiScanStart,
        WifiScanResultsMenu,
        WifiPasswordPrompt,
        WifiSavedMenu,
        WifiSavedActionsMenu,
#endif
#if BASEUI_MAP_ADDRESS_SEARCH
        NavAddressPrompt,
        NavSearchResultsMenu,
#endif
#if BASEUI_WAYPOINT_EDITOR
        WaypointEditorMenu,
        WaypointNamePrompt,
        WaypointNotePrompt,
        WaypointPinPicker,
        WaypointExpiryMenu,
#endif
#if BASEUI_MAP_ROUTING
        NavSavedRoutesStart,
        NavSavedRoutesMenu,
        NavSavedRouteActions,
#endif
    };
    static screenMenus menuQueue;
    static uint32_t pickedNodeNum; // node selected by NodePicker for ManageNodeMenu
    static meshtastic_Config_LoRaConfig_RegionCode pendingRegion;

    static void OnboardMessage();
    static void LoraRegionPicker(uint32_t duration = 30000);
    static void loraMenu();
    static void deviceRolePicker();
    static void radioPresetPicker();
    static void txEnabledMenu();
    static void FrequencySlotPicker();
    static void handleMenuSwitch(OLEDDisplay *display);
    static void showConfirmationBanner(const char *message, std::function<void()> onConfirm);
    static void clockMenu();
    static void TZPicker();
    static void twelveHourPicker();
    static void clockFacePicker();
    static void messageResponseMenu();
    static void messageViewModeMenu();
    static void replyMenu();
    static void deleteMessagesMenu();
    static void homeBaseMenu();
    static void textMessageBaseMenu();
    static void systemBaseMenu();
    static void favoriteBaseMenu();
    static void positionBaseMenu();
    static void compassNorthMenu();
    static void GPSToggleMenu();
    static void GPSFormatMenu();
    static void GPSSmartPositionMenu();
    static void GPSUpdateIntervalMenu();
    static void GPSPositionBroadcastMenu();
    static void BuzzerModeMenu();
    static void switchToMUIMenu();
    static void nodeListMenu();
    static void resetNodeDBMenu();
    static void BrightnessPickerMenu();
    static void rebootMenu();
    static void shutdownMenu();
    static void NodePicker();
    static void manageNodeMenu();
    static void addFavoriteMenu();
    static void removeFavoriteMenu();
    static void waypointBaseMenu();
    static void geofenceWaypointMenu();
    static void geofenceOptionsMenu();
    static void removeWaypointMenu();
    static void traceRouteMenu();
    static void testMenu();
    static void hostPowerOffMenu();
    static void numberTest();
    static void environmentTelemetryMenu();
    static void environmentTelemetrySourceMenu();
    static void wifiBaseMenu();
    static void wifiToggleMenu();
#if BASEUI_WIFI_MANAGER
    static void wifiScanResultsMenu();
    static void wifiSavedMenu();
    static void wifiSavedActionsMenu();
    // Called as the WiFi frame draws: once a scan it started has finished, queues the results.
    static void pollWifiScan();
#endif
    static void screenOptionsMenu();
    static void powerMenu();
    static void nodeNameLengthMenu();
    static void frameTogglesMenu();
    static void displayUnitsMenu();
    static void messageBubblesMenu();
    static void themeMenu();
    static void panelVcomMenu();
#if BASEUI_HAS_TOUCH_CALIBRATION
    static void touchCalibrationMenu();
    static void runTouchCalibration();
#endif
    static void textMessageMenu();
    static void messageOrderMenu();
    static void hamModeConfirmMenu();
    static void licensedToNormalConfirmMenu();
    // The Map frame's own menu. Where pan, zoom and Follow Me are buttons on the frame, it holds the rest.
#if BASEUI_HAS_MAP
    static void mapBaseMenu();
#if !MESHTASTIC_EXCLUDE_WAYPOINT
    static void mapWaypointsMenu();
#endif
#endif
#if BASEUI_HAS_MAP && !BASEUI_MAP_ONSCREEN_CONTROLS
    static void mapFollowMeMenu();
    static void mapZoomLevelMenu();
    static void mapPanMenu();
#endif
#if BASEUI_MAP_PNG_TILES
    static void mapStyleMenu();
#endif
#if BASEUI_MAP_ONLINE_TILES
    static void mapSourceMenu();
#endif
#if BASEUI_MAP_NAVIGATION
    static void navigateMenu();
    static void navWaypointMenu();
#endif
#if BASEUI_MAP_ADDRESS_SEARCH
    static void navSearchResultsMenu();
#endif
#if BASEUI_WAYPOINT_EDITOR
    // New Waypoint Here: starts a fresh waypoint at our position and opens its editor. From the map with Follow Me
    // off, at the centre of the panned view instead.
    static void newWaypointHere(bool fromMap = false);
    static void waypointEditorMenu();
    static void waypointExpiryMenu();
#endif
#if BASEUI_MAP_ROUTING
    static void navSavedRoutesMenu();
    static void navSavedRouteActions();
    // Called as the map draws: an open Navigate menu still offering to stop a download that has ended is rebuilt
    // without that row.
    static void refreshNavigateMenu();
#endif
#if HAS_LORA_FEM
    static void LoRaFEMLNAToggleMenu();
#endif

    // Lifted out of its banner-callback lambda so it is reachable without a Screen. The lambda only
    // ever runs via screen->showOverlayBanner(), which is why nothing here was unit-testable.
    static void toggleNodeMuted(uint32_t nodeNum); // uint32_t, matching pickedNodeNum above

    // Preset a region selection should leave installed. `lora` is the config as it stands *before*
    // the selection is written.
    static meshtastic_Config_LoRaConfig_ModemPreset presetForRegionSelection(const meshtastic_Config_LoRaConfig &lora,
                                                                             meshtastic_Config_LoRaConfig_RegionCode selected);

  private:
    static void saveUIConfig();
    static void keyVerificationInitMenu();
    static void keyVerificationFinalPrompt();
    static void bluetoothToggleMenu();
};

/* Generic Menu Options designations  */
enum class OptionsAction { Back, Select };

template <typename T> struct MenuOption {
    const char *label;
    OptionsAction action;
    bool hasValue;
    T value;

    MenuOption(const char *labelIn, OptionsAction actionIn, T valueIn)
        : label(labelIn), action(actionIn), hasValue(true), value(valueIn)
    {
    }

    MenuOption(const char *labelIn, OptionsAction actionIn) : label(labelIn), action(actionIn), hasValue(false), value() {}
};

using RadioPresetOption = MenuOption<meshtastic_Config_LoRaConfig_ModemPreset>;
using LoraRegionOption = MenuOption<meshtastic_Config_LoRaConfig_RegionCode>;
using TimezoneOption = MenuOption<const char *>;
using CompassOption = MenuOption<meshtastic_CompassMode>;
using GPSToggleOption = MenuOption<meshtastic_Config_PositionConfig_GpsMode>;
using GPSFormatOption = MenuOption<meshtastic_DeviceUIConfig_GpsCoordinateFormat>;
using NodeNameOption = MenuOption<bool>;
using PositionMenuOption = MenuOption<int>;
using ManageNodeOption = MenuOption<int>;
using ClockFaceOption = MenuOption<bool>;
#if BASEUI_HAS_MAP
using MapMenuOption = MenuOption<int>;
using MapToggleOption = MenuOption<bool>;
#endif
#if HAS_LORA_FEM
using LoRaFEMLNAToggleOption = MenuOption<meshtastic_Config_LoRaConfig_FEM_LNA_Mode>;
#endif

} // namespace graphics
#endif
