#pragma once

#include "configuration.h"

#if HAS_SCREEN && BASEUI_HAS_GAMES

#include "Game.h"
#include "Observer.h"
#include "concurrency/OSThread.h"
#include "input/InputBroker.h"
#include "mesh/SinglePortModule.h"
#include <vector>

// Broadcasting a new all-time #1 to the mesh is a COMPILE-TIME option, default OFF. Spending shared
// airtime must be opted into at build time with -DGAMES_ANNOUNCE_HIGH_SCORE=1; when disabled (the
// default) the announcement code is compiled out entirely -- there is no runtime toggle. The
// announcement is shared by every hosted game, with the game's name() spliced into the message.
#ifndef GAMES_ANNOUNCE_HIGH_SCORE
#define GAMES_ANNOUNCE_HIGH_SCORE 0
#endif

#ifndef GAMES_HIGH_SCORE_STRING
#define GAMES_HIGH_SCORE_STRING "New %s high score %lu by %s!"
#endif

enum GamesUiState : uint8_t {
    GAMES_IDLE,     // attract screen of the selected game; OSThread idle (unless a game broadcasts)
    GAMES_PLAYING,  // active game running; tick thread ticking
    GAMES_PAUSED,   // paused mid-game
    GAMES_GAMEOVER, // final score / new-high notice
    GAMES_HISCORES, // top-5 table of the active/selected game
};

/**
 * The single host for all BaseUI games. It owns the always-present "games" frame (drawn through
 * Screen's trampoline right after home), the shared UI state machine, the game-tick OSThread, the
 * initials picker + high-score persistence flow, and the GAME_APP mesh port. Individual games
 * are self-contained Game subclasses registered in the constructor (see src/modules/games/); the
 * attract screen cycles between them with UP/DOWN and SELECT plays the shown game.
 */
class GamesModule : public SinglePortModule, public Observable<const UIFrameEvent *>, private concurrency::OSThread
{
  public:
    GamesModule();

    /// Start the currently-selected game (invoked when SELECT is pressed on the games frame).
    void launchGame();

    // Drawn through the games-frame trampoline, and queried by Screen's input gating / nav-bar.
    // Drawn through the games-frame trampoline, and queried by Screen's input gating / nav-bar, so
    // these are public. While a game is active we own the D-pad; on the attract screen the D-pad
    // cycles games (UP/DOWN) and otherwise navigates between frames as usual.
    void drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y);
    virtual bool interceptingKeyboardInput() override { return uiState != GAMES_IDLE; }

    /// Mesh passthrough for hosted games (a Game is not itself a MeshModule).
    meshtastic_MeshPacket *gameAllocDataPacket() { return allocDataPacket(); }

  protected:
    virtual int32_t runOnce() override; // game tick + idle mesh scheduling
    virtual Observable<const UIFrameEvent *> *getUIFrameObservable() override { return this; }
    virtual bool wantUIFrame() override { return false; } // shares the games frame; no own slot
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

  private:
    int handleInputEvent(const InputEvent *event);
    CallbackObserver<GamesModule, const InputEvent *> inputObserver =
        CallbackObserver<GamesModule, const InputEvent *>(this, &GamesModule::handleInputEvent);

    // === State transitions ===
    void startPlaying();
    void enterGameOver();
    void exitToIdle();
    void requestRedraw();
    void kickTick();

    void promptForInitials();
    void recordHighScore(const char *initials);
#if GAMES_ANNOUNCE_HIGH_SCORE
    void announceHighScore(const char *initials, uint32_t score);
#endif

    // === Shared rendering ===
    void drawCenteredLines(OLEDDisplay *display, int16_t x, int16_t y, const char *const *lines, uint8_t count);
    void drawHighScores(OLEDDisplay *display, int16_t x, int16_t y, HighScoreTableBase &scores);

    std::vector<Game *> games;
    uint8_t selected = 0;
    Game *active = nullptr;
    GamesUiState uiState = GAMES_IDLE;
    uint32_t lastScore = 0;
    int lastRank = -1;
    bool lastWasNewTop = false;
    uint32_t lastAwakeKickMs = 0;

    // attract-screen cursor (index into games)
    // game currently playing / whose scores are shown; null when idle
    // score of the just-finished game (for the GAME OVER screen)
    // rank achieved last game (-1 == didn't place)
    // last game set a new all-time #1
    // throttles the power-FSM wake nudge during long runs
};

extern GamesModule *gamesModule;

#endif // HAS_SCREEN && BASEUI_HAS_GAMES
