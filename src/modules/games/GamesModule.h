#pragma once

#include "configuration.h"

#if HAS_SCREEN && BASEUI_HAS_GAMES

#include "Game.h"
#include "Observer.h"
#include "concurrency/OSThread.h"
#include "input/InputBroker.h"
#include "mesh/SinglePortModule.h"
#include <vector>

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
    void drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y);
    virtual bool interceptingKeyboardInput() override { return uiState != GAMES_IDLE; }

    /// Mesh passthrough for hosted games (a Game is not itself a MeshModule).
    meshtastic_MeshPacket *gameAllocDataPacket() { return allocDataPacket(); }

  protected:
    virtual int32_t runOnce() override;
    virtual Observable<const UIFrameEvent *> *getUIFrameObservable() override { return this; }
    virtual bool wantUIFrame() override { return false; }
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

  private:
    int handleInputEvent(const InputEvent *event);
    CallbackObserver<GamesModule, const InputEvent *> inputObserver =
        CallbackObserver<GamesModule, const InputEvent *>(this, &GamesModule::handleInputEvent);

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
};

extern GamesModule *gamesModule;

#endif // HAS_SCREEN && BASEUI_HAS_GAMES
