#include "Breakout.h"

// ===========================================================================
// Pure BreakoutGame logic (no display/FS dependencies; always compiled)
// ===========================================================================

uint32_t BreakoutGame::nextRandom()
{
    uint32_t x = rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng = x;
    return x;
}

void BreakoutGame::buildBricks()
{
    for (uint8_t r = 0; r < BRICK_ROWS; r++)
        for (uint8_t c = 0; c < BRICK_COLS; c++)
            bricks[r][c] = 1;
    bricksLeft = static_cast<uint16_t>(BRICK_ROWS) * BRICK_COLS;
}

void BreakoutGame::serveBall()
{
    // Centre the paddle and launch the ball upward from just above it, at a slight angle whose
    // side is chosen randomly so successive serves are not identical.
    paddleLeft = (BOARD_W - PADDLE_W) / 2;
    ballPxX = static_cast<int32_t>(BOARD_W / 2) * SUBPX;
    ballPxY = static_cast<int32_t>(PADDLE_Y - 2) * SUBPX;
    ballVx = (nextRandom() & 1u) ? 28 : -28;
    ballVy = -BALL_VY;
}

void BreakoutGame::nextLevel()
{
    levelNum++;
    buildBricks();
    serveBall();
}

void BreakoutGame::reset(uint32_t seed)
{
    rng = seed ? seed : 0xA5A5A5A5u; // xorshift32 must never be seeded with 0
    points = 0;
    livesLeft = START_LIVES;
    levelNum = 1;
    alive = true;
    ballTick = false;
    buildBricks();
    serveBall();
}

void BreakoutGame::movePaddle(int16_t dxPx)
{
    if (!alive)
        return;
    paddleLeft += dxPx;
    if (paddleLeft < 0)
        paddleLeft = 0;
    else if (paddleLeft > BOARD_W - PADDLE_W)
        paddleLeft = BOARD_W - PADDLE_W;
}

void BreakoutGame::moveLeft()
{
    movePaddle(-PADDLE_STEP);
}

void BreakoutGame::moveRight()
{
    movePaddle(PADDLE_STEP);
}

bool BreakoutGame::step()
{
    if (!alive)
        return false;

    // The ball advances on every other step() so the caller can tick (and poll the paddle) at twice
    // the ball's rate -- this keeps the ball speed constant while paddle control refreshes faster.
    ballTick = !ballTick;
    if (!ballTick)
        return true;

    ballPxX += ballVx;
    ballPxY += ballVy;

    int16_t px = static_cast<int16_t>(ballPxX / SUBPX);
    int16_t py = static_cast<int16_t>(ballPxY / SUBPX);

    // Side walls.
    if (px <= 0) {
        ballPxX = 0;
        px = 0;
        ballVx = -ballVx;
    } else if (px >= BOARD_W - 1) {
        ballPxX = static_cast<int32_t>(BOARD_W - 1) * SUBPX;
        px = BOARD_W - 1;
        ballVx = -ballVx;
    }
    // Top wall.
    if (py <= 0) {
        ballPxY = 0;
        py = 0;
        ballVy = -ballVy;
    }

    // Bricks: at most one brick per step (single, blocky reflection off the bottom/top face).
    if (py >= BRICK_TOP && py < BRICK_TOP + BRICK_ROWS * BRICK_H) {
        const int r = (py - BRICK_TOP) / BRICK_H;
        const int c = px / BRICK_W;
        if (r >= 0 && r < BRICK_ROWS && c >= 0 && c < BRICK_COLS && bricks[r][c]) {
            bricks[r][c] = 0;
            bricksLeft--;
            points += POINTS_PER_BRICK;
            ballVy = -ballVy;
            if (bricksLeft == 0) {
                nextLevel();
                return true;
            }
        }
    }

    // Paddle: bounce up and steer horizontally based on where the ball struck.
    if (ballVy > 0 && py >= PADDLE_Y - 1 && py <= PADDLE_Y + PADDLE_H) {
        if (px >= paddleLeft && px < paddleLeft + PADDLE_W) {
            ballPxY = static_cast<int32_t>(PADDLE_Y - 1) * SUBPX;
            ballVy = -BALL_VY;
            // Six zones across the paddle map to increasing outward angles; no zone is vertical.
            static const int16_t vxByZone[6] = {-48, -28, -8, 8, 28, 48};
            int zone = ((px - paddleLeft) * 6) / PADDLE_W;
            if (zone < 0)
                zone = 0;
            else if (zone > 5)
                zone = 5;
            ballVx = vxByZone[zone];
        }
    }

    // Ball lost past the bottom edge.
    if (py >= BOARD_H) {
        if (livesLeft > 0)
            livesLeft--;
        if (livesLeft == 0) {
            alive = false;
            return false;
        }
        serveBall();
    }

    return alive;
}

