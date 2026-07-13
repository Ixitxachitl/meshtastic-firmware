#pragma once

#include "configuration.h"

#if HAS_SCREEN && BASEUI_HAS_GAMES

#include "../SnakeGame.h"
#include "Game.h"
#include "GamesModule.h"
#include "HighScoreTable.h"

/**
 * Snake as a hosted Game. Wraps the pure SnakeGame logic and supplies the attract art, the
 * playfield renderer, the direction input, the length-based speed curve, and its own high-score
 * table. When GAMES_ANNOUNCE_HIGH_SCORE=1 it broadcasts scores using the GAME_APP protobuf
 * protocol (or a text message in GAME_DEMO_MODE).
 */
class Snake : public Game
{
  public:
    Snake();

    const char *name() const override { return "Snake"; }

    void start(uint32_t seed) override { game.reset(seed); }
    bool tick() override { return game.step(); }
    bool isPlaying() const override { return game.isPlaying(); }
    uint32_t score() const override { return game.score(); }
    int32_t tickIntervalMs() const override;

    void handleInput(input_broker_event ev) override;

    void drawAttract(OLEDDisplay *display, int16_t x, int16_t y) override;
    void drawPlaying(OLEDDisplay *display, int16_t x, int16_t y) override;
    const char *gameOverHint() const override { return "SEL: scores  BCK: exit"; }

    HighScoreTableBase &scores() override { return scores_; }

    uint32_t gameType() const override;

#if GAMES_ANNOUNCE_HIGH_SCORE
    void onAnnounceScore(GamesModule &host, const char *initials, uint32_t score) override;
#endif

  private:
    // On-disk high-score record. Version 2 adds scoreId for per-session dedup.
    // Magic 'SNEK', file version 2.
    struct SnakeEntry {
        uint32_t score;
        uint32_t nodeNum;
        char shortName[5];
        uint32_t epoch;
        uint32_t scoreId;
    } __attribute__((packed));

    SnakeGame game;
    HighScoreTable<SnakeEntry> scores_{"/prefs/snake.dat", 0x534E454Bu, 2, "Snake"};

    // Configure game.w_/h_ to fill the given display, choosing cellPx so the grid tiles the screen.
    void setupGrid(OLEDDisplay *display);

#if GAMES_ANNOUNCE_HIGH_SCORE
    void announceHighScore(GamesModule &host, uint32_t score, const char *name);
#endif
};

#endif // HAS_SCREEN && BASEUI_HAS_GAMES
