#pragma once

// Retires the short register-block aliases the SoC header leaks.
//
// hal_sifli's cmsis/sf32lb52x/register.h ends with about a hundred defines of
// the form `#define CRC hwp_crc`, and it reaches every translation unit that
// includes <zephyr/kernel.h> - through arch.h and cmsis_core.h. Names like
// CRC, SPI1, SPI2, RTC and FLASH1 are ordinary identifiers in portable code:
// LovyanGFX declares an SPI1 bus object, LVGL's lodepng has a local variable
// named CRC, and every one of those becomes a syntax error inside a register
// cast.
//
// Force-including this header pulls the SoC definitions in first and then
// removes the aliases, so the rest of the translation unit sees plain
// identifiers again. The hwp_* names the aliases point at are untouched -
// that is what platform code (and the LCDC bus) actually uses.

#include <zephyr/kernel.h>

#undef ATIM1
#undef BTIM1
#undef BTIM2
#undef BTIM3
#undef BTIM4
#undef CRC
#undef DMA1
#undef DMA2
#undef EPIC
#undef EZIP
#undef FLASH1
#undef FLASH2
#undef GPTIM1
#undef GPTIM2
#undef GPTIM3
#undef GPTIM4
#undef GPTIM5
#undef I2C1
#undef I2C2
#undef I2C3
#undef I2C4
#undef LCDC1
#undef LPTIM1
#undef LPTIM2
#undef LPTIM3
#undef SDIO1
#undef SPI1
#undef SPI2
#undef TRNG
#undef USART1
#undef USART2
#undef USART3
#undef USART4
#undef USART5
