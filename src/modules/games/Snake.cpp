#include "Snake.h"

#if HAS_SCREEN && BASEUI_HAS_GAMES

#include "GameUtils.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "graphics/ScreenFonts.h"
#include "graphics/TFTColorRegions.h"
#include "graphics/TFTPalette.h"
#include "graphics/images.h"
#include "main.h"
#include "mesh/generated/meshtastic/game.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>

static constexpr int16_t SNAKE_CELL_PX = 4;
static constexpr int16_t SNAKE_SCORE_H = 16;

Snake::Snake()
{
    scores_.load();
}

int32_t Snake::tickIntervalMs() const
{
    // Speed scales with snake length: starts at 450 ms, shrinks by 5 ms per body cell, floor 80 ms.
    const int32_t base = 450 - static_cast<int32_t>(game.length()) * 5;
    return base < 80 ? 80 : base;
}

void Snake::handleInput(input_broker_event ev)
{
    switch (ev) {
    case INPUT_BROKER_UP:
        game.setDirection(SnakeGame::DIR_UP);
        break;
    case INPUT_BROKER_DOWN:
        game.setDirection(SnakeGame::DIR_DOWN);
        break;
    case INPUT_BROKER_LEFT:
        game.setDirection(SnakeGame::DIR_LEFT);
        break;
    case INPUT_BROKER_RIGHT:
        game.setDirection(SnakeGame::DIR_RIGHT);
        break;
    default:
        break;
    }
}

void Snake::drawAttract(OLEDDisplay *display, int16_t x, int16_t y)
{
    display->setColor(WHITE);
    const int16_t w = display->getWidth();
    const int16_t dH = display->getHeight();
    const int16_t cx = x + w / 2;
    const int16_t scale = dH / 64;
    auto syOff = [&](int16_t gy) -> int16_t { return static_cast<int16_t>(y + static_cast<int32_t>(gy) * dH / 64); };
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    display->drawString(cx, y, "S N A K E");
    drawXbmScaled(display, x + (w - snake_width * scale) / 2, syOff(15), snake_width, snake_height, snake, scale);
#if GRAPHICS_TFT_COLORING_ENABLED
    {
        const uint16_t abg = graphics::getThemeBodyBg();
        const int16_t logoX = x + (w - snake_width * scale) / 2;
        const int16_t logoY = syOff(15);
        graphics::registerTFTColorRegionDirect(logoX, logoY, snake_width * scale, snake_height * scale,
                                               graphics::TFTPalette::Green, abg);
    }
#endif
    char hi[32];
    if (scores_.scoreAt(0) > 0 && scores_.nameAt(0)[0] != '\0')
        snprintf(hi, sizeof(hi), "High: %s %lu", scores_.nameAt(0), static_cast<unsigned long>(scores_.scoreAt(0)));
    else
        snprintf(hi, sizeof(hi), "High: %lu", static_cast<unsigned long>(scores_.scoreAt(0)));
    display->drawString(cx, syOff(34), hi);
    display->drawString(cx, syOff(48), "SEL=Play  Hold=Scores");
}

void Snake::drawPlaying(OLEDDisplay *display, int16_t x, int16_t y)
{
    char buf[24];
    display->setColor(WHITE);
    display->setFont(FONT_SMALL);

    const int16_t dW = display->getWidth();
    const int16_t dH = display->getHeight();

    // Scale cell size to fill the display: constrained by both width and height.
    const int16_t cByW = static_cast<int16_t>(dW / SnakeGame::GRID_W);
    const int16_t cByH = static_cast<int16_t>((dH - SNAKE_SCORE_H) / SnakeGame::GRID_H);
    const int16_t cellPx = cByW < cByH ? cByW : cByH;

    const int16_t boardW = static_cast<int16_t>(SnakeGame::GRID_W * cellPx);
    const int16_t boardH = static_cast<int16_t>(SnakeGame::GRID_H * cellPx);
    const int16_t ox = x + (dW - boardW) / 2;
    const int16_t oy = y + SNAKE_SCORE_H + (dH - SNAKE_SCORE_H - boardH) / 2;

    display->setTextAlignment(TEXT_ALIGN_LEFT);
    snprintf(buf, sizeof(buf), "Score %lu", static_cast<unsigned long>(game.score()));
    display->drawString(x + 2, y + 2, buf);
    if (scores_.scoreAt(0) > 0) {
        display->setTextAlignment(TEXT_ALIGN_RIGHT);
        snprintf(buf, sizeof(buf), "Hi %lu", static_cast<unsigned long>(scores_.scoreAt(0)));
        display->drawString(x + dW - 2, y + 2, buf);
    }
    display->drawLine(x, oy - 1, x + dW - 1, oy - 1);

    for (uint16_t i = 0; i < game.length(); i++) {
        SnakeGame::Cell c = game.bodyAt(i);
        display->fillRect(ox + c.x * cellPx, oy + c.y * cellPx, cellPx, cellPx);
    }
    SnakeGame::Cell f = game.food();
    const int16_t foodSz = cellPx > 2 ? static_cast<int16_t>(cellPx / 2) : static_cast<int16_t>(1);
    display->fillRect(ox + f.x * cellPx + 1, oy + f.y * cellPx + 1, foodSz, foodSz);
}

uint32_t Snake::gameType() const
{
    return meshtastic_GameType_GAME_SNAKE;
}

#if GAMES_ANNOUNCE_HIGH_SCORE

void Snake::onAnnounceScore(GamesModule &host, const char *initials, uint32_t score)
{
    if (score == 0 || !service)
        return;
    announceHighScore(host, score, initials);
}

void Snake::announceHighScore(GamesModule &host, uint32_t score, const char *name)
{
    if (!service)
        return;
    meshtastic_GameLeaderboard lb = meshtastic_GameLeaderboard_init_default;
    lb.game = meshtastic_GameType_GAME_SNAKE;
    lb.entries_count = 1;
    const char *n = (name && name[0]) ? name : owner.short_name;
    const uint32_t localNum = nodeDB ? nodeDB->getNodeNum() : 0;
    lb.entries[0].node_num = localNum;
    strncpy(lb.entries[0].short_name, n, sizeof(lb.entries[0].short_name) - 1);
    lb.entries[0].short_name[sizeof(lb.entries[0].short_name) - 1] = '\0';
    lb.entries[0].score = score;
    lb.entries[0].score_id = 0;
    for (uint8_t i = 0; i < HighScoreTableBase::HS_COUNT; i++) {
        if (scores_.entryAt(i).score == score && scores_.entryAt(i).nodeNum == localNum) {
            lb.entries[0].score_id = scores_.entryAt(i).scoreId;
            break;
        }
    }
    meshtastic_MeshPacket *p = host.gameAllocDataPacket();
    p->to = NODENUM_BROADCAST;
    p->channel = 0;
    pb_ostream_t stream = pb_ostream_from_buffer(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes));
    if (pb_encode(&stream, meshtastic_GameLeaderboard_fields, &lb)) {
        p->decoded.payload.size = static_cast<pb_size_t>(stream.bytes_written);
        service->sendToMesh(p);
        LOG_INFO("Snake: broadcast score %lu", static_cast<unsigned long>(score));
    } else {
        LOG_WARN("Snake: pb_encode score failed");
        packetPool.release(p);
    }
}

#endif // GAMES_ANNOUNCE_HIGH_SCORE

#endif // HAS_SCREEN && BASEUI_HAS_GAMES
