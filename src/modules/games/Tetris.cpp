#include "Tetris.h"

#if HAS_SCREEN && BASEUI_HAS_GAMES

#include "MeshService.h"
#include "NodeDB.h"
#include "buzz/buzz.h"
#include "graphics/ScreenFonts.h"
#include "graphics/images.h"
#include "main.h"
#include "mesh/generated/meshtastic/game.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>

static constexpr int16_t TETRIS_CELL_PX = 4;

Tetris::Tetris()
{
    scores_.load();
}

void Tetris::start(uint32_t seed)
{
    game.reset(seed);
    lockDelayActive = false;
    pendingLineClearChirp = false;
}

int32_t Tetris::tickIntervalMs() const
{
    // Speed ramps with level: 600 ms base, 30 ms per level, floor 80 ms.
    const int32_t iv = 600 - static_cast<int32_t>(game.level()) * 30;
    return iv < 80 ? 80 : iv;
}

bool Tetris::tick()
{
    if (pendingLineClearChirp) {
        pendingLineClearChirp = false;
        playChirp();
    }

    const uint32_t now = millis();

    if (game.isGrounded()) {
        if (!lockDelayActive) {
            lockDelayStartMs = now;
            lockDelayActive = true;
        } else if (now - lockDelayStartMs >= LOCK_DELAY_MS) {
            lockDelayActive = false;
            game.lockNow();
            if (!game.isPlaying())
                return false;
            if (game.lastLinesClearedCount() > 0)
                playChirp();
        }
        // Still in lock-delay window -- no gravity tick, just stay alive.
        return true;
    } else {
        lockDelayActive = false;
        game.tryGravity();
    }

    return game.isPlaying();
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void Tetris::handleInput(input_broker_event ev)
{
    switch (ev) {
    case INPUT_BROKER_UP:
        if (game.rotate() && lockDelayActive)
            lockDelayStartMs = millis();
        break;
    case INPUT_BROKER_LEFT:
        if (game.moveLeft() && lockDelayActive)
            lockDelayStartMs = millis();
        break;
    case INPUT_BROKER_RIGHT:
        if (game.moveRight() && lockDelayActive)
            lockDelayStartMs = millis();
        break;
    case INPUT_BROKER_DOWN:
        if (game.tryGravity()) {
            lockDelayActive = false;
        } else if (lockDelayActive) {
            lockDelayStartMs = millis();
        }
        break;
    case INPUT_BROKER_SELECT:
        // Hard drop: bypass lock delay and lock immediately.
        lockDelayActive = false;
        game.hardDrop();
        if (game.lastLinesClearedCount() > 0)
            pendingLineClearChirp = true;
        break;
    case INPUT_BROKER_SELECT_LONG:
        // Hold / swap piece.
        if (game.holdPiece())
            lockDelayActive = false;
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void Tetris::drawAttract(OLEDDisplay *display, int16_t x, int16_t y)
{
    const int16_t w = display->getWidth();
    const int16_t cx = x + w / 2;
    display->setColor(WHITE);
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    display->drawString(cx, y, "T E T R I S");
    display->drawXbm(x + (w - tetris_width) / 2, y + 15, tetris_width, tetris_height, tetris);
    char hi[32];
    if (scores_.scoreAt(0) > 0 && scores_.nameAt(0)[0] != '\0')
        snprintf(hi, sizeof(hi), "High: %s %lu", scores_.nameAt(0), static_cast<unsigned long>(scores_.scoreAt(0)));
    else
        snprintf(hi, sizeof(hi), "High: %lu", static_cast<unsigned long>(scores_.scoreAt(0)));
    display->drawString(cx, y + 34, hi);
    display->drawString(cx, y + 48, "SEL=Play  Hold=Scores");
}

void Tetris::drawPlayfield(OLEDDisplay *display, int16_t x, int16_t y)
{
    const int16_t boardW = TetrisGame::BOARD_COLS * TETRIS_CELL_PX; // 40 px
    const int16_t ox = x + (display->getWidth() - boardW) / 2;
    const int16_t oy = y;

    display->setColor(WHITE);

    // Board border lines.
    display->drawLine(ox - 1, oy, ox - 1, oy + display->getHeight() - 1);
    display->drawLine(ox + boardW, oy, ox + boardW, oy + display->getHeight() - 1);
    display->drawLine(ox - 1, oy + display->getHeight() - 1, ox + boardW, oy + display->getHeight() - 1);

    auto drawCell = [&](int8_t col, int8_t row) {
        if (col < 0 || row < 0 || col >= TetrisGame::BOARD_COLS || row >= TetrisGame::BOARD_ROWS)
            return;
        display->fillRect(ox + static_cast<int16_t>(col) * TETRIS_CELL_PX, oy + static_cast<int16_t>(row) * TETRIS_CELL_PX,
                          TETRIS_CELL_PX - 1, TETRIS_CELL_PX - 1);
    };

    // Locked cells.
    for (uint8_t r = 0; r < TetrisGame::BOARD_ROWS; r++)
        for (uint8_t c = 0; c < TetrisGame::BOARD_COLS; c++)
            if (game.board[r][c])
                drawCell(static_cast<int8_t>(c), static_cast<int8_t>(r));

    // Ghost piece.
    const TetrisGame::Piece &cur = game.current();
    const int8_t ghostR = game.ghostRow();
    if (ghostR != cur.row) {
        for (uint8_t pr = 0; pr < 4; pr++) {
            for (uint8_t pc = 0; pc < 4; pc++) {
                if (!TetrisGame::pieceCell(cur.type, cur.rot, pr, pc))
                    continue;
                const int8_t gc = static_cast<int8_t>(cur.col + pc);
                const int8_t gr = static_cast<int8_t>(ghostR + pr);
                if (gc < 0 || gr < 0 || gc >= TetrisGame::BOARD_COLS || gr >= TetrisGame::BOARD_ROWS)
                    continue;
                display->drawRect(ox + static_cast<int16_t>(gc) * TETRIS_CELL_PX, oy + static_cast<int16_t>(gr) * TETRIS_CELL_PX,
                                  TETRIS_CELL_PX - 1, TETRIS_CELL_PX - 1);
            }
        }
    }

    // Active piece.
    for (uint8_t pr = 0; pr < 4; pr++) {
        for (uint8_t pc = 0; pc < 4; pc++) {
            if (!TetrisGame::pieceCell(cur.type, cur.rot, pr, pc))
                continue;
            drawCell(static_cast<int8_t>(cur.col + pc), static_cast<int8_t>(cur.row + pr));
        }
    }

    // Left panel: SCR and LVL.
    char buf[12];
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->drawString(x + 2, y + 2, "SCR");
    snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(game.score()));
    display->drawString(x + 2, y + 2 + FONT_HEIGHT_SMALL, buf);
    display->drawString(x + 2, y + 2 + FONT_HEIGHT_SMALL * 2 + 2, "LVL");
    snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(game.level()));
    display->drawString(x + 2, y + 2 + FONT_HEIGHT_SMALL * 3 + 2, buf);

    // Right panel: NXT and HLD previews.
    const int16_t rpx = ox + boardW + 2;
    const int16_t rpanelW = display->getWidth() - rpx;
    const int16_t rcx = rpx + rpanelW / 2;
    static constexpr int16_t PREV_PX = 3;
    const int16_t previewW = 4 * PREV_PX;
    const int16_t previewX = rpx + (rpanelW - previewW) / 2;

    display->setTextAlignment(TEXT_ALIGN_CENTER);

    display->drawString(rcx, y + 2, "NXT");
    const int16_t nxtY = y + 2 + FONT_HEIGHT_SMALL;
    const TetrisGame::Piece &nxt = game.next();
    for (uint8_t pr = 0; pr < 4; pr++)
        for (uint8_t pc = 0; pc < 4; pc++)
            if (TetrisGame::pieceCell(nxt.type, nxt.rot, pr, pc))
                display->fillRect(previewX + static_cast<int16_t>(pc) * PREV_PX, nxtY + static_cast<int16_t>(pr) * PREV_PX,
                                  PREV_PX - 1, PREV_PX - 1);

    const int16_t hldLabelY = nxtY + 4 * PREV_PX + 3;
    display->drawString(rcx, hldLabelY, "HLD");
    const int16_t hldY = hldLabelY + FONT_HEIGHT_SMALL;
    const uint8_t heldType = game.heldPieceType();
    if (heldType != 255u) {
        for (uint8_t pr = 0; pr < 4; pr++)
            for (uint8_t pc = 0; pc < 4; pc++)
                if (TetrisGame::pieceCell(heldType, 0, pr, pc))
                    display->fillRect(previewX + static_cast<int16_t>(pc) * PREV_PX, hldY + static_cast<int16_t>(pr) * PREV_PX,
                                      PREV_PX - 1, PREV_PX - 1);
    } else {
        display->drawString(rcx, hldY + PREV_PX, "---");
    }
}

void Tetris::drawPlaying(OLEDDisplay *display, int16_t x, int16_t y)
{
    drawPlayfield(display, x, y);
}

// ---------------------------------------------------------------------------
// Mesh receive
// ---------------------------------------------------------------------------

ProcessMessage Tetris::handleReceived(const meshtastic_MeshPacket &mp)
{
#if !TETRIS_ANNOUNCE_HIGH_SCORE
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
    if (lb.game != meshtastic_GameType_GAME_TETRIS || lb.entries_count == 0)
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
            LOG_INFO("Tetris: remote score %lu from 0x%08x placed at rank %d", static_cast<unsigned long>(e.score), nodeNum,
                     rank + 1);
        }
    }
    if (changed)
        scores_.save();
    return ProcessMessage::CONTINUE;
