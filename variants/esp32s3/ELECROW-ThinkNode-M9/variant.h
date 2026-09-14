#define CANNED_MESSAGE_MODULE_ENABLE 1
#define PRESET_MESSAGE_MODULE_ENABLE 1

/*Power*/
#define VEXT_ENABLE 18
#define VEXT_ON_VALUE LOW
#define PIN_GPS_EN 11
#define GPS_EN_ACTIVE LOW

#define USE_POWERSAVE
#define SLEEP_TIME 120

/*Wire Interface*/
#define WIRE_INTERFACES_COUNT 2
// I2C keyboard
#define I2C_SCL 21
#define I2C_SDA 20
#define KB_INT 12             // STC8H key-press interrupt (idle low, rising edge on press)
#define KB_INT_WAKE_ON_HIGH 1 // KB_INT rests low; wake light sleep on its HIGH (active) level
#define KB_LED 46             // STC8H keypad backlight LED
// The STC8H keypad is a real 5-way pad, but not a type configuration.h recognises. Declaring it
// gets modal map Pan/Zoom (held until Back) instead of the one-step-per-pick menus.
#define HAS_DIRECTIONAL_INPUT 1
// No Tab key, so Up in the message composer opens the channel/DM picker instead.
#define CANNED_MESSAGE_UP_OPENS_DESTINATION
// I2C peripheral
#define I2C_SCL1 6
#define I2C_SDA1 7

/*BUZZER*/
#define PIN_BUZZER 9

/*CHARGE_CHECK*/
#define EXT_PWR_DETECT 1
// #define EXT_CHRG_DETECT 1
#define EXT_PWR_DETECT_VALUE LOW

/*GPS*/
#define HAS_GPS 1
#define GPS_BAUDRATE 115200
#define PIN_GPS_RESET 5
#define PIN_GPS_PPS 4
#define GPS_TX_PIN 3
#define GPS_RX_PIN 2
#define GPS_THREAD_INTERVAL 50

/*SPI*/
#define SPI_MOSI 47
#define SPI_SCK 40
#define SPI_MISO 38

/*Screen*/
#define ST7789_CS 16
#define ST7789_RS 15
#define ST7789_TE 19
#define ST7789_SDA SPI_MOSI // MOSI
#define ST7789_SCK SPI_SCK
#define ST7789_RESET 14
#define ST7789_MISO SPI_MISO
#define ST7789_BUSY -1
#define ST7789_BL 17
#define ST7789_SPI_HOST SPI3_HOST
#define SPI_READ_FREQUENCY 16000000

#define USE_TFTDISPLAY 1
#define HAS_SPI_TFT 1
#define TFT_CS ST7789_CS
#define TFT_BL ST7789_BL
#define TFT_HEIGHT 320
#define TFT_WIDTH 240
#define TFT_OFFSET_X 0
#define TFT_OFFSET_Y 0
#define TFT_OFFSET_ROTATION 0
#define TFT_PWM_FREQ 44000
#define TFT_PWM_CHANNEL 7
#define TFT_INVERT_LIGHT true
#define TFT_BACKLIGHT_ON LOW
// GPIO 17 is the LovyanGFX PWM backlight, so nothing else may digitalWrite it - see
// TFT_BACKLIGHT_PWM_ONLY in TFTDisplay.cpp. Screen off is brightness 0.
#define TFT_BACKLIGHT_PWM_ONLY 1
#define SCREEN_ROTATE
// The arrow-key slide is timed and drawn from snapshots, so this only sets how many frames fill it;
// frames the panel can't keep up with are dropped rather than stretching the slide.
#define SCREEN_TRANSITION_FRAMERATE 60
// Compass screens redraw at the magnetometer's 20ms cadence rather than the default 20fps.
#define COMPASS_ACTIVE_FRAMERATE 50
#define SCREEN_NAV_TRANSITION_MS 200 // same length as the pocket-watch firmware's page slide
#define BRIGHTNESS_DEFAULT 128
// LovyanGFX's ST7789 VCOM (0x28, from another panel's datasheet) left a faint image of a static screen behind.
// 0x0A (350 mV), tuned on hardware; higher ghosts more and washes the panel out.
#define ST7789_VCOMS 0x0A
// Display > Panel VCOM, to try values live before changing ST7789_VCOMS.
#define BASEUI_PANEL_VCOM_TUNING 1
// Default Dark's navy canvas drawn as the topographic artwork in graphics/img/background.h.
#define BASEUI_BACKGROUND_IMAGE 1
// Read-only /flash/ page on the web server, for inspecting /prefs and the rest of the device filesystem.
#define WEB_FLASH_BROWSER 1

// Custom boot splash, shown for the second half of the boot screen: full-panel 320x240 artwork, drawn in
// colour on colour-framebuffer builds. 1:1 scale - BASEUI_ICON_SCALE would double it off the panel.
#define USERPREFS_OEM_TEXT "Ixitxachitl Build"
#define USERPREFS_OEM_FONT_SIZE 1
#define USERPREFS_OEM_IMAGE_SCALE 1
#include "graphics/img/oem_splash.h"

/*Lora radio*/
#define HW_SPI1_DEVICE
#define LORA_SCK SPI_SCK
#define LORA_MISO SPI_MISO
#define LORA_MOSI SPI_MOSI
#define LORA_CS 39
#define LORA_RESET 45
#define LORA_DIO0 41
#define LORA_DIO1 42

#define USE_LR1110
#define LR1110_IRQ_PIN LORA_DIO1
#define LR1110_NRESET_PIN LORA_RESET
#define LR1110_BUSY_PIN LORA_DIO0
#define LR1110_SPI_NSS_PIN LORA_CS
#define LR1110_SPI_SCK_PIN LORA_SCK
#define LR1110_SPI_MOSI_PIN LORA_MOSI
#define LR1110_SPI_MISO_PIN LORA_MISO
#define LR11X0_DIO3_TCXO_VOLTAGE 3.3
#define LR11X0_DIO_AS_RF_SWITCH

/*RTC*/
#define PCF8563_RTC 0x51

/*IMU (QMI8658)*/
// Wake on motion from the accel samples in firmware: the chip's own engine wants >=500Hz, well above ours.
#define QMI8658_SOFTWARE_MOTION_WAKE
// The QMC6309's offset shifts by gauss between boots here, so keep refitting it instead of asking for recalibration.
#define QMC6309_HARD_IRON_TRACKING 1
#define SHOW_STEP_COUNTER

/*BATTERY*/
#define BATTERY_PIN 13
#define BATTERY_IMMUTABLE
#define ADC_MULTIPLIER 2.0f
#define BAT_MEASURE_ADC_UNIT ADC_UNIT_2
#define ADC_CHANNEL ADC_CHANNEL_2
#define OCV_ARRAY 4200, 4080, 3980, 3920, 3870, 3820, 3790, 3750, 3700, 3600, 3100
