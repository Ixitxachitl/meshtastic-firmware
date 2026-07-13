#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/**
 * Pure, self-contained Snake game logic.
 *
 * Deliberately free of Arduino / Screen / heap dependencies so it can be unit-tested natively
 * (see test/test_snake) and reused by SnakeModule without pulling in the display stack.
 *
 * The board is a fixed GRID_W x GRID_H grid of cells. The snake body lives in a ring buffer
 * sized to the whole board, plus an occupancy bitmap for O(1) collision and food-placement
 * checks. No dynamic allocation: total state is ~1 KB and statically sized.
 */
class SnakeGame
{
  public:
    // Playfield dimensions in cells - default chosen for OLED (128×64 with a 16 px score bar).
    // Call configure(w, h) before reset() to use a different grid size (e.g. to fill a larger
    // TFT display). MAX_CELLS sizes the static arrays; no heap allocation.
    static constexpr uint8_t GRID_W = 32;
    static constexpr uint8_t GRID_H = 12;
    static constexpr uint16_t CELL_COUNT = static_cast<uint16_t>(GRID_W) * GRID_H; // 384
    static constexpr uint16_t MAX_CELLS = 48 * 24;                                 // 1152 - ceiling for any supported display

    // Initial snake length at the start of a game.
    static constexpr uint8_t START_LEN = 3;

    /** Set the active grid size before calling reset(). Silently clamps to MAX_CELLS. */
    void configure(uint8_t w, uint8_t h);
    uint8_t gridW() const { return w_; }
    uint8_t gridH() const { return h_; }

    enum Direction : uint8_t { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };

    struct Cell {
        uint8_t x;
        uint8_t y;
    };

    /**
     * (Re)start a game. The snake spawns horizontally in the middle of the board heading right,
     * and the first food is placed. `seed` drives deterministic food placement (xorshift32).
     */
    void reset(uint32_t seed);

    /**
     * Latch a new heading to be applied on the next step(). A 180-degree reversal of the
     * currently-committed direction is rejected (returns false) because it would immediately
     * run the head into the neck. Comparing against the committed direction (not the pending
     * one) means multiple key presses within a single tick can't chain into a reversal.
     */
    bool setDirection(Direction d);

    /**
     * Advance the simulation by one tick. Returns true if the snake is still alive afterwards,
     * false if this move ended the game (wall hit, self-collision, or board filled == win).
     * Once dead, further step() calls are no-ops returning false.
     */
    bool step();

    bool isPlaying() const { return alive; }
    bool isWon() const { return won; }
    uint16_t length() const { return len; }
    uint32_t score() const { return points; }

    Cell head() const { return body[headIdx]; }
    Cell food() const { return foodCell; }
    Direction direction() const { return dir; }

    /// True if cell (x,y) is currently part of the snake body.
    bool occupied(uint8_t x, uint8_t y) const { return getOcc(cellIndex(x, y)); }

    /// Iterate the body from tail (i == 0) to head (i == length()-1); used by the renderer.
    Cell bodyAt(uint16_t i) const { return body[(tailIdx + i) % CAP]; }

    /**
     * Test/aid seam: force the next food to a specific cell so unit tests can drive
     * deterministic growth. Unused in production. Caller must pass an unoccupied cell.
     */
    void placeFoodAt(uint8_t x, uint8_t y) { foodCell = {x, y}; }

  private:
    static constexpr uint16_t CAP = MAX_CELLS;

    uint8_t w_ = GRID_W; // active grid width  (set by configure())
    uint8_t h_ = GRID_H; // active grid height (set by configure())
    uint16_t cellCount() const { return static_cast<uint16_t>(w_) * h_; }

    Cell body[CAP] = {};
    uint16_t headIdx = 0;
    uint16_t tailIdx = 0;
    uint16_t len = 0;

    uint8_t occ[(MAX_CELLS + 7) / 8] = {}; // occupancy bitmap, indexed by cellIndex()

    Cell foodCell = {0, 0};
    Direction dir = DIR_RIGHT;
    Direction pendingDir = DIR_RIGHT;
    uint32_t points = 0;
    uint32_t rng = 1;
    bool alive = false;
    bool won = false;

    uint16_t cellIndex(uint8_t x, uint8_t y) const { return static_cast<uint16_t>(y) * w_ + x; }
    bool getOcc(uint16_t idx) const { return (occ[idx >> 3] >> (idx & 7)) & 1u; }
    void setOcc(uint16_t idx) { occ[idx >> 3] |= static_cast<uint8_t>(1u << (idx & 7)); }
    void clearOcc(uint16_t idx) { occ[idx >> 3] &= static_cast<uint8_t>(~(1u << (idx & 7))); }

    uint32_t nextRandom();
    bool placeFood(); // returns false if the board is full (no free cell -> win)
    static bool isReverse(Direction a, Direction b);
};
