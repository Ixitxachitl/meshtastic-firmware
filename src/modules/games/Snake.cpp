#include "Snake.h"

#if HAS_SCREEN && BASEUI_HAS_GAMES

#include "GameUtils.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "graphics/ScreenFonts.h"
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

// ---------------------------------------------------------------------------
// Mesh receive
// ---------------------------------------------------------------------------

ProcessMessage Snake::handleReceived(const meshtastic_MeshPacket &mp)
{
#if !SNAKE_ANNOUNCE_HIGH_SCORE
    (void)mp;
    return ProcessMessage::CONTINUE;
#else
    auto isIgnored = [](NodeNum num) -> bool {
        if (!nodeDB || num == 0)
            return false;
        const meshtastic_NodeInfoLite *n = nodeDB->getMeshNode(num);
        return n && nodeInfoLiteIsIgnored(n);
    };
    if (isIgnored(mp.from))
        return ProcessMessage::CONTINUE;

    meshtastic_GameLeaderboard lb = meshtastic_GameLeaderboard_init_default;
    pb_istream_t stream = pb_istream_from_buffer(mp.decoded.payload.bytes, mp.decoded.payload.size);
    if (!pb_decode(&stream, meshtastic_GameLeaderboard_fields, &lb))
        return ProcessMessage::CONTINUE;
    if (lb.game != meshtastic_GameType_GAME_SNAKE || lb.entries_count == 0)
        return ProcessMessage::CONTINUE;

    bool changed = false;
    for (pb_size_t i = 0; i < lb.entries_count; i++) {
        auto &e = lb.entries[i];
        if (e.score == 0)
            continue;
        e.short_name[sizeof(e.short_name) - 1] = '\0';
        const NodeNum nodeNum = (e.node_num != 0) ? e.node_num : mp.from;
        bool dummy = false;
        const int rank = scores_.insert(e.score, e.short_name, nodeNum, dummy, e.score_id);
        if (rank >= 0) {
            changed = true;
            LOG_INFO("Snake: remote score %lu from 0x%08x placed at rank %d", static_cast<unsigned long>(e.score), nodeNum,
                     rank + 1);
        }
    }
    if (changed)
        scores_.save();
    return ProcessMessage::CONTINUE;
#endif
}

// ---------------------------------------------------------------------------
// Mesh announce (SNAKE_ANNOUNCE_HIGH_SCORE only)
// ---------------------------------------------------------------------------

#if SNAKE_ANNOUNCE_HIGH_SCORE

int32_t Snake::nextBroadcastIntervalMs() const
{
    const uint32_t now = millis();
    if (lastBroadcastMs == 0)
        return (now >= BROADCAST_INITIAL_MS) ? 0 : static_cast<int32_t>(BROADCAST_INITIAL_MS - now);
    const uint32_t elapsed = now - lastBroadcastMs;
    return (elapsed >= BROADCAST_INTERVAL_MS) ? 0 : static_cast<int32_t>(BROADCAST_INTERVAL_MS - elapsed);
}

int32_t Snake::meshTick(GamesModule &host)
{
    const int32_t ms = nextBroadcastIntervalMs();
    if (ms == 0) {
        broadcastAllScores(host);
        lastBroadcastMs = millis();
        return static_cast<int32_t>(BROADCAST_INTERVAL_MS);
    }
    return ms;
}

void Snake::broadcastAllScores(GamesModule &host)
{
#if GAME_DEMO_MODE
    return;
#endif
    if (!service)
        return;
    meshtastic_GameLeaderboard lb = meshtastic_GameLeaderboard_init_default;
    lb.game = meshtastic_GameType_GAME_SNAKE;
    lb.entries_count = 0;
    for (uint8_t i = 0; i < HighScoreTableBase::HS_COUNT; i++) {
        if (scores_.scoreAt(i) == 0)
            break;
        auto &e = lb.entries[lb.entries_count];
        e.node_num = scores_.entryAt(i).nodeNum;
        strncpy(e.short_name, scores_.entryAt(i).shortName, sizeof(e.short_name) - 1);
        e.short_name[sizeof(e.short_name) - 1] = '\0';
        e.score = scores_.entryAt(i).score;
        e.score_id = scores_.entryAt(i).scoreId;
        lb.entries_count++;
    }
    if (lb.entries_count == 0)
        return;
    meshtastic_MeshPacket *p = host.gameAllocDataPacket();
    p->to = NODENUM_BROADCAST;
    p->channel = 0;
    pb_ostream_t stream = pb_ostream_from_buffer(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes));
    if (pb_encode(&stream, meshtastic_GameLeaderboard_fields, &lb)) {
        p->decoded.payload.size = static_cast<pb_size_t>(stream.bytes_written);
        service->sendToMesh(p);
        LOG_INFO("Snake: broadcast table (%u entries)", lb.entries_count);
    } else {
        LOG_WARN("Snake: pb_encode table failed");
        packetPool.release(p);
    }
}

void Snake::onNewHighScore(GamesModule &host, const char *initials, uint32_t score, bool isNewTop)
{
    if (score == 0 || !service)
        return;
#if GAME_DEMO_MODE
    if (!isNewTop)
        return;
    char msg[64];
    const char *n = (initials && initials[0]) ? initials : owner.short_name;
    snprintf(msg, sizeof(msg), "%s set a new Snake high score: %lu", n, static_cast<unsigned long>(score));
    meshtastic_MeshPacket *p = host.gameAllocDataPacket();
    p->to = NODENUM_BROADCAST;
    p->channel = channels.getPrimaryIndex();
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p->want_ack = false;
    const pb_size_t msgLen = static_cast<pb_size_t>(strnlen(msg, sizeof(msg) - 1));
    memcpy(p->decoded.payload.bytes, msg, msgLen);
    p->decoded.payload.size = msgLen;
    service->sendToMesh(p);
    LOG_INFO("Snake Demo: broadcast text '%s'", msg);
#else
    announceHighScore(host, score, initials);
#endif
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

#endif // SNAKE_ANNOUNCE_HIGH_SCORE

#endif // HAS_SCREEN && BASEUI_HAS_GAMES
