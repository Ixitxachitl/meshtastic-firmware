
// CO5300 TFT AMOLED
#define CO5300_CS 41
#define CO5300_SCK 40
#define CO5300_RESET 37
#define CO5300_TE 6
#define CO5300_IO0 38
#define CO5300_IO1 39
#define CO5300_IO2 42
#define CO5300_IO3 45
#define CO5300_SPI_HOST SPI2_HOST
#define SPI_FREQUENCY 75000000
#define SPI_READ_FREQUENCY 16000000 // irrelevant
#define TFT_HEIGHT 502
#define TFT_WIDTH 410
#define TFT_OFFSET_X 22
#define TFT_OFFSET_Y 0
#define TFT_OFFSET_ROTATION 0
// Framerate while a frame transition is animating - including one a finger is dragging.
//
// This is a hard cap, not a hint: setFastFramerate() feeds it to OLEDDisplayUi::setTargetFPS(),
// which turns it into updateInterval, and update() then refuses to redraw more often than that no
// matter how many times the thread runs. At the old value of 5 a drag was drawn five times a
// second, which is what made swiping here feel worse than the T-Deck (30).
//
// 15 is sized to what the panel measurably does: a full repaint costs ~26 ms idle and ~46 ms
// mid-transition, when both the outgoing and incoming frame register colour regions to overprint.
// Going higher just saturates the cooperative scheduler and starves the touch poll, since these
// threads cannot preempt each other.
#define SCREEN_TRANSITION_FRAMERATE 15 // fps
#define USE_TFTDISPLAY 1
#define HAS_SCREEN 1
#define TFT_RESET_AFTER_SLEEP
#define OLED_HUGE
#define ROUNDED_SCREEN true
#define BASEUI_HEADER_MARGIN 15
#define BASEUI_HEADER_LR_MARGIN 55
#define BASEUI_BELOW_HEADER_MARGIN 15
#define BASEUI_BODY_LR_MARGIN 35
#define BASEUI_BODY_TOP_MARGIN 8
#define BASEUI_NAV_ICONS_PER_PAGE 5
// Swipe navigation: keep the current frame lit in the middle slot and rotate the rest
// past it, rather than paging. Paging made the highlight look wrong mid-swipe.
#define BASEUI_NAV_INFINITE_SCROLL 1
#define BASEUI_NAV_ICON_SIZE_PCT 75
#define BASEUI_NODE_LIST_ROW_ADJUST -1
// Messages sit against their sender's edge - incoming left, outgoing right - rather than centred,
// with the remaining fifth of the width left as a gutter on the far side so the split reads at a
// glance. BASEUI_CENTER_MESSAGE_BUBBLES stays at its shared default of 0.
#define BASEUI_MESSAGE_BUBBLE_MAX_PCT 100
#define BASEUI_FIXED_COMPASS_SIZE 1
#define BASEUI_SPLASH_CORNER_INSET_PCT 25
#define BASEUI_ICON_SCALE 2
// Emote picker cells are sized with BASEUI_ICON_SCALE already applied, so at scale 2 the
// artwork filled only half its cell. Draw it twice as large to fill the space.
#define EMOTE_PICKER_SCALE_BOOST 2

// Custom boot splash, shown for the second half of the boot screen. The artwork is already
// drawn at panel resolution, so pin it to 1:1 - BASEUI_ICON_SCALE would blow 216x300 up to
// 432x600 and clip it against this 410x502 panel.
#define USERPREFS_OEM_TEXT "Ixitxachitl Build"
#define USERPREFS_OEM_FONT_SIZE 1
#define USERPREFS_OEM_IMAGE_SCALE 1
#include "oem_splash.h"
// Draw the on-screen keyboard twice as wide as the screen so the keys are big enough to hit on
// this panel, and pan it left/right with a finger to reach the rest.
#define BASEUI_KEYBOARD_ZOOM_PCT 200
// ...and half again as tall, which this panel has the vertical room for.
#define BASEUI_KEYBOARD_KEY_HEIGHT_PCT 150
// The virtual keyboard is the only screen that puts touch targets hard against the panel edges. The
// strap crowds the bottom of a watch, so the key block is lifted clear of it; the sides need only a
// hair, since fourteen columns are already narrow and the panel does not round away far enough
// there to swallow a key. Much smaller than BASEUI_BODY_LR_MARGIN, and deliberately so.
#define BASEUI_KEYBOARD_LR_MARGIN_PCT 1
#define BASEUI_KEYBOARD_BOTTOM_MARGIN_PCT 10