#endif
}

// ---------------------------------------------------------------------------
// Mesh announce (TETRIS_ANNOUNCE_HIGH_SCORE only)
// ---------------------------------------------------------------------------

#if TETRIS_ANNOUNCE_HIGH_SCORE

int32_t Tetris::nextBroadcastIntervalMs() const
{
    const uint32_t now = millis();
    if (lastBroadcastMs == 0)
        return (now >= BROADCAST_INITIAL_MS) ? 0 : static_cast<int32_t>(BROADCAST_INITIAL_MS - now);
    const uint32_t elapsed = now - lastBroadcastMs;
    return (elapsed >= BROADCAST_INTERVAL_MS) ? 0 : static_cast<int32_t>(BROADCAST_INTERVAL_MS - elapsed);
}

int32_t Tetris::meshTick(GamesModule &host)
{
    const int32_t ms = nextBroadcastIntervalMs();
    if (ms == 0) {
        broadcastAllScores(host);
        lastBroadcastMs = millis();
        return static_cast<int32_t>(BROADCAST_INTERVAL_MS);
    }
    return ms;
}

void Tetris::broadcastAllScores(GamesModule &host)
{
#if GAME_DEMO_MODE
    return;
#endif
    if (!service)
        return;
    meshtastic_GameLeaderboard lb = meshtastic_GameLeaderboard_init_default;
    lb.game = meshtastic_GameType_GAME_TETRIS;
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
        LOG_INFO("Tetris: broadcast table (%u entries)", lb.entries_count);
    } else {
        LOG_WARN("Tetris: pb_encode table failed");
        packetPool.release(p);
    }
}

