#pragma once

#define ARCH_SIFLI

//
// Feature flags for SiFli SF32LB52x.
//
// Every optional subsystem in Meshtastic is wrapped in `#if HAS_FOO`, so a
// board only pays for the hardware it ships. Defaults start off here and are
// flipped on by the variant as each subsystem is brought up; prefer adding a
// HAS_* gate over sprinkling `#ifdef ARCH_SIFLI` through shared code.
//

#ifndef HAS_BLUETOOTH
#define HAS_BLUETOOTH 1
#endif
#ifndef HAS_SCREEN
#define HAS_SCREEN 0
#endif
#ifndef HAS_WIRE
#define HAS_WIRE 0
#endif
#ifndef HAS_GPS
#define HAS_GPS 0
#endif
#ifndef HAS_BUTTON
#define HAS_BUTTON 0
#endif
#ifndef HAS_TELEMETRY
#define HAS_TELEMETRY 0
#endif
#ifndef HAS_SENSOR
#define HAS_SENSOR 0
#endif
#ifndef HAS_RADIO
#define HAS_RADIO 1
#endif
#ifndef HAS_CPU_SHUTDOWN
#define HAS_CPU_SHUTDOWN 0
#endif

// GPADC reference on the SF32LB52x; battery divider ratio lives in the variant.
#ifndef AREF_VOLTAGE
#define AREF_VOLTAGE 3.3
#endif
#ifndef BATTERY_SENSE_RESOLUTION_BITS
#define BATTERY_SENSE_RESOLUTION_BITS 12
#endif

//
// HW_VENDOR - maps the build-time board define to a HardwareModel enum.
// PRIVATE_HW (255) covers boards without an assigned SKU; the T-Display SF32
// has no enum value of its own yet.
//
#ifdef T_DISPLAY_SF32
#define HW_VENDOR meshtastic_HardwareModel_PRIVATE_HW
#else
#define HW_VENDOR meshtastic_HardwareModel_UNSET
#endif
