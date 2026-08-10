#pragma once

// Portduino has no separate program memory, so PROGMEM data just lives in ordinary RAM.
// This exists only so the vendored SAM tables (src/audio/sam/*Tabs.h, render.c), written
// against the AVR/ESP8266 <pgmspace.h> header, compile unmodified on native builds too.
// -Isrc/platform/portduino is already on the include path for every portduino env (see
// build_flags_common in variants/native/portduino.ini), which is what makes the bare
// #include <pgmspace.h> in those files resolve here.
//
// Forwarding to the framework's own compat header (rather than hand-rolling PROGMEM/
// pgm_read_byte here) matters: this file sits on a global include path, so any other
// library's own `#include "pgmspace.h"` / __has_include(<pgmspace.h>) probe - LovyanGFX's
// lgfx/utility/pgmspace.h does exactly that - resolves to this file too. A partial shim
// (e.g. missing memcmp_P/memcpy_P) silently broke LovyanGFX's PNG decoder at link time the
// first time this file existed; forwarding to the real thing gives every caller the full
// AVR-compat surface instead of just what SAM happens to use.
#include "deprecated-avr-comp/avr/pgmspace.h"
