/*
  pins_arduino.h for T-Echo Plus
  Defines Arduino compatibility macros for SensorLib
*/

#ifndef PINS_ARDUINO_H
#define PINS_ARDUINO_H

#include "variant.h"

#ifdef __cplusplus
extern "C" {
#endif

// Arduino SPI compatibility (required by SensorLib even though we only use I2C)
static const uint8_t SS = PIN_SPI_SS;
static const uint8_t MOSI = PIN_SPI_MOSI;
static const uint8_t MISO = PIN_SPI_MISO;
static const uint8_t SCK = PIN_SPI_SCK;

#ifdef __cplusplus
}
#endif

#endif // PINS_ARDUINO_H
