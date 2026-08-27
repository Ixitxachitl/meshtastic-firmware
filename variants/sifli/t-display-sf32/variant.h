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

// The module brings DIO2 and DIO3 out internally only, so the usual module
// wiring is assumed: DIO2 drives the RF switch, DIO3 the TCXO. Both need
// confirming against a real board before the radio is trusted.
#define SX126X_DIO2_AS_RF_SWITCH
#define SX126X_DIO3_TCXO_VOLTAGE 1.8f

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

// ── Touch (CST9220) ─────────────────────────────────────────────────────────
#define TOUCH_RST 0
#define TOUCH_INT 1
#define TOUCH_I2C_ADDR 0x5A
#define SCREEN_TOUCH_INT TOUCH_INT

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
