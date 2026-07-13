#pragma once

#include "configuration.h"

#if HAS_SCREEN && BASEUI_HAS_GAMES

#include "HighScoreTable.h"
#include "input/InputBroker.h" // input_broker_event
#include "mesh/MeshModule.h"   // ProcessMessage, meshtastic_MeshPacket
#include <cstdint>

#ifndef GAMES_ANNOUNCE_HIGH_SCORE
#define GAMES_ANNOUNCE_HIGH_SCORE 0
#endif

#ifndef GAME_DEMO_MODE
#define GAME_DEMO_MODE 0
#endif

class OLEDDisplay;
class OLEDDisplayUiState;
class GamesModule;

/**
 * A single game hosted by GamesModule. The host owns the shared UI state machine (attract /
 * playing / paused / game-over / high-scores), the initials picker, high-score persistence calls,
 * the tick thread, and the mesh port; each Game supplies only the game-specific pieces: its
 * attract art, its playfield, its per-key input while playing, its speed curve, and its own
 * high-score table (and, optionally, a mesh announce/receive protocol).
 */
class Game
{
  public:
    virtual ~Game() = default;

    virtual const char *name() const = 0;

    // --- Lifecycle ---
    virtual void start(uint32_t seed) = 0; // (re)start the underlying game logic
    virtual bool tick() = 0;               // advance one step; returns isPlaying() afterwards
    virtual bool isPlaying() const = 0;
    virtual uint32_t score() const = 0;
    virtual int32_t tickIntervalMs() const = 0; // per-game speed curve

    // --- Input while PLAYING (the host handles the BACK-to-pause and menu keys) ---
    virtual void handleInput(input_broker_event ev) = 0;

    // --- Rendering (the host draws the shared PAUSED / GAME OVER / HIGH SCORES chrome) ---
    virtual void drawAttract(OLEDDisplay *display, int16_t x, int16_t y) = 0; // title/art + hi + hint
    virtual void drawPlaying(OLEDDisplay *display, int16_t x, int16_t y) = 0; // playfield only
    virtual const char *gameOverHint() const { return "SELECT: scores"; }

    // --- High scores (the host runs the initials picker + save) ---
    virtual HighScoreTableBase &scores() = 0;

    virtual uint32_t gameType() const = 0;

    // --- Mesh ---
    // Called (when GAMES_ANNOUNCE_HIGH_SCORE=1, non-DEMO mode) to broadcast a new score via the wire protocol.
    virtual void onAnnounceScore(GamesModule &host, const char *initials, uint32_t score) {}
#if GAMES_ANNOUNCE_HIGH_SCORE
    // Unified broadcast and receive implementations provided by Game base (Game.cpp).
    virtual bool wantsPeriodicMesh() const final { return true; }
    virtual int32_t meshTick(GamesModule &host) final;
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) final;

  protected:
    // Initial delay before the first full-table broadcast (override to stagger games).
    virtual uint32_t broadcastInitialMs() const { return 60000UL; }

  private:
    static constexpr uint32_t BROADCAST_INTERVAL_MS = 12UL * 60 * 60 * 1000;
    uint32_t lastBroadcastMs_ = 0;
    int32_t nextBroadcastIntervalMs() const;
    void broadcastAllScores(GamesModule &host);
#else
    virtual bool wantsPeriodicMesh() const { return false; }
    virtual int32_t meshTick(GamesModule &host) { return -1; }
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) { return ProcessMessage::CONTINUE; }
#endif
};

#endif // HAS_SCREEN && BASEUI_HAS_GAMES