#define HAS_TOUCHSCREEN 1
#define HAS_SPI_TFT 1
#define ENABLE_TOUCH_INT 1
#define VARIANT_TOUCHSCREEN 1
#define SCREEN_TOUCH_INT 12
#define TOUCH_I2C_PORT 0
#define TOUCH_SLAVE_ADDRESS 0x1A
#define WAKE_ON_TOUCH

// Touch sampling deliberately left on the shared defaults (20 ms floor / 20 ms active).
//
// Sampling faster was tried - TOUCH_MIN_POLL_INTERVAL 8 with TOUCH_POLL_INTERVAL_ACTIVE 10 - and it
// does reach ~11 ms in practice, but it produces phantom taps: the CST92xx only returns a point
// when its interrupt says a new one is ready, so polling ahead of the chip's own report rate reads
// as an intermittent release, and a release is what synthesises a tap. Measurement also showed
// polling was never the constraint here - the screen only repaints a few times a second during a
// drag, so the finger is tracked far more finely than it is ever drawn.

#define BUTTON_PIN 0

#define USE_POWERSAVE
#define SLEEP_TIME 120

// External expansion chip XL9555
#define USE_XL9555

// PCF85063 RTC Module
#define PCF85063_RTC 0x51
#define HAS_RTC 1

// MAX98357A
#define HAS_I2S
#define DAC_I2S_BCK 9
#define DAC_I2S_WS 10
#define DAC_I2S_DOUT 11
#define DAC_I2S_MCLK -1 // TODO
// Audio volume lives in platformio.ini as -D flags, not here: RtttlPcm.h deliberately
// includes nothing but <stdint.h> so the native tests can build it, which means defines
// made in this header never reach it. See AUDIO_RTTTL_AMPLITUDE / AUDIO_SPEECH_GAIN.
// The MAX98357A's analog gain is pin-strapped, so those are the only volume control.

#define HAS_AXP2101
#define PMU_IRQ 7
#define PMU_POWER_BUTTON_IS_CANCEL
#define HAS_DRV2605 1

#define HAS_BHI260AP
#define BHI260AP_INT 8
// Measured from the gravity vector: the part's own axes already line up with the watch (X at 3
// o'clock, Y at 12, Z out of the screen), so no remapping. Required for the wrist-tilt gesture.
#define BHI260AP_REMAP_AXES TOP_LAYER_LEFT_CORNER
#undef MESHTASTIC_EXCLUDE_ACCELEROMETER
#define SHOW_STEP_COUNTER

#define I2C_SDA 3
#define I2C_SCL 2
#define I2C_NO_RESCAN

#define HAS_GPS 1
#define GPS_BAUDRATE 38400
#define GPS_RX_PIN 44
#define GPS_TX_PIN 43
#define PIN_GPS_PPS 13

#define USE_SX1262
// #define USE_SX1280
#define HW_SPI1_DEVICE

#define LORA_SCK 35
#define LORA_MISO 33
#define LORA_MOSI 34
#define LORA_CS 36

#define LORA_DIO0 -1 // a No connect on the SX1262 module
#define LORA_RESET 47
#define LORA_DIO1 14 // SX1262 IRQ
#define LORA_DIO2 48 // SX1262 BUSY
#define LORA_DIO3

#define SX126X_CS LORA_CS
#define SX126X_DIO1 LORA_DIO1
#define SX126X_BUSY LORA_DIO2
#define SX126X_RESET LORA_RESET
#define SX126X_DIO2_AS_RF_SWITCH
#define SX126X_DIO3_TCXO_VOLTAGE 1.8

#define USE_VIRTUAL_KEYBOARD 1
#define DISPLAY_CLOCK_FRAME 1
