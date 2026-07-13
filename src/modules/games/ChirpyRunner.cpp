#include "ChirpyRunner.h"

// ===========================================================================
// Pure ChirpyRunnerGame logic (no display/FS dependencies; always compiled)
// ===========================================================================

uint32_t ChirpyRunnerGame::nextRandom()
{
    uint32_t x = rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng = x;
    return x;
}

int16_t ChirpyRunnerGame::pickGapSteps()
{
    return static_cast<int16_t>(GAP_STEPS_MIN + static_cast<int16_t>(nextRandom() % (GAP_STEPS_MAX - GAP_STEPS_MIN + 1)));
}

void ChirpyRunnerGame::resetClouds()
{
    // Spread the clouds across the sky at staggered x, at varied heights near the top.
    for (uint8_t i = 0; i < CLOUD_COUNT; i++) {
        cloud[i].xSub = static_cast<int32_t>(i * (BOARD_W / CLOUD_COUNT) + 6) * SUBPX;
        cloud[i].y = static_cast<int16_t>(10 + nextRandom() % 10u); // 10..19 (below the score row)
    }
}

void ChirpyRunnerGame::scrollClouds()
{
    // Slow parallax drift; wrap back to the right (at a fresh height) once off the left edge.
    for (uint8_t i = 0; i < CLOUD_COUNT; i++) {
        cloud[i].xSub -= CLOUD_SPEED_SUB;
        if (cloud[i].xSub / SUBPX + CLOUD_W < 0) {
            cloud[i].xSub = static_cast<int32_t>(BOARD_W + nextRandom() % 24u) * SUBPX;
            cloud[i].y = static_cast<int16_t>(10 + nextRandom() % 10u);
        }
    }
}

void ChirpyRunnerGame::spawnObstacle()
{
    for (uint8_t i = 0; i < MAX_OBSTACLES; i++) {
        if (obst[i].active)
            continue;
        obst[i].active = true;
        obst[i].scored = false;
        obst[i].xSub = static_cast<int32_t>(BOARD_W) * SUBPX;
        obst[i].w = OBST_W;
        // Three height tiers so timing varies (kept clearable with margin for a forgiving jump).
        const uint32_t tier = nextRandom() % 3u;
        obst[i].h = BUILDING_HEIGHTS[tier];
        obst[i].colorIdx = static_cast<uint8_t>((spawnCount / 10u) % OBST_COLOR_COUNT);
        spawnCount++;
        return;
    }
}

void ChirpyRunnerGame::reset(uint32_t seed)
{
    rng = seed ? seed : 0xA5A5A5A5u; // xorshift32 must never be seeded with 0
    points = 0;
    alive = true;
    chirpyTop = groundedTopSub();
    vy = 0;
    grounded = true;
    for (uint8_t i = 0; i < MAX_OBSTACLES; i++)
        obst[i] = {};
    speedSub = SPEED_BASE;
    spawnTimer = 0; // first obstacle spawns on the first step
    spawnCount = 0;
    resetClouds();
}

void ChirpyRunnerGame::jump()
{
    if (!alive || !grounded)
        return;
    vy = -JUMP_V;
    grounded = false;
}

bool ChirpyRunnerGame::step()
{
    if (!alive)
        return false;

    scrollClouds(); // decorative background parallax

    // --- Chirpy vertical motion ---
    vy += GRAVITY;
    chirpyTop += vy;
    const int32_t gt = groundedTopSub();
    if (chirpyTop >= gt) {
        chirpyTop = gt;
        vy = 0;
        grounded = true;
    } else {
        grounded = false;
    }

    // --- Scroll obstacles, score, retire off-screen ones ---
    for (uint8_t i = 0; i < MAX_OBSTACLES; i++) {
        if (!obst[i].active)
            continue;
        obst[i].xSub -= speedSub;
        const int16_t ox = obstacleX(i);
        if (!obst[i].scored && ox + obst[i].w < CHIRPY_X) {
            obst[i].scored = true;
            points++;
        }
        if (ox + obst[i].w < 0)
            obst[i].active = false;
    }

    // --- Spawn on a tick timer (time-based spacing that scales with speed) ---
    if (spawnTimer > 0)
        spawnTimer--;
    if (spawnTimer <= 0) {
        spawnObstacle();
        spawnTimer = pickGapSteps();
    }

    // --- Difficulty ramp (scroll speed grows with score, then caps) ---
    const uint32_t capped = points < SPEED_CAP_PTS ? points : SPEED_CAP_PTS;
    speedSub = SPEED_BASE + static_cast<int32_t>(capped) * SPEED_INC;

    // --- Collision (forgiving hitbox: skip the antenna, inset the sides) ---
    const int16_t hx = CHIRPY_X + 2;
    const int16_t hxr = hx + (CHIRPY_W - 4);
    const int16_t hBottom = chirpyY() + CHIRPY_H;
    const int16_t hTop = chirpyY() + 4;
    for (uint8_t i = 0; i < MAX_OBSTACLES; i++) {
        if (!obst[i].active)
            continue;
        const int16_t ox = obstacleX(i);
        const int16_t oxr = ox + obst[i].w;
        const int16_t oTop = GROUND_Y - obst[i].h;
        if (hx < oxr && hxr > ox && hTop < GROUND_Y && hBottom > oTop) {
            alive = false;
            return false;
        }
    }

    return alive;
}