void Tetris::onNewHighScore(GamesModule &host, const char *initials, uint32_t score, bool isNewTop)
{
    if (score == 0 || !service)
        return;
#if GAME_DEMO_MODE
    if (!isNewTop)
        return;
    char msg[64];
    const char *n = (initials && initials[0]) ? initials : owner.short_name;
    snprintf(msg, sizeof(msg), "%s set a new Tetris high score: %lu", n, static_cast<unsigned long>(score));
    meshtastic_MeshPacket *p = host.gameAllocDataPacket();
    p->to = NODENUM_BROADCAST;
    p->channel = channels.getPrimaryIndex();
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p->want_ack = false;
    const pb_size_t msgLen = static_cast<pb_size_t>(strnlen(msg, sizeof(msg) - 1));
    memcpy(p->decoded.payload.bytes, msg, msgLen);
    p->decoded.payload.size = msgLen;
    service->sendToMesh(p);
    LOG_INFO("Tetris Demo: broadcast text '%s'", msg);
#else
    announceHighScore(host, score, initials);
#endif
}

void Tetris::announceHighScore(GamesModule &host, uint32_t score, const char *name)
{
    if (!service)
        return;
    meshtastic_GameLeaderboard lb = meshtastic_GameLeaderboard_init_default;
    lb.game = meshtastic_GameType_GAME_TETRIS;
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
        LOG_INFO("Tetris: broadcast score %lu", static_cast<unsigned long>(score));
    } else {
        LOG_WARN("Tetris: pb_encode score failed");
        packetPool.release(p);
    }
}

#endif // TETRIS_ANNOUNCE_HIGH_SCORE

#endif // HAS_SCREEN && BASEUI_HAS_GAMES
