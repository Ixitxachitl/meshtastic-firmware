#include "./MapJpegTile.h"

#if BASEUI_MAP_JPEG_TILES

#include <JPEGDEC.h>
#include <esp_heap_caps.h>
#include <new>
#include <stdlib.h>
#include <string.h>

namespace NicheGraphics::MapTiles::Jpeg
{
namespace
{

JPEGDEC *decoder = nullptr; // ~20KB of state, so kept in PSRAM and made once
int drawSize = 0;

// Copies one MCU block into the tile, clipped to it in case a decoder edge block overhangs.
int drawBlock(JPEGDRAW *draw)
{
    auto *out = static_cast<uint16_t *>(draw->pUser);
    if (draw->x < 0 || draw->y < 0 || draw->x >= drawSize)
        return 1;
    const int width = draw->iWidthUsed < drawSize - draw->x ? draw->iWidthUsed : drawSize - draw->x;
    for (int row = 0; row < draw->iHeight && draw->y + row < drawSize; row++)
        memcpy(out + (draw->y + row) * drawSize + draw->x, draw->pPixels + row * draw->iWidth, width * sizeof(uint16_t));
    return 1;
}

} // namespace

Result decodeTile(uint8_t *data, size_t size, uint16_t *out, int tileSize, int &width, int &height, int &error)
{
    width = height = error = 0;
    if (!decoder) {
        void *mem = heap_caps_malloc(sizeof(JPEGDEC), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!mem)
            mem = malloc(sizeof(JPEGDEC));
        if (!mem)
            return Result::Failed;
        decoder = new (mem) JPEGDEC();
    }

    if (!decoder->openRAM(data, (int)size, drawBlock)) {
        error = decoder->getLastError();
        return Result::Failed;
    }
    width = decoder->getWidth();
    height = decoder->getHeight();
    if (width != tileSize || height != tileSize) {
        decoder->close();
        return Result::WrongSize;
    }

    drawSize = tileSize;
    decoder->setPixelType(RGB565_LITTLE_ENDIAN);
    decoder->setUserPointer(out);
    const int ok = decoder->decode(0, 0, 0);
    error = decoder->getLastError();
    decoder->close();
    return ok ? Result::Decoded : Result::Failed;
}

} // namespace NicheGraphics::MapTiles::Jpeg

#endif
