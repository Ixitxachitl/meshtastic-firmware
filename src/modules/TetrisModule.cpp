#include "TetrisModule.h"

#if HAS_SCREEN && BASEUI_HAS_GAMES

#include "FSCommon.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "PowerFSM.h"
#include "SPILock.h"
#include "SafeFile.h"
#include "buzz/buzz.h"
#include "concurrency/LockGuard.h"
#include "gps/RTC.h"
#include "graphics/Screen.h"
#include "graphics/ScreenFonts.h"
#include "graphics/images.h"
#include "main.h"
#include <ErriezCRC32.h>
#include <cstddef>

TetrisModule *tetrisModule;

static constexpr uint8_t TETRIS_WIRE_VERSION = 1;
static constexpr uint8_t TETRIS_WIRE_GAME_ID = 0x54u; // 'T'
#if TETRIS_ANNOUNCE_HIGH_SCORE
static constexpr uint32_t BROADCAST_INITIAL_MS = 60000UL;
static constexpr uint32_t BROADCAST_INTERVAL_MS = 12UL * 60 * 60 * 1000;
#endif

// Wire structs - game_id byte makes sizes 11 / 68 (vs Snake's 10 / 67).
struct TetrisScoreWire {
    uint8_t game_id; // = TETRIS_WIRE_GAME_ID
    uint8_t version;
    char shortName[5];
    uint32_t score;
} __attribute__((packed)); // 11 bytes

struct TetrisTableEntry {
    uint32_t nodeNum;
    char shortName[5];
    uint32_t score;
} __attribute__((packed)); // 13 bytes

struct TetrisTableWire {
    uint8_t game_id; // = TETRIS_WIRE_GAME_ID
    uint8_t version;
    uint8_t count;
    TetrisTableEntry entries[5];
} __attribute__((packed)); // 68 bytes

// ---------------------------------------------------------------------------
// Vertical pixel layout on a 128×64 OLED
//
//  Board occupies the left side of the screen:
//    x = col × CELL_PX   (col 0 at left edge)
//    y = row × CELL_PX   (row 0 at top edge)
//    10 cols × 4 px = 40 px wide
//    16 rows × 4 px = 64 px tall  (fills the full display height)
//
//  Score panel: x = SCORE_OX .. 127  (86 px wide)
//    Labels (SCR / LVL / NXT) + values + next-piece preview.
// ---------------------------------------------------------------------------
static constexpr int16_t CELL_PX = 4;
static constexpr const char *TETRIS_HS_FILE = "/prefs/tetris.dat";

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