// ===========================================================================
// Breakout adapter (display + persistence; BaseUI games build only)
// ===========================================================================

#if HAS_SCREEN && BASEUI_HAS_GAMES

#include "GameUtils.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "graphics/Screen.h"
#include "graphics/ScreenFonts.h"
#include "graphics/TFTColorRegions.h"
#include "graphics/TFTPalette.h"
#include "graphics/images.h"
#include "main.h"
#include "mesh/Throttle.h"
#include "mesh/generated/meshtastic/game.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>
#if ARCH_PORTDUINO && defined(__linux__)
#include "input/LinuxJoystick.h"
#endif
#if defined(M5STACK_CARDPUTER_ADV)
#include "input/cardKbI2cImpl.h"
#endif

// Paddle pixels moved per tick while a joystick direction is held (continuous polling path).
static constexpr int16_t PADDLE_POLL_STEP = 3;

#if GRAPHICS_TFT_COLORING_ENABLED
// Classic Breakout brick-wall colours, top row to bottom.
static uint16_t brickRowColor(uint8_t row)
{
    using namespace graphics;
    switch (row) {
    case 0:
        return TFTPalette::Red;
    case 1:
        return TFTPalette::Orange;
    case 2:
        return TFTPalette::Yellow;
    case 3:
        return TFTPalette::Green;
    default:
        return TFTPalette::Cyan;
    }
}
#endif

Breakout::Breakout()
{
    scores_.load();
}

int32_t Breakout::tickIntervalMs() const
{
    // Tick at twice the ball's cadence: the ball advances every other step() (BreakoutGame::step),
    // so halving the interval keeps the ball speed the same while the paddle is polled/redrawn twice
    // as often. Speed ramps with level: ~22 ms base, floor 10 ms.
    int32_t iv = 45 - static_cast<int32_t>(game.level() - 1) * 5;
    if (iv < 20)
        iv = 20;
    return iv / 2;
}

bool Breakout::tick()
{
#if ARCH_PORTDUINO && defined(__linux__)
    if (aLinuxJoystick) {
        const int held = aLinuxJoystick->heldXZone();
        if (held < 0)
            game.movePaddle(-PADDLE_POLL_STEP);
        else if (held > 0)
            game.movePaddle(PADDLE_POLL_STEP);
    }
#elif TB_LEFT != 255
    // Device has discrete direction GPIO (INPUT_PULLUP, active-low) - poll pin state each tick
    // for true held detection and smooth acceleration.
    const bool heldLeft = !digitalRead(TB_LEFT);
    const bool heldRight = !digitalRead(TB_RIGHT);
    if (heldLeft && !heldRight) {
        if (paddleVel > 0)
            paddleVel = 0; // instant direction flip
        paddleVel = (paddleVel - 1 < -PADDLE_VEL_MAX) ? -PADDLE_VEL_MAX : static_cast<int16_t>(paddleVel - 1);
    } else if (heldRight && !heldLeft) {
        if (paddleVel < 0)
            paddleVel = 0;
        paddleVel = (paddleVel + 1 > PADDLE_VEL_MAX) ? PADDLE_VEL_MAX : static_cast<int16_t>(paddleVel + 1);
    } else {
        paddleVel = (paddleVel > 0) ? paddleVel - 1 : (paddleVel < 0) ? paddleVel + 1 : 0;
    }
    if (paddleVel != 0)
        game.movePaddle(paddleVel);
#elif defined(M5STACK_CARDPUTER_ADV)
    // Cardputer: poll the TCA8418 held-key state each tick for smooth continuous paddle movement.
    if (cardKbI2cImpl) {
        bool heldLeft = false, heldRight = false;
        cardKbI2cImpl->isNavKeyHeld(heldLeft, heldRight);
        if (heldLeft && !heldRight) {
            if (paddleVel > 0)
                paddleVel = 0;
            paddleVel = (paddleVel - 1 < -PADDLE_VEL_MAX) ? -PADDLE_VEL_MAX : static_cast<int16_t>(paddleVel - 1);
        } else if (heldRight && !heldLeft) {
            if (paddleVel < 0)
                paddleVel = 0;
            paddleVel = (paddleVel + 1 > PADDLE_VEL_MAX) ? PADDLE_VEL_MAX : static_cast<int16_t>(paddleVel + 1);
        } else {
            paddleVel = (paddleVel > 0) ? paddleVel - 1 : (paddleVel < 0) ? paddleVel + 1 : 0;
        }
        if (paddleVel != 0)
            game.movePaddle(paddleVel);
    }
#else
    // Fallback: event-driven velocity set by handleInput(), decay each tick.
    if (paddleVel != 0) {
        game.movePaddle(paddleVel);
        paddleVel += (paddleVel > 0) ? -1 : 1;
    }
#endif
    return game.step();
}

