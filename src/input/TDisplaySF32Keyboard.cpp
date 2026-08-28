#include "TDisplaySF32Keyboard.h"
#include "configuration.h"
#include <ctype.h>

using Key = TCA8418KeyboardBase::TCA8418Key;

#define _TCA8418_ROWS 5
#define _TCA8418_COLS 4
#define _TCA8418_NUM_KEYS 20

#define MULTI_TAP_THRESHOLD 2000

// The pad as it sits, top row first:
//
//   UP    ESC   HOME  MAIL
//   SHIFT 1     2     3
//   DOWN  4     5     6
//   DEL   7     8     9
//   ENTER *     0     #
//
// The TCA8418 numbers rows from the bottom, so row 0 is the *,0,# row and the
// key index below is row * 4 + col.
#define TAP_KEYS 13
#define KEY_INDEX_SHIFT 12

// How many characters each key cycles through.
static const uint8_t TapMod[_TCA8418_NUM_KEYS] = {1, 1,  2, 1,  // row 0: ENTER  *  0  #
                                                  1, 5,  4, 5,  // row 1: DEL    7  8  9
                                                  1, 4,  4, 4,  // row 2: DOWN   4  5  6
                                                  1, 13, 4, 4,  // row 3: SHIFT  1  2  3
                                                  1, 1,  1, 1}; // row 4: UP     ESC HOME MAIL

// Digit first, so the number pad still types numbers on a single press.
// Uppercase comes from the shift key rather than from a longer cycle.
static const unsigned char TapMap[_TCA8418_NUM_KEYS][TAP_KEYS] = {
    {Key::SELECT},
    {'*'},
    {'0', ' '},
    {'#'},
    {Key::BSP},
    {'7', 'p', 'q', 'r', 's'},
    {'8', 't', 'u', 'v'},
    {'9', 'w', 'x', 'y', 'z'},
    {Key::DOWN},
    {'4', 'g', 'h', 'i'},
    {'5', 'j', 'k', 'l'},
    {'6', 'm', 'n', 'o'},
    {Key::NONE}, // shift, handled in released()
    {'1', '.', ',', '?', '!', ':', ';', '-', '_', '\\', '/', '(', ')'},
    {'2', 'a', 'b', 'c'},
    {'3', 'd', 'e', 'f'},
    {Key::UP},
    {Key::ESC},
    {Key::FUNCTION_F1},
    {Key::FUNCTION_F2}};

TDisplaySF32Keyboard::TDisplaySF32Keyboard()
    : TCA8418KeyboardBase(_TCA8418_ROWS, _TCA8418_COLS), last_key(UINT8_MAX), last_tap(0), char_idx(0), shift_pending(false)
{
}

void TDisplaySF32Keyboard::reset(void)
{
    TCA8418KeyboardBase::reset();
    last_key = UINT8_MAX;
    last_tap = 0;
    char_idx = 0;
    shift_pending = false;
}

void TDisplaySF32Keyboard::pressed(uint8_t key)
{
    if (state == Init || state == Busy) {
        return;
    }

    int row = (key - 1) / 10;
    int col = (key - 1) % 10;
    if (row >= _TCA8418_ROWS || col >= _TCA8418_COLS) {
        return;
    }
    uint8_t next_key = row * _TCA8418_COLS + col;

    uint32_t now = millis();
    int32_t tap_interval = now - last_tap;
    if (tap_interval < 0) {
        // millis() has wrapped; drop this press rather than mis-time it.
        last_tap = 0;
        state = Busy;
        return;
    }

    // A different key, or too long a gap, commits whatever was pending and
    // starts the new key at its first character.
    if (next_key != last_key || tap_interval > MULTI_TAP_THRESHOLD) {
        char_idx = 0;
        // Shift survives only until the character it was armed for is typed,
        // so it has to outlive the shift key press itself. Cycling within one
        // key keeps it too, otherwise 2,2 would give "Ab".
        if (last_key != KEY_INDEX_SHIFT) {
            shift_pending = false;
        }
    } else {
        char_idx += 1;
    }

    last_key = next_key;
    last_tap = now;
    state = Held;
}

void TDisplaySF32Keyboard::released(void)
{
    if (state != Held) {
        return;
    }
    if (last_key >= _TCA8418_NUM_KEYS) {
        last_key = UINT8_MAX;
        state = Idle;
        return;
    }

    // Shift emits nothing of its own; pressing it again cancels it.
    if (last_key == KEY_INDEX_SHIFT) {
        shift_pending = !shift_pending;
        return;
    }

    // Cycling within one key replaces the character already shown, so back it
    // out first. Single-character keys never do this.
    if (char_idx > 0 && TapMod[last_key] > 1) {
        queueEvent(Key::BSP);
    }

    unsigned char c = TapMap[last_key][char_idx % TapMod[last_key]];
    if (shift_pending && isalpha(c)) {
        c = toupper(c);
    }
    queueEvent(c);
}