TetrisModule::TetrisModule() : SinglePortModule("tetris", meshtastic_PortNum_PRIVATE_APP), concurrency::OSThread("Tetris")
{
    loadHighScores();
    inputObserver.observe(inputBroker);
#if TETRIS_ANNOUNCE_HIGH_SCORE
    setIntervalFromNow(BROADCAST_INITIAL_MS);
#else
    disable();
#endif
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void TetrisModule::showTitle()
{
    if (!highScoresLoaded)
        loadHighScores();
    uiState = TETRIS_TITLE;
    requestRedraw();
}

void TetrisModule::launchGame()
{
    if (!highScoresLoaded)
        loadHighScores();
    startPlaying();
}

void TetrisModule::startPlaying()
{
    game.reset(static_cast<uint32_t>(random()) ^ millis());
    uiState = TETRIS_PLAYING;
    lockDelayActive = false;
    lastAwakeKickMs = millis();
    kickTick();
    requestRedraw();
}

void TetrisModule::enterGameOver()
{
    lastScore = game.score();
    if (!highScoresLoaded)
        loadHighScores();
    lastRank = -1;
    lastWasNewTop = false;
    uiState = TETRIS_GAMEOVER;
#if GAME_DEMO_MODE
    if (qualifiesForHighScore(lastScore))
        promptForInitials();
#else
    if (qualifiesForHighScore(lastScore))
        recordHighScore();
#endif
    requestRedraw();
}

void TetrisModule::exitToIdle()
{
    uiState = TETRIS_INACTIVE;
    disable();
    // The games frame trampoline falls through to Snake's attract screen now.
    requestRedraw();
}

void TetrisModule::requestRedraw()
{
    UIFrameEvent e;
    e.action = UIFrameEvent::Action::REDRAW_ONLY;
    notifyObservers(&e);
}

void TetrisModule::kickTick()
{
    enabled = true;
    setIntervalFromNow(250);
}

int32_t TetrisModule::tickIntervalMs() const
{
    // Speed ramps with level: 600 ms base, 30 ms per level, floor 80 ms.
    int32_t iv = 600 - static_cast<int32_t>(game.level()) * 30;
    return iv < 80 ? 80 : iv;
}

// ---------------------------------------------------------------------------
// OSThread tick
// ---------------------------------------------------------------------------

int32_t TetrisModule::runOnce()
{
    if (uiState != TETRIS_PLAYING) {
#if TETRIS_ANNOUNCE_HIGH_SCORE
        const int32_t bcastMs = nextBroadcastIntervalMs();
        if (bcastMs == 0) {
            broadcastAllScores();
            lastBroadcastMs = millis();
            return static_cast<int32_t>(BROADCAST_INTERVAL_MS);
        }
        return bcastMs;
#else
        return disable();
#endif
    }

    const uint32_t now = millis();

    if (pendingLineClearChirp) {
        pendingLineClearChirp = false;
        playChirp();
    }

    if (game.isGrounded()) {
        if (!lockDelayActive) {
            // Piece just landed - start the lock-delay countdown.
            lockDelayStartMs = now;
            lockDelayActive = true;
        } else if (now - lockDelayStartMs >= LOCK_DELAY_MS) {
            // Delay expired - lock the piece.
            lockDelayActive = false;
            game.lockNow();
            if (!game.isPlaying()) {
                enterGameOver();
                return disable();
            }
            if (game.lastLinesClearedCount() > 0)
                playChirp();
        } else {
            // Still waiting - come back when the delay expires.
            requestRedraw();
            return static_cast<int32_t>(LOCK_DELAY_MS - (now - lockDelayStartMs));
        }
    } else {
        lockDelayActive = false;
        game.tryGravity();
    }

    if (now - lastAwakeKickMs > 1500) {
        powerFSM.trigger(EVENT_PRESS);
        lastAwakeKickMs = now;
    }
    requestRedraw();
    return tickIntervalMs();
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

int TetrisModule::handleInputEvent(const InputEvent *event)
{
    if (!isActive())
        return 0;
    if (screen && screen->isOverlayBannerShowing())
        return 0;

    const input_broker_event ev = event->inputEvent;
    const bool isBack = (ev == INPUT_BROKER_CANCEL || ev == INPUT_BROKER_BACK);

    switch (uiState) {
    case TETRIS_TITLE:
        if (ev == INPUT_BROKER_SELECT) {
            startPlaying();
            return 1;
        } else if (isBack) {
            // Let the normal system handle BACK (turns display off), but clean up first.
            exitToIdle();
            return 0;
        } else if (ev == INPUT_BROKER_UP || ev == INPUT_BROKER_DOWN) {
            exitToIdle();
            return 1;
        } else if (ev == INPUT_BROKER_SELECT_LONG) {
            uiState = TETRIS_HISCORES;
            requestRedraw();
            return 1;
        } else if (ev == INPUT_BROKER_LEFT) {
            exitToIdle();
            if (screen)
                screen->showPrevFrame();
            return 1;
        } else if (ev == INPUT_BROKER_RIGHT) {
            exitToIdle();
            if (screen)
                screen->showNextFrame();
            return 1;
        }
        return 0;

    case TETRIS_PLAYING:
        if (isBack) {
            uiState = TETRIS_PAUSED;
            disable();
            lockDelayActive = false;
            requestRedraw();
        } else {
            switch (ev) {
            case INPUT_BROKER_UP:
                if (game.rotate() && lockDelayActive)
                    lockDelayStartMs = millis(); // reset lock delay on successful rotate
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
                    lockDelayActive = false; // moved down, no longer grounded
                } else if (lockDelayActive) {
                    lockDelayStartMs = millis(); // reset timer on soft drop while grounded
                }
                break;
            case INPUT_BROKER_SELECT:
                // Hard drop: bypass lock delay and lock immediately.
                lockDelayActive = false;
                game.hardDrop();
                if (!game.isPlaying()) {
                    enterGameOver();
                    return 1;
                }
                if (game.lastLinesClearedCount() > 0)
                    pendingLineClearChirp = true;
                break;
            case INPUT_BROKER_SELECT_LONG:
                // Hold / swap piece.
                if (game.holdPiece()) {
                    lockDelayActive = false; // new piece in play, reset delay
                    if (!game.isPlaying()) {
                        enterGameOver();
                        return 1;
                    }
                }
                break;
            default:
                break;
            }
            requestRedraw();
        }
        return 1;

    case TETRIS_PAUSED:
        if (isBack) {
            showTitle();
        } else if (ev == INPUT_BROKER_SELECT || ev == INPUT_BROKER_UP || ev == INPUT_BROKER_DOWN || ev == INPUT_BROKER_LEFT ||
                   ev == INPUT_BROKER_RIGHT) {
            uiState = TETRIS_PLAYING;
            kickTick();
            requestRedraw();
        }
        return 1;

    case TETRIS_GAMEOVER:
        if (ev == INPUT_BROKER_SELECT) {
            uiState = TETRIS_HISCORES;
            requestRedraw();
        } else if (isBack) {
            showTitle(); // back to Tetris title, not Snake
        }
        return 1;

    case TETRIS_HISCORES:
        if (ev == INPUT_BROKER_SELECT_LONG) {
            static const char *opts[] = {"No", "Yes"};
            graphics::BannerOverlayOptions confirm;
            confirm.message = "Clear Scores?";
            confirm.optionsArrayPtr = opts;
            confirm.optionsCount = 2;
            confirm.bannerCallback = [this](int selected) {
                if (selected == 1) {
                    memset(highScores, 0, sizeof(highScores));
                    saveHighScores();
                    requestRedraw();
                    LOG_INFO("Tetris: high scores cleared");
                }
            };
            if (screen)
                screen->showOverlayBanner(confirm);
        } else if (ev == INPUT_BROKER_SELECT || isBack) {
            showTitle(); // back to Tetris title, not Snake
        }
        return 1;

    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// Rendering helpers
// ---------------------------------------------------------------------------

void TetrisModule::drawCenteredLines(OLEDDisplay *display, int16_t x, int16_t y, const char *const *lines, uint8_t count)
{
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    const int16_t lineH = FONT_HEIGHT_SMALL;
    const int16_t total = static_cast<int16_t>(count) * lineH;
    int16_t startY = y + (display->getHeight() - total) / 2;
    if (startY < y)
        startY = y;
    const int16_t cx = x + display->getWidth() / 2;
    for (uint8_t i = 0; i < count; i++)
        display->drawString(cx, startY + i * lineH, lines[i]);
}

void TetrisModule::drawHighScores(OLEDDisplay *display, int16_t x, int16_t y)
{
    display->setFont(FONT_SMALL);
    display->setColor(WHITE);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->drawString(x, y, "HIGH SCORES");
    display->setTextAlignment(TEXT_ALIGN_RIGHT);
    display->drawString(x + display->getWidth() - 2, y, "Hold=Clear");
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    const int16_t rowH = (display->getHeight() - FONT_HEIGHT_SMALL) / HS_COUNT;
    for (uint8_t i = 0; i < HS_COUNT; i++) {
        char row[32];
        if (highScores[i].score > 0) {
            snprintf(row, sizeof(row), "%u. %-4s %lu", static_cast<unsigned>(i + 1), highScores[i].shortName,
                     static_cast<unsigned long>(highScores[i].score));
        } else {
            snprintf(row, sizeof(row), "%u. ---", static_cast<unsigned>(i + 1));
        }
        display->drawString(x + 6, y + FONT_HEIGHT_SMALL + i * rowH, row);
    }
}

void TetrisModule::drawPlayfield(OLEDDisplay *display, int16_t x, int16_t y)
{
    // Layout (128×64 OLED):
    //   left panel  (SCR / LVL)       : x = 0 .. ox-2
    //   board (centered, 40×64 px)    : x = ox .. ox+boardW-1
    //   right panel (NXT / HLD)        : x = ox+boardW+1 .. 127
    const int16_t boardW = TetrisGame::BOARD_COLS * CELL_PX;   // 40 px
    const int16_t ox = x + (display->getWidth() - boardW) / 2; // board left edge
    const int16_t oy = y;

    display->setColor(WHITE);

    // Board separator lines and bottom wall.
    display->drawLine(ox - 1, oy, ox - 1, oy + display->getHeight() - 1);
    display->drawLine(ox + boardW, oy, ox + boardW, oy + display->getHeight() - 1);
    display->drawLine(ox - 1, oy + display->getHeight() - 1, ox + boardW, oy + display->getHeight() - 1);

    // Cell helper.
    auto drawCell = [&](int8_t col, int8_t row) {
        if (col < 0 || row < 0 || col >= TetrisGame::BOARD_COLS || row >= TetrisGame::BOARD_ROWS)
            return;
        display->fillRect(ox + static_cast<int16_t>(col) * CELL_PX, oy + static_cast<int16_t>(row) * CELL_PX, CELL_PX - 1,
                          CELL_PX - 1);
    };

    // Locked cells.
    for (uint8_t r = 0; r < TetrisGame::BOARD_ROWS; r++)
        for (uint8_t c = 0; c < TetrisGame::BOARD_COLS; c++)
            if (game.board[r][c])
                drawCell(static_cast<int8_t>(c), static_cast<int8_t>(r));

    // Ghost piece - hollow outline.
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
                display->drawRect(ox + static_cast<int16_t>(gc) * CELL_PX, oy + static_cast<int16_t>(gr) * CELL_PX, CELL_PX - 1,
                                  CELL_PX - 1);
            }
        }
    }

    // Active piece - filled.
    for (uint8_t pr = 0; pr < 4; pr++) {
        for (uint8_t pc = 0; pc < 4; pc++) {
            if (!TetrisGame::pieceCell(cur.type, cur.rot, pr, pc))
                continue;
            drawCell(static_cast<int8_t>(cur.col + pc), static_cast<int8_t>(cur.row + pr));
        }
    }

    // --- Left panel: SCR and LVL ---
    char buf[12];
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->drawString(x + 2, y + 2, "SCR");
    snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(game.score()));
    display->drawString(x + 2, y + 2 + FONT_HEIGHT_SMALL, buf);
    display->drawString(x + 2, y + 2 + FONT_HEIGHT_SMALL * 2 + 2, "LVL");
    snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(game.level()));
    display->drawString(x + 2, y + 2 + FONT_HEIGHT_SMALL * 3 + 2, buf);

    // --- Right panel: NXT and HLD previews ---
    const int16_t rpx = ox + boardW + 2;               // right panel left edge
    const int16_t rpanelW = display->getWidth() - rpx; // ~42 px
    const int16_t rcx = rpx + rpanelW / 2;             // centre of right panel
    static constexpr int16_t PREV_PX = 3;              // preview cell size (px)
    const int16_t previewW = 4 * PREV_PX;              // 12 px
    const int16_t previewX = rpx + (rpanelW - previewW) / 2;

    display->setTextAlignment(TEXT_ALIGN_CENTER);

    // NXT label + preview
    display->drawString(rcx, y + 2, "NXT");
    const int16_t nxtY = y + 2 + FONT_HEIGHT_SMALL;
    const TetrisGame::Piece &nxt = game.next();
    for (uint8_t pr = 0; pr < 4; pr++)
        for (uint8_t pc = 0; pc < 4; pc++)
            if (TetrisGame::pieceCell(nxt.type, nxt.rot, pr, pc))
                display->fillRect(previewX + static_cast<int16_t>(pc) * PREV_PX, nxtY + static_cast<int16_t>(pr) * PREV_PX,
                                  PREV_PX - 1, PREV_PX - 1);

    // HLD label + preview (or dashes if empty)
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