// ===========================================================================
// ChirpyRunner adapter (display + persistence; BaseUI games build only)
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
#include "mesh/generated/meshtastic/game.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>

ChirpyRunner::ChirpyRunner()
{
    scores_.load();
}

void ChirpyRunner::handleInput(input_broker_event ev)
{
    // SELECT is the jump (as requested); UP is accepted as a convenient alternate.
    if (ev == INPUT_BROKER_SELECT || ev == INPUT_BROKER_SELECT_LONG || ev == INPUT_BROKER_UP)
        game.jump();
}

void ChirpyRunner::drawAttract(OLEDDisplay *display, int16_t x, int16_t y)
{
    display->setColor(WHITE);
    const int16_t w = display->getWidth();
    const int16_t dH = display->getHeight();
    const int16_t cx = x + w / 2;
    const int16_t scale = dH / 64;
    // Scale vertical offsets proportionally to the actual display height.
    auto syOff = [&](int16_t gy) -> int16_t {
        return static_cast<int16_t>(y + static_cast<int32_t>(gy) * dH / ChirpyRunnerGame::BOARD_H);
    };
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    display->drawString(cx, y, "CHIRPY DASH");
    const int16_t logoX = x + (w - chirpy_run_width * scale) / 2;
    const int16_t logoY = syOff(15);
    drawXbmScaled(display, logoX, logoY, chirpy_run_width, chirpy_run_height, chirpy_run, scale);
#if GRAPHICS_TFT_COLORING_ENABLED
    // Chirpy is green, with white eyes. The eyes are the lit pixels at rows 5-7, cols 4-7 of the
    // glyph; a white region registered after the green one wins there.
    graphics::registerTFTColorRegionDirect(logoX, logoY, chirpy_run_width * scale, chirpy_run_height * scale,
                                           graphics::TFTPalette::MeshtasticGreen, graphics::getThemeBodyBg());
    graphics::registerTFTColorRegionDirect(logoX + 4 * scale, logoY + 5 * scale, 4 * scale, 3 * scale,
                                           graphics::TFTPalette::White, graphics::getThemeBodyBg());
#endif
    char hi[32];
    if (scores_.scoreAt(0) > 0 && scores_.nameAt(0)[0] != '\0')
        snprintf(hi, sizeof(hi), "High: %s %lu", scores_.nameAt(0), static_cast<unsigned long>(scores_.scoreAt(0)));
    else
        snprintf(hi, sizeof(hi), "High: %lu", static_cast<unsigned long>(scores_.scoreAt(0)));
    display->drawString(cx, syOff(34), hi);
    display->drawString(cx, syOff(48), "SEL=Play  Hold=Scores");
}

#if GRAPHICS_TFT_COLORING_ENABLED
// Obstacle colour palette; the game logic advances the index every 10 spawns.
static uint16_t obstacleColor(uint8_t idx)
{
    using namespace graphics;
    switch (idx) {
    case 0:
        return TFTPalette::Red;
    case 1:
        return TFTPalette::Orange;
    case 2:
        return TFTPalette::Yellow;
    case 3:
        return TFTPalette::Magenta;
    case 4:
        return TFTPalette::Cyan;
    default:
        return TFTPalette::Blue;
    }
}
#endif

