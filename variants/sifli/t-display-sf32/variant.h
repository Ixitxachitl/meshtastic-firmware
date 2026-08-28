#pragma once

/*
 * LilyGo T-Display SF32 (SF32LB525UC6) - Meshtastic variant
 *
 * Arduino pin numbers are SF32 PAxx numbers; the shim splits them across the
 * gpioa_00_31 / gpioa_32_44 Zephyr devices. Peripheral routing (UART, I2C,
 * SPI, LCDC) lives in the board DTS, not here.
 *
 * Board map, from LilyGo's V1.0 schematic:
 *
 *   LoRa (HPB16B3 / SX1262)   MOSI PA24  MISO PA25  SCK PA28  NSS PA29
 *                             DIO1 PA30  BUSY PA31  RST PA32  VCC_EN PA26
 *   TF card (shares SPI1)     CS PA44
 *   NOR flash (MPI2, XIP)     PA12-PA17
 *   AMOLED CO5300, QSPI       TE PA2  CS PA3  CLK PA4  D0-D3 PA5-PA8
 *                             RST PA9  VCI_EN PA43
 *   Touch CST9220 (0x5A)      RST PA0  INT PA1, on the 1V8 side of the
 *                             display FPC level shifter
 *   I2C1 (3V3)                SDA PA37  SCL PA38 - touch, QWIIC, sensor
 *                             header, and everything on the keyboard module
 *   Console UART (CH343P)     TX PA19  RX PA18
 *   Keyboard UART             TX PA40  RX PA39 - L76K GNSS or ESP32-C6,
 *                             muxed by MOD_SEL on the module's XL9555
 *   Keyboard misc             KEY_INT PA11  GPS_PPS PA10  IR_PWM PA36
 *   Power                     3V3_EN PA41  PWR_INT PA42  SPK_CTRL PA20
 *
 * Keyboard module I2C addresses: TCA8418 keypad 0x34, XL9555 expander 0x22,
 * AW21009 backlight 0x25, AW86224 haptics 0x58, BME280 0x76.
 */

#ifndef T_DISPLAY_SF32
#define T_DISPLAY_SF32
#endif

// ── SX1262 (HPB16B3 module) ─────────────────────────────────────────────────
#define USE_SX1262
#define SX126X_CS 29
#define SX126X_DIO1 30
#define SX126X_BUSY 31
#define SX126X_RESET 32

// Module supply switch - held HIGH by initVariant() before RadioLib starts.
#define SX126X_POWER_EN 26

// DIO2 and DIO3 are internal to the module. Both settings come from LilyGo's
// own SX1262_SF32LB52X board port: DIO2 drives the RF switch, and DIO3 feeds
// the TCXO at 3.0 V - not the 1.8 V that is common on other SX1262 boards.
#define SX126X_DIO2_AS_RF_SWITCH
#define SX126X_DIO3_TCXO_VOLTAGE 3.0f

// ── I2C ─────────────────────────────────────────────────────────────────────
#define PIN_WIRE_SDA 37
#define PIN_WIRE_SCL 38
#define WIRE_INTERFACES_COUNT 1

// ── IMU (BHI260AP, main board) ──────────────────────────────────────────────
// Detected at 0x28 by the I2C scan. The chip is a sensor hub: SensorLib feeds
// it a Bosch firmware blob at every boot, so it is useless without SensorLib.
#define BHI260AP_INT 27

// ── TF card, on the LoRa SPI bus ────────────────────────────────────────────
#define SDCARD_CS 44

// ── Display (CO5300 AMOLED, QSPI) ───────────────────────────────────────────
// Kept as plain pin numbers until the LCDC quad path exists; the panel is
// driven through the Zephyr LCDC, not bit-banged.
#define LCD_TE 2
#define LCD_CS 3
#define LCD_CLK 4
#define LCD_D0 5
#define LCD_D1 6
#define LCD_D2 7
#define LCD_D3 8
#define LCD_RST 9
#define LCD_VCI_EN 43
#define TFT_WIDTH 480
#define TFT_HEIGHT 480

// ── BaseUI layout for the 480x480 panel ─────────────────────────────────────
// Same CO5300 family as the T-Watch Ultra, so the knobs start from that
// board's values. Every margin here is a starting point, not a measurement:
// they keep artwork clear of the rounded corners, which is a physical property
// nothing in the build can infer. Expect to retune against a real panel.
#define OLED_HUGE
#define ROUNDED_SCREEN true
#define BASEUI_ICON_SCALE 2
#define BASEUI_HEADER_MARGIN 15
#define BASEUI_HEADER_LR_MARGIN 55
#define BASEUI_BELOW_HEADER_MARGIN 15
#define BASEUI_BODY_LR_MARGIN 35
#define BASEUI_BODY_TOP_MARGIN 8
#define BASEUI_SPLASH_CORNER_INSET_PCT 25

// Nav bar: keep the current frame lit in the middle slot and rotate the rest
// past it, rather than paging.
#define BASEUI_NAV_ICONS_PER_PAGE 5
#define BASEUI_NAV_INFINITE_SCROLL 1
#define BASEUI_NAV_ICON_SIZE_PCT 75

#define BASEUI_NODE_LIST_ROW_ADJUST -1
#define BASEUI_FIXED_COMPASS_SIZE 1

// A hard cap, not a hint: setFastFramerate() feeds this to setTargetFPS(), and
// update() then refuses to redraw more often. The stock 5fps drew a drag five
// times a second, which is what made finger-tracking feel bad on the watch.
#define SCREEN_TRANSITION_FRAMERATE 15 // fps

// Emote picker cells already have BASEUI_ICON_SCALE applied, so the artwork
// needs drawing twice as large again to fill them.
#define EMOTE_PICKER_SCALE_BOOST 2

// On-screen keyboard, for when the keypad module is not attached. Drawn wider
// than the screen so the keys are hittable, panned with a finger. No wrist
// strap here, so it does not need lifting as far off the bottom as the watch.
#define BASEUI_KEYBOARD_ZOOM_PCT 200
#define BASEUI_KEYBOARD_KEY_HEIGHT_PCT 150
#define BASEUI_KEYBOARD_LR_MARGIN_PCT 1
#define BASEUI_KEYBOARD_BOTTOM_MARGIN_PCT 5

// ── Touch (CST9220) ─────────────────────────────────────────────────────────
#define TOUCH_RST 0
#define TOUCH_INT 1
#define TOUCH_I2C_ADDR 0x5A
#define SCREEN_TOUCH_INT TOUCH_INT

// Same panel family as the T-Watch Ultra, which redraws fast enough to track
// a finger, so report drags continuously rather than classifying on release.
#define BASEUI_HAS_TOUCH_DRAG 1

// ── Keyboard module ─────────────────────────────────────────────────────────
#define KEYBOARD_INT 11
#define GPS_PPS 10
#define IR_PWM 36

// ── Power ───────────────────────────────────────────────────────────────────
#define PIN_3V3_EN 41
#define PIN_PWR_INT 42

// ── Buttons ─────────────────────────────────────────────────────────────────
// Silkscreened C, A and B. Pins and polarity are from LilyGo's pin map and
// examples/button: A is active-high with a pull-down, C and B active-low.
#define PIN_BUTTON1 33 // C
#define PIN_BUTTON2 34 // A
#define PIN_BUTTON3 35 // B
#define BUTTON_NEED_PULLUP

// PIN_BUTTON2 becomes ALT_BUTTON_PIN in configuration.h, which defaults to
// active-low with a pull-up. A is neither.
#define ALT_BUTTON_ACTIVE_LOW false
#define ALT_BUTTON_ACTIVE_PULLUP false
