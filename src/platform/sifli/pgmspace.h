// pgmspace.h - shim for Zephyr/picolibc
//
// AVR and the ESP8266 keep constant tables in a separate address space and
// reach them through these macros. Cortex-M has one flat address space, so
// they all collapse to plain reads. Vendored code that includes <pgmspace.h>
// directly - src/audio/sam's tables, for one - lands here.
//
// Self-contained rather than including Arduino.h, because the code that wants
// it is C and Arduino.h is not. Definitions match Arduino.h's exactly, and are
// guarded so including both is harmless.
#pragma once

#include <stdint.h>
#include <string.h>

#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef PSTR
#define PSTR(s) (s)
#endif
#ifndef pgm_read_byte
#define pgm_read_byte(addr) (*((const uint8_t *)(addr)))
#endif
#ifndef pgm_read_word
#define pgm_read_word(addr) (*((const uint16_t *)(addr)))
#endif
#ifndef pgm_read_dword
#define pgm_read_dword(addr) (*((const uint32_t *)(addr)))
#endif
#ifndef pgm_read_float
#define pgm_read_float(addr) (*((const float *)(addr)))
#endif
#ifndef pgm_read_ptr
#define pgm_read_ptr(addr) (*((const void **)(addr)))
#endif
#ifndef strlen_P
#define strlen_P(s) strlen(s)
#endif
#ifndef strcpy_P
#define strcpy_P(d, s) strcpy(d, s)
#endif
#ifndef memcpy_P
#define memcpy_P(d, s, n) memcpy(d, s, n)
#endif