void Breakout::handleInput(input_broker_event ev)
{
#if ARCH_PORTDUINO && defined(__linux__)
    if (aLinuxJoystick)
        return;
#endif
#if TB_LEFT != 255 || defined(M5STACK_CARDPUTER_ADV)
    // Paddle is polled every tick() (GPIO or held-key query); ignore LEFT/RIGHT events to avoid
    // double-moving on the key-release edge.
    if (ev == INPUT_BROKER_LEFT || ev == INPUT_BROKER_RIGHT)
        return;
#endif
    const uint32_t now = millis();
    switch (ev) {
    case INPUT_BROKER_LEFT: {
        const bool sameDir = (paddleVel < 0);
        const bool recent = Throttle::isWithinTimespanMs(lastDirEventMs, PADDLE_ACCEL_WINDOW_MS);
        if (sameDir && recent)
            paddleVel = (paddleVel - PADDLE_VEL_STEP < -PADDLE_VEL_MAX) ? -PADDLE_VEL_MAX
                                                                        : static_cast<int16_t>(paddleVel - PADDLE_VEL_STEP);
        else
            paddleVel = -PADDLE_VEL_STEP;
        lastDirEventMs = now;
        break;
    }
    case INPUT_BROKER_RIGHT: {
        const bool sameDir = (paddleVel > 0);
        const bool recent = Throttle::isWithinTimespanMs(lastDirEventMs, PADDLE_ACCEL_WINDOW_MS);
        if (sameDir && recent)
            paddleVel = (paddleVel + PADDLE_VEL_STEP > PADDLE_VEL_MAX) ? PADDLE_VEL_MAX
                                                                       : static_cast<int16_t>(paddleVel + PADDLE_VEL_STEP);
        else
            paddleVel = PADDLE_VEL_STEP;
        lastDirEventMs = now;
        break;
    }
    default:
        break;
    }
}

void Breakout::drawAttract(OLEDDisplay *display, int16_t x, int16_t y)
{
    display->setColor(WHITE);
    const int16_t w = display->getWidth();
    const int16_t dH = display->getHeight();
    const int16_t cx = x + w / 2;
    const int16_t scale = dH / 64;
    auto syOff = [&](int16_t gy) -> int16_t { return static_cast<int16_t>(y + static_cast<int32_t>(gy) * dH / 64); };
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    display->drawString(cx, y, "B R E A K O U T");
    const int16_t logoX = x + (w - breakout_width * scale) / 2;
    const int16_t logoY = syOff(15);
    drawXbmScaled(display, logoX, logoY, breakout_width, breakout_height, breakout, scale);
#if GRAPHICS_TFT_COLORING_ENABLED
    // The glyph is three brick courses, a ball, and a paddle -- colour each to match the game.
    const uint16_t abg = graphics::getThemeBodyBg();
    graphics::registerTFTColorRegionDirect(logoX, logoY + 1 * scale, breakout_width * scale, 2 * scale, graphics::TFTPalette::Red,
                                           abg);
    graphics::registerTFTColorRegionDirect(logoX, logoY + 4 * scale, breakout_width * scale, 2 * scale,
                                           graphics::TFTPalette::Yellow, abg);
    graphics::registerTFTColorRegionDirect(logoX, logoY + 7 * scale, breakout_width * scale, 2 * scale,
                                           graphics::TFTPalette::Green, abg);
    graphics::registerTFTColorRegionDirect(logoX + 4 * scale, logoY + 14 * scale, 8 * scale, 2 * scale,
                                           graphics::TFTPalette::Blue, abg); // paddle
    graphics::registerTFTColorRegionDirect(logoX + 7 * scale, logoY + 10 * scale, 2 * scale, 2 * scale,
                                           graphics::TFTPalette::White, abg); // ball
#endif
    char hi[32];
    if (scores_.scoreAt(0) > 0 && scores_.nameAt(0)[0] != '\0')
        snprintf(hi, sizeof(hi), "High: %s %lu", scores_.nameAt(0), static_cast<unsigned long>(scores_.scoreAt(0)));
    else
        snprintf(hi, sizeof(hi), "High: %lu", static_cast<unsigned long>(scores_.scoreAt(0)));
    display->drawString(cx, syOff(34), hi);
    display->drawString(cx, syOff(48), "SEL=Play  Hold=Scores");
}

