#ifndef HAS_SCREEN
#define HAS_SCREEN 1
#endif
#define USE_TFTDISPLAY 1

// Custom boot splash, shown for the second half of the boot screen. The artwork is authored
// at panel resolution, so pin it to 1:1 - BASEUI_ICON_SCALE would blow 240x200 up to 480x400,
// which overruns the 320x240-class panels these builds usually drive.
#define USERPREFS_OEM_TEXT "Ixitxachitl Build"
#define USERPREFS_OEM_FONT_SIZE 1
#define USERPREFS_OEM_IMAGE_SCALE 1
#include "graphics/img/oem_splash_landscape.h"

#define HAS_GPS 1
#define MAX_RX_TOPHONE portduino_config.maxtophone
#define MAX_NUM_NODES portduino_config.MaxNodes

// RAK12002 RTC Module
#define RV3028_RTC (uint8_t)0b1010010

// Enable Traffic Management Module for native/portduino
#ifndef HAS_TRAFFIC_MANAGEMENT
#define HAS_TRAFFIC_MANAGEMENT 1
#endif
#ifndef HAS_VARIABLE_HOPS
#define HAS_VARIABLE_HOPS 1
#endif
