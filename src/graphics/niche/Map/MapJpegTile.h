#pragma once

#include "configuration.h"

#if BASEUI_MAP_JPEG_TILES

#include <stddef.h>
#include <stdint.h>

// JPEG map tiles, for imagery sources that only serve JPEG. Kept apart from MapPngTiles.cpp: JPEGDEC and PNGdec
// define the same byte-order macros differently, so the two headers can't share a translation unit.
namespace NicheGraphics::MapTiles::Jpeg
{

// A JPEG starts with the SOI marker, then another marker.
inline bool isJpeg(const uint8_t *data, size_t size)
{
    return size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
}

enum class Result { Decoded, WrongSize, Failed };

// Decodes a baseline JPEG of exactly tileSize x tileSize into out (native-endian RGB565). On WrongSize, width and
// height hold what the file declared; on Failed, error holds JPEGDEC's code. Display task only.
Result decodeTile(uint8_t *data, size_t size, uint16_t *out, int tileSize, int &width, int &height, int &error);

} // namespace NicheGraphics::MapTiles::Jpeg

#endif