void Breakout::drawPlaying(OLEDDisplay *display, int16_t x, int16_t y)
{
    display->setColor(WHITE);
    display->setFont(FONT_SMALL);

    const int16_t dW = display->getWidth();
    const int16_t dH = display->getHeight();

    // Project game-space coordinates (BOARD_W x BOARD_H) to screen pixels.
    auto sx = [&](int16_t gx) -> int16_t {
        return static_cast<int16_t>(x + static_cast<int32_t>(gx) * dW / BreakoutGame::BOARD_W);
    };
    auto sy = [&](int16_t gy) -> int16_t {
        return static_cast<int16_t>(y + static_cast<int32_t>(gy) * dH / BreakoutGame::BOARD_H);
    };
    auto sw = [&](int16_t gw) -> int16_t {
        const int16_t r = static_cast<int16_t>(static_cast<int32_t>(gw) * dW / BreakoutGame::BOARD_W);
        return r > 0 ? r : static_cast<int16_t>(1);
    };
    auto sh = [&](int16_t gh) -> int16_t {
        const int16_t r = static_cast<int16_t>(static_cast<int32_t>(gh) * dH / BreakoutGame::BOARD_H);
        return r > 0 ? r : static_cast<int16_t>(1);
    };

    // Score row (top-left), remaining lives as small squares (top-right).
    char buf[16];
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    snprintf(buf, sizeof(buf), "Sc %lu", static_cast<unsigned long>(game.score()));
    display->drawString(x + 2, y, buf);
    for (uint8_t i = 0; i < game.lives(); i++)
        display->fillRect(x + dW - sw(3) - i * sw(4), sy(2), sw(2), sh(2));

    // Bricks.
    for (uint8_t r = 0; r < BreakoutGame::BRICK_ROWS; r++)
        for (uint8_t c = 0; c < BreakoutGame::BRICK_COLS; c++)
            if (game.brickAt(r, c))
                display->fillRect(sx(c * BreakoutGame::BRICK_W), sy(BreakoutGame::BRICK_TOP + r * BreakoutGame::BRICK_H),
                                  sw(BreakoutGame::BRICK_W - 1), sh(BreakoutGame::BRICK_H - 1));

    // Paddle.
    display->fillRect(sx(game.paddleX()), sy(BreakoutGame::PADDLE_Y), sw(BreakoutGame::PADDLE_W), sh(BreakoutGame::PADDLE_H));

    // Ball.
    display->fillRect(sx(game.ballX()), sy(game.ballY()), sw(2), sh(2));

#if GRAPHICS_TFT_COLORING_ENABLED
    // Colour the wall by row, plus a blue paddle and white ball.
    const uint16_t bg = graphics::getThemeBodyBg();
    for (uint8_t r = 0; r < BreakoutGame::BRICK_ROWS; r++)
        graphics::registerTFTColorRegionDirect(x, sy(BreakoutGame::BRICK_TOP + r * BreakoutGame::BRICK_H), dW,
                                               sh(BreakoutGame::BRICK_H - 1), brickRowColor(r), bg);
    graphics::registerTFTColorRegionDirect(sx(game.paddleX()), sy(BreakoutGame::PADDLE_Y), sw(BreakoutGame::PADDLE_W),
                                           sh(BreakoutGame::PADDLE_H), graphics::TFTPalette::Blue, bg);
    graphics::registerTFTColorRegionDirect(sx(game.ballX()), sy(game.ballY()), sw(2), sh(2), graphics::TFTPalette::White, bg);
#endif
}

// ---------------------------------------------------------------------------
uint32_t Breakout::gameType() const
{
    return meshtastic_GameType_GAME_BREAKOUT;
}

#if GAMES_ANNOUNCE_HIGH_SCORE

void Breakout::onAnnounceScore(GamesModule &host, const char *initials, uint32_t score)
{
    if (score == 0 || !service)
        return;
    announceHighScore(host, score, initials);
}

void Breakout::announceHighScore(GamesModule &host, uint32_t score, const char *name)
{
    if (!service)
        return;
    meshtastic_GameLeaderboard lb = meshtastic_GameLeaderboard_init_default;
    lb.game = meshtastic_GameType_GAME_BREAKOUT;
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
        LOG_INFO("Breakout: broadcast score %lu", static_cast<unsigned long>(score));
    } else {
        LOG_WARN("Breakout: pb_encode score failed");
        packetPool.release(p);
    }
}

#endif // GAMES_ANNOUNCE_HIGH_SCORE

#endif // HAS_SCREEN && BASEUI_HAS_GAMES