void ChirpyRunner::drawPlaying(OLEDDisplay *display, int16_t x, int16_t y)
{
    display->setColor(WHITE);
    display->setFont(FONT_SMALL);

    const int16_t dW = display->getWidth();
    const int16_t dH = display->getHeight();

    // Project game-space coordinates to screen pixels using rational scale factors
    // (scaleX = dW/BOARD_W, scaleY = dH/BOARD_H), kept in integer arithmetic.
    auto sx = [&](int16_t gx) -> int16_t {
        return static_cast<int16_t>(x + static_cast<int32_t>(gx) * dW / ChirpyRunnerGame::BOARD_W);
    };
    auto sy = [&](int16_t gy) -> int16_t {
        return static_cast<int16_t>(y + static_cast<int32_t>(gy) * dH / ChirpyRunnerGame::BOARD_H);
    };
    // Scaled dimensions: minimum 1 to avoid zero-size rects.
    auto sw = [&](int16_t gw) -> int16_t {
        const int16_t r = static_cast<int16_t>(static_cast<int32_t>(gw) * dW / ChirpyRunnerGame::BOARD_W);
        return r > 0 ? r : static_cast<int16_t>(1);
    };
    auto sh = [&](int16_t gh) -> int16_t {
        const int16_t r = static_cast<int16_t>(static_cast<int32_t>(gh) * dH / ChirpyRunnerGame::BOARD_H);
        return r > 0 ? r : static_cast<int16_t>(1);
    };

    // Clouds drifting in the background (drawn first so everything else sits in front).
    for (uint8_t i = 0; i < ChirpyRunnerGame::cloudSlots(); i++) {
        const int16_t cxp = sx(game.cloudX(i));
        const int16_t cyp = sy(game.cloudY(i));
        display->fillRect(cxp + sw(2), cyp, sw(4), sh(1));
        display->fillRect(cxp + sw(1), cyp + sh(1), sw(6), sh(1));
        display->fillRect(cxp, cyp + sh(2), sw(8), sh(1));
#if GRAPHICS_TFT_COLORING_ENABLED
        graphics::registerTFTColorRegionDirect(cxp, cyp, sw(8), sh(3), graphics::TFTPalette::LightGray,
                                               graphics::getThemeBodyBg());
#endif
    }

    // Score (top-left).
    char buf[16];
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    snprintf(buf, sizeof(buf), "Sc %lu", static_cast<unsigned long>(game.score()));
    display->drawString(x + 2, y, buf);

    // Ground line.
    const int16_t groundY = sy(ChirpyRunnerGame::GROUND_Y);
    display->drawLine(x, groundY, x + dW - 1, groundY);

    // Obstacles drawn as little buildings: a solid tower with two columns of punched-out windows
    // (dark holes). On colour displays the walls cycle colour every 10 spawns and the windows glow
    // (they are the region's off-pixels, so they take the off-colour).
    for (uint8_t i = 0; i < ChirpyRunnerGame::obstacleSlots(); i++) {
        if (!game.obstacleActive(i))
            continue;
        const int16_t soh = sh(game.obstacleH(i));
        const int16_t sow = sw(game.obstacleW(i));
        const int16_t oxp = sx(game.obstacleX(i));
        const int16_t oyp = static_cast<int16_t>(groundY - soh);

        display->setColor(WHITE);
        display->fillRect(oxp, oyp, sow, soh);
        // Windows: holes in the left and right columns, every other scaled row, skipping the roof
        // and ground-floor rows so the tower reads as a building.
        display->setColor(BLACK);
        const int16_t wStep = sh(2);
        for (int16_t wy = oyp + sh(2); wy <= oyp + soh - sh(3); wy += wStep) {
            display->fillRect(oxp + sw(1), wy, sw(1), sh(1));
            display->fillRect(oxp + sow - sw(2), wy, sw(1), sh(1));
        }
        display->setColor(WHITE);
#if GRAPHICS_TFT_COLORING_ENABLED
        graphics::registerTFTColorRegionDirect(oxp, oyp, sow, soh, obstacleColor(game.obstacleColorIndex(i)),
                                               graphics::TFTPalette::White); // lit windows
#endif
    }

    // Chirpy sprite scaled to match the playfield's screen/board ratio.
    const int16_t spriteScale = dH / ChirpyRunnerGame::BOARD_H;
    const int16_t cxp = sx(ChirpyRunnerGame::CHIRPY_X);
    const int16_t cyp = sy(game.chirpyY());
    drawXbmScaled(display, cxp, cyp, chirpy_run_width, chirpy_run_height, chirpy_run, spriteScale);
#if GRAPHICS_TFT_COLORING_ENABLED
    graphics::registerTFTColorRegionDirect(cxp, cyp, chirpy_run_width * spriteScale, chirpy_run_height * spriteScale,
                                           graphics::TFTPalette::MeshtasticGreen, graphics::getThemeBodyBg());
    graphics::registerTFTColorRegionDirect(cxp + 4 * spriteScale, cyp + 5 * spriteScale, 4 * spriteScale, 3 * spriteScale,
                                           graphics::TFTPalette::White, graphics::getThemeBodyBg());
#endif
}

// ---------------------------------------------------------------------------
uint32_t ChirpyRunner::gameType() const
{
    return meshtastic_GameType_GAME_CHIRPY_RUNNER;
}

#if GAMES_ANNOUNCE_HIGH_SCORE

void ChirpyRunner::onAnnounceScore(GamesModule &host, const char *initials, uint32_t score)
{
    if (score == 0 || !service)
        return;
    announceHighScore(host, score, initials);
}

void ChirpyRunner::announceHighScore(GamesModule &host, uint32_t score, const char *name)
{
    if (!service)
        return;
    meshtastic_GameLeaderboard lb = meshtastic_GameLeaderboard_init_default;
    lb.game = meshtastic_GameType_GAME_CHIRPY_RUNNER;
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
        LOG_INFO("Chirpy: broadcast score %lu", static_cast<unsigned long>(score));
    } else {
        LOG_WARN("Chirpy: pb_encode score failed");
        packetPool.release(p);
    }
}

#endif // GAMES_ANNOUNCE_HIGH_SCORE

#endif // HAS_SCREEN && BASEUI_HAS_GAMES