// ---------------------------------------------------------------------------
// drawFrame - the entry point called by Screen's games-frame trampoline
// ---------------------------------------------------------------------------

void TetrisModule::drawFrame(OLEDDisplay *display, OLEDDisplayUiState * /*state*/, int16_t x, int16_t y)
{
    display->setColor(WHITE);

    switch (uiState) {
    case TETRIS_TITLE: {
        const int16_t w = display->getWidth();
        const int16_t cx = x + w / 2;
        display->setFont(FONT_SMALL);
        display->setTextAlignment(TEXT_ALIGN_CENTER);
        display->drawString(cx, y, "T E T R I S");
        display->drawXbm(x + (w - tetris_width) / 2, y + 15, tetris_width, tetris_height, tetris);
        char hi[32];
        if (highScores[0].score > 0 && highScores[0].shortName[0] != '\0')
            snprintf(hi, sizeof(hi), "High: %s %lu", highScores[0].shortName, static_cast<unsigned long>(highScores[0].score));
        else
            snprintf(hi, sizeof(hi), "High: %lu", static_cast<unsigned long>(highScores[0].score));
        display->drawString(cx, y + 34, hi);
        display->drawString(cx, y + 48, "SEL=Play  Hold=Scores");
        break;
    }

    case TETRIS_PLAYING:
        drawPlayfield(display, x, y);
        break;

    case TETRIS_PAUSED: {
        drawPlayfield(display, x, y);
        display->setFont(FONT_SMALL);
        display->setTextAlignment(TEXT_ALIGN_CENTER);
        display->drawString(x + display->getWidth() / 2, y + display->getHeight() / 2 - FONT_HEIGHT_SMALL / 2, "- PAUSED -");
        break;
    }

    case TETRIS_GAMEOVER: {
        char scoreLine[24];
        snprintf(scoreLine, sizeof(scoreLine), "Score: %lu", static_cast<unsigned long>(lastScore));
        const char *status = lastWasNewTop ? "NEW HIGH SCORE!" : (lastRank >= 0 ? "You made the top 5!" : "");
        const char *lines[] = {"GAME OVER", scoreLine, status, "SEL: scores  BCK: exit"};
        drawCenteredLines(display, x, y, lines, 4);
        break;
    }

    case TETRIS_HISCORES:
        drawHighScores(display, x, y);
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// High-score table
// ---------------------------------------------------------------------------

bool TetrisModule::qualifiesForHighScore(uint32_t score) const
{
    if (score == 0)
        return false;
    for (int i = 0; i < HS_COUNT; i++) {
        if (score > highScores[i].score)
            return true;
    }
    return false;
}

int TetrisModule::insertHighScore(uint32_t score, const char *name, uint32_t nodeNum, bool &isNewTop)
{
    isNewTop = false;
    if (score == 0)
        return -1;
    int pos = -1;
    for (int i = 0; i < HS_COUNT; i++) {
        if (score > highScores[i].score) {
            pos = i;
            break;
        }
    }
    if (pos < 0)
        return -1;
    for (int i = HS_COUNT - 1; i > pos; i--)
        highScores[i] = highScores[i - 1];
    HighScoreEntry &e = highScores[pos];
    memset(&e, 0, sizeof(e));
    e.score = score;
    e.nodeNum = nodeNum;
    strncpy(e.shortName, (name && name[0]) ? name : owner.short_name, sizeof(e.shortName) - 1);
    e.shortName[sizeof(e.shortName) - 1] = '\0';
    e.epoch = getValidTime(RTCQualityDevice, false);
    isNewTop = (pos == 0);
    return pos;
}

#if GAME_DEMO_MODE
void TetrisModule::promptForInitials()
{
    if (screen)
        screen->showAlphanumericPicker("New High Score!\nEnter initials", "AAA", 60000, 3,
                                       [this](const std::string &initials) { this->recordHighScore(initials.c_str()); });
    else
        recordHighScore(nullptr);
}
#endif

void TetrisModule::recordHighScore(const char *initials)
{
    bool isNewTop = false;
    const char *name = (initials && initials[0]) ? initials : owner.short_name;
    lastRank = insertHighScore(lastScore, name, nodeDB ? nodeDB->getNodeNum() : 0, isNewTop);
    lastWasNewTop = isNewTop;
    if (lastRank >= 0)
        saveHighScores();
#if TETRIS_ANNOUNCE_HIGH_SCORE
    if (lastScore > 0 && lastRank >= 0) {
#if GAME_DEMO_MODE
        if (isNewTop)
#endif
            announceHighScore(lastScore, name);
    }
#endif
    requestRedraw();
}

// ---------------------------------------------------------------------------
// Mesh receive - merge remote Tetris scores into local table
// ---------------------------------------------------------------------------

ProcessMessage TetrisModule::handleReceived(const meshtastic_MeshPacket &mp)
{
#if !TETRIS_ANNOUNCE_HIGH_SCORE
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

    const size_t sz = mp.decoded.payload.size;

    if (sz == sizeof(TetrisTableWire)) {
        TetrisTableWire tbl;
        memcpy(&tbl, mp.decoded.payload.bytes, sizeof(tbl));
        if (tbl.game_id != TETRIS_WIRE_GAME_ID || tbl.version != TETRIS_WIRE_VERSION)
            return ProcessMessage::CONTINUE;
        const uint8_t count = tbl.count < HS_COUNT ? tbl.count : HS_COUNT;
        bool changed = false;
        for (uint8_t i = 0; i < count; i++) {
            if (tbl.entries[i].score == 0)
                continue;
            tbl.entries[i].shortName[sizeof(tbl.entries[0].shortName) - 1] = '\0';
            bool dummy = false;
            const int rank = insertHighScore(tbl.entries[i].score, tbl.entries[i].shortName, tbl.entries[i].nodeNum, dummy);
            if (rank >= 0)
                changed = true;
        }
        if (changed) {
            saveHighScores();
            requestRedraw();
        }
        return ProcessMessage::CONTINUE;
    }

    if (sz == sizeof(TetrisScoreWire)) {
        TetrisScoreWire wire;
        memcpy(&wire, mp.decoded.payload.bytes, sizeof(wire));
        if (wire.game_id != TETRIS_WIRE_GAME_ID || wire.version != TETRIS_WIRE_VERSION || wire.score == 0)
            return ProcessMessage::CONTINUE;
        wire.shortName[sizeof(wire.shortName) - 1] = '\0';
        bool dummy = false;
        const int rank = insertHighScore(wire.score, wire.shortName, mp.from, dummy);
        if (rank >= 0) {
            saveHighScores();
            requestRedraw();
            LOG_INFO("Tetris: remote score %lu from 0x%08x placed at rank %d", static_cast<unsigned long>(wire.score), mp.from,
                     rank + 1);
        }
        return ProcessMessage::CONTINUE;
    }

    return ProcessMessage::CONTINUE;
#endif // TETRIS_ANNOUNCE_HIGH_SCORE
}

#if TETRIS_ANNOUNCE_HIGH_SCORE
int32_t TetrisModule::nextBroadcastIntervalMs() const
{
    const uint32_t now = millis();
    if (lastBroadcastMs == 0)
        return (now >= BROADCAST_INITIAL_MS) ? 0 : static_cast<int32_t>(BROADCAST_INITIAL_MS - now);
    const uint32_t elapsed = now - lastBroadcastMs;
    return (elapsed >= BROADCAST_INTERVAL_MS) ? 0 : static_cast<int32_t>(BROADCAST_INTERVAL_MS - elapsed);
}

void TetrisModule::broadcastAllScores()
{
#if GAME_DEMO_MODE
    return; // Demo mode: individual scores are broadcast as text; no binary table.
#endif
    if (!service)
        return;
    TetrisTableWire tbl;
    memset(&tbl, 0, sizeof(tbl));
    tbl.game_id = TETRIS_WIRE_GAME_ID;
    tbl.version = TETRIS_WIRE_VERSION;
    tbl.count = 0;
    for (uint8_t i = 0; i < HS_COUNT; i++) {
        if (highScores[i].score == 0)
            break;
        tbl.entries[tbl.count].nodeNum = highScores[i].nodeNum;
        strncpy(tbl.entries[tbl.count].shortName, highScores[i].shortName, sizeof(tbl.entries[0].shortName) - 1);
        tbl.entries[tbl.count].shortName[sizeof(tbl.entries[0].shortName) - 1] = '\0';
        tbl.entries[tbl.count].score = highScores[i].score;
        tbl.count++;
    }
    if (tbl.count == 0)
        return;
    static_assert(sizeof(tbl) <= sizeof(meshtastic_MeshPacket().decoded.payload.bytes), "TetrisTableWire too large");
    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = NODENUM_BROADCAST;
    p->channel = 0;
    memcpy(p->decoded.payload.bytes, &tbl, sizeof(tbl));
    p->decoded.payload.size = sizeof(tbl);
    service->sendToMesh(p);
    LOG_INFO("Tetris: broadcast table (%u entries)", tbl.count);
}

void TetrisModule::announceHighScore(uint32_t score, const char *name)
{
    if (!service)
        return;

#if GAME_DEMO_MODE
    char msg[64];
    const char *n = (name && name[0]) ? name : owner.short_name;
    snprintf(msg, sizeof(msg), "%s set a new Tetris high score: %lu", n, static_cast<unsigned long>(score));
    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = NODENUM_BROADCAST;
    p->channel = channels.getPrimaryIndex();
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p->want_ack = false;
    pb_size_t msgLen = static_cast<pb_size_t>(strnlen(msg, sizeof(msg) - 1));
    memcpy(p->decoded.payload.bytes, msg, msgLen);
    p->decoded.payload.size = msgLen;
    service->sendToMesh(p);
    LOG_INFO("Tetris Demo: broadcast text '%s'", msg);
#else
    TetrisScoreWire wire;
    wire.game_id = TETRIS_WIRE_GAME_ID;
    wire.version = TETRIS_WIRE_VERSION;
    const char *n = (name && name[0]) ? name : owner.short_name;
    strncpy(wire.shortName, n, sizeof(wire.shortName) - 1);
    wire.shortName[sizeof(wire.shortName) - 1] = '\0';
    wire.score = score;
    static_assert(sizeof(wire) <= sizeof(meshtastic_MeshPacket().decoded.payload.bytes), "TetrisScoreWire too large");
    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = NODENUM_BROADCAST;
    p->channel = 0;
    memcpy(p->decoded.payload.bytes, &wire, sizeof(wire));
    p->decoded.payload.size = sizeof(wire);
    service->sendToMesh(p);
    LOG_INFO("Tetris: broadcast score %lu", static_cast<unsigned long>(score));
#endif
}
#endif // TETRIS_ANNOUNCE_HIGH_SCORE

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

void TetrisModule::loadHighScores()
{
    highScoresLoaded = true;
    memset(highScores, 0, sizeof(highScores));
#ifdef FSCom
    concurrency::LockGuard g(spiLock);
    auto f = FSCom.open(TETRIS_HS_FILE, FILE_O_READ);
    if (!f)
        return;
    HighScoreFile file;
    const bool ok = (f.read(reinterpret_cast<uint8_t *>(&file), sizeof(file)) == sizeof(file));
    f.close();
    if (!ok || file.magic != HS_MAGIC || file.version != HS_VERSION) {
        LOG_DEBUG("Tetris: no valid high-score file");
        return;
    }
    if (crc32Buffer(&file, offsetof(HighScoreFile, crc)) != file.crc) {
        LOG_WARN("Tetris: high-score CRC mismatch, resetting");
        return;
    }
    memcpy(highScores, file.entries, sizeof(highScores));
    LOG_INFO("Tetris: loaded high scores (top=%lu)", static_cast<unsigned long>(highScores[0].score));
#endif
}

void TetrisModule::saveHighScores()
{
#ifdef FSCom
    {
        concurrency::LockGuard g(spiLock);
        FSCom.mkdir("/prefs");
    }
    HighScoreFile file;
    memset(&file, 0, sizeof(file));
    file.magic = HS_MAGIC;
    file.version = HS_VERSION;
    memcpy(file.entries, highScores, sizeof(highScores));
    file.crc = crc32Buffer(&file, offsetof(HighScoreFile, crc));
    auto sf = SafeFile(TETRIS_HS_FILE, true);
    const size_t written = sf.write(reinterpret_cast<uint8_t *>(&file), sizeof(file));
    if (!sf.close() || written != sizeof(file))
        LOG_WARN("Tetris: failed to save high scores");
#endif
}

#endif // HAS_SCREEN && BASEUI_HAS_GAMES
