#pragma once

#include "configuration.h"

#if HAS_SCREEN && BASEUI_HAS_GAMES

#include "../TetrisGame.h"
#include "Game.h"
#include "GamesModule.h"
#include "HighScoreTable.h"

/**
 * Tetris as a hosted Game. Wraps the pure TetrisGame logic and supplies the attract art, the
 * portrait playfield renderer (with SCR/LVL left panel and NXT/HLD right panel), the
 * rotate/move/drop input, the level-based speed curve, a configurable lock-delay, a hold piece,
 * and its own high-score table.
 *
 * When GAMES_ANNOUNCE_HIGH_SCORE=1 it broadcasts scores using the GAME_APP protobuf protocol
 * (or a text message in GAME_DEMO_MODE).
 */
class Tetris : public Game
{
  public:
    Tetris();

    const char *name() const override { return "Tetris"; }

    void start(uint32_t seed) override;
    bool tick() override;
    bool isPlaying() const override { return game.isPlaying(); }
    uint32_t score() const override { return game.score(); }
    int32_t tickIntervalMs() const override;

    void handleInput(input_broker_event ev) override;

    void drawAttract(OLEDDisplay *display, int16_t x, int16_t y) override;
    void drawPlaying(OLEDDisplay *display, int16_t x, int16_t y) override;
    const char *gameOverHint() const override { return "SEL: scores  BCK: exit"; }

    HighScoreTableBase &scores() override { return scores_; }

    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

#if GAMES_ANNOUNCE_HIGH_SCORE
    bool wantsPeriodicMesh() const override { return true; }
    int32_t meshTick(GamesModule &host) override;
    void onAnnounceScore(GamesModule &host, const char *initials, uint32_t score) override;
#endif

  private:
    // On-disk high-score record. Version 2 adds scoreId for per-session dedup.
    // Magic 'TETR', file version 2.
    struct TetrisEntry {
        uint32_t score;
        char shortName[5];
        uint32_t nodeNum;
        uint32_t epoch;
        uint32_t scoreId;
    } __attribute__((packed));

    TetrisGame game;
    HighScoreTable<TetrisEntry> scores_{"/prefs/tetris.dat", 0x54455452u, 2, "Tetris"};

    // Lock-delay state: we wait LOCK_DELAY_MS after the piece lands before locking.
    static constexpr uint32_t LOCK_DELAY_MS = 500;
    bool lockDelayActive = false;
    uint32_t lockDelayStartMs = 0;

    // Deferred chirp: set in handleInput() for hard-drop line clears so it plays
    // on the next tick() rather than synchronously in the input handler.
    bool pendingLineClearChirp = false;

#if GAMES_ANNOUNCE_HIGH_SCORE
    static constexpr uint32_t BROADCAST_INITIAL_MS = 60000UL;
    static constexpr uint32_t BROADCAST_INTERVAL_MS = 12UL * 60 * 60 * 1000;
    uint32_t lastBroadcastMs = 0;

    int32_t nextBroadcastIntervalMs() const;
    void broadcastAllScores(GamesModule &host);
    void announceHighScore(GamesModule &host, uint32_t score, const char *name);
#endif

    void drawPlayfield(OLEDDisplay *display, int16_t x, int16_t y);
};

#endif // HAS_SCREEN && BASEUI_HAS_GAMES
