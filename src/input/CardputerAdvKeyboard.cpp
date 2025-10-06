#if defined(M5STACK_CARDPUTER_ADV)

#include "CardputerAdvKeyboard.h"
#include "main.h"
#include "configuration.h"        // for config.*
#include "MeshService.h"          // for service->reloadConfig
#include "graphics/draw/MessageRenderer.h" // scrollUp/Down()
#include "graphics/SharedUIDisplay.h"      // isMessagesScreenActive()
#include "buzz/buzz.h"

#define _TCA8418_COLS 8
#define _TCA8418_ROWS 7
#define _TCA8418_NUM_KEYS 56

#define _TCA8418_MULTI_TAP_THRESHOLD 1500

using Key = TCA8418KeyboardBase::TCA8418Key;

constexpr uint8_t modifierShiftKey = 7 - 1; // keynum -1
constexpr uint8_t modifierRightShift = 0b0001;

constexpr uint8_t modifierFnKey = 3 - 1;
constexpr uint8_t modifierFn = 0b0010;

constexpr uint8_t modifierCtrlKey = 4 - 1;
constexpr uint8_t modifierOptKey  = 8 - 1;
constexpr uint8_t modifierAltKey  = 12 - 1;


// Num chars per key, Modulus for rotating through characters
// https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1178/Sch_M5CardputerAdv_v1.0_2025_06_20_17_19_58_page_02.png
static uint8_t CardputerAdvTapMod[_TCA8418_NUM_KEYS] = {3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
                                                        3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
                                                        3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};

static unsigned char CardputerAdvTapMap[_TCA8418_NUM_KEYS][3] = {{'`', '~', Key::ESC},
                                                                 {Key::TAB, 0x00, 0x00},
                                                                 {0x00, 0x00, 0x00}, // Fn
                                                                 {0x00, 0x00, 0x00}, // ctrl
                                                                 {'1', '!', 0x00},
                                                                 {'q', 'Q', Key::REBOOT},
                                                                 {0x00, 0x00, 0x00}, // shift
                                                                 {0x00, 0x00, 0x00}, // opt
                                                                 {'2', '@', 0x00},
                                                                 {'w', 'W', 0x00},
                                                                 {'a', 'A', 0x00},
                                                                 {0x00, 0x00, 0x00}, // alt
                                                                 {'3', '#', 0x00},
                                                                 {'e', 'E', 0x00},
                                                                 {'s', 'S', 0x00},
                                                                 {'z', 'Z', 0x00},
                                                                 {'4', '$', 0x00},
                                                                 {'r', 'R', 0x00},
                                                                 {'d', 'D', 0x00},
                                                                 {'x', 'X', 0x00},
                                                                 {'5', '%', 0x00},
                                                                 {'t', 'T', 0x00},
                                                                 {'f', 'F', 0x00},
                                                                 {'c', 'C', 0x00},
                                                                 {'6', '^', 0x00},
                                                                 {'y', 'Y', 0x00},
                                                                 {'g', 'G', Key::GPS_TOGGLE},
                                                                 {'v', 'V', 0x00},
                                                                 {'7', '&', 0x00},
                                                                 {'u', 'U', 0x00},
                                                                 {'h', 'H', 0x00},
                                                                 {'b', 'B', Key::BT_TOGGLE},
                                                                 {'8', '*', 0x00},
                                                                 {'i', 'I', 0x00},
                                                                 {'j', 'J', 0x00},
                                                                 {'n', 'N', 0x00},
                                                                 {'9', '(', 0x00},
                                                                 {'o', 'O', 0x00},
                                                                 {'k', 'K', 0x00},
                                                                 {'m', 'M', Key::MUTE_TOGGLE},
                                                                 {'0', ')', 0x00},
                                                                 {'p', 'P', Key::SEND_PING},
                                                                 {'l', 'L', 0x00},
                                                                 {',', '<', Key::LEFT},
                                                                 {'_', '-', 0x00},
                                                                 {'[', '{', 0x00},
                                                                 {';', ':', Key::UP},
                                                                 {'.', '>', Key::DOWN},
                                                                 {'=', '+', 0x00},
                                                                 {']', '}', 0x00},
                                                                 {'\'', '"', 0x00},
                                                                 {'/', '?', Key::RIGHT},
                                                                 {Key::BSP, 0x00, 0x00},
                                                                 {'\\', '|', 0x00},
                                                                 {Key::SELECT, 0x00, 0x00}, // Enter
                                                                 {' ', ' ', ' '}};          // Space

CardputerAdvKeyboard::CardputerAdvKeyboard()
    : TCA8418KeyboardBase(_TCA8418_ROWS, _TCA8418_COLS),
      last_key(-1),
      next_key(-1),
      last_tap(0L),
      char_idx(0),
      tap_interval(0)
{
    reset();
}

void CardputerAdvKeyboard::reset(void)
{
    TCA8418KeyboardBase::reset();
    shiftHeld = false;
    fnHeld = false;
    last_key = -1;
    _released_key_raw = 0;
    _released_key_idx = -1;
    _repeatStartMs = _repeatNextMs = 0;
    _handledNavOnPress = false;
}

// handle multi-key presses (shift and alt) + run auto-repeat while held
void CardputerAdvKeyboard::trigger()
{
    uint8_t count = keyCount();

    // Drain hardware key FIFO as usual
    if (count) {
        for (uint8_t i = 0; i < count; ++i) {
            uint8_t k = readRegister(TCA8418_REG_KEY_EVENT_A + i);
            uint8_t key = k & 0x7F; // raw TCA8418 key number (1..)
            if (k & 0x80) {
                // PRESS
                pressed(key);
            } else {
                // RELEASE
                int row = (key - 1) / 10;
                int col = (key - 1) % 10;
                if (row >= _TCA8418_ROWS || col >= _TCA8418_COLS) continue;
                int idx = row * _TCA8418_COLS + col;

                // if it's a modifier, clear it now and skip released()
                if (isModifierKey(idx)) {
                    setModifierOff(idx);
                    state = Idle;        // nothing to emit for modifier release
                    continue;
                }

                // Non-modifier release -> go through normal release handling
                _released_key_raw = key;
                _released_key_idx = idx;
                released();              // emit based on current held mods
                state = Idle;

                // clear carry + stop repeating
                _released_key_raw = 0;
                _released_key_idx = -1;
                _repeatStartMs = _repeatNextMs = 0;
            }
        }
    }

    // Even if no new events, generate repeats if a repeatable key is held
    maybeAutoRepeat();
}

void CardputerAdvKeyboard::pressed(uint8_t key)
{
    if (state == Init || state == Busy) return;

    int row = (key - 1) / 10;
    int col = (key - 1) % 10;
    if (row >= _TCA8418_ROWS || col >= _TCA8418_COLS) return;

    uint8_t next_key = row * _TCA8418_COLS + col;

    // Set mods while held
    setModifierOn(next_key);

    // don't treat modifiers as the "last_key" for tap/hold logic
    if (isModifierKey(next_key)) {
        state = Held;   // still mark as held so subsequent key sees held mod
        return;
    }

    if (handleFnShortcutOnPress(next_key)) {
        // We intentionally do NOT set last_key nor arm repeat.
        // Leave state as Held so other keys can continue to interact with fn.
        state = Held;
        return;
    }

    // NAV KEYS: scroll on PRESS, immediately
    unsigned char out = resolveOutput(next_key);
    if (out == Key::UP || out == Key::DOWN) {
        if (graphics::isMessagesScreenActive() && !graphics::isOverlayActive()) {
            if (out == Key::UP)   graphics::MessageRenderer::scrollUp();
            if (out == Key::DOWN) graphics::MessageRenderer::scrollDown();
        }
        last_key = next_key;
        uint32_t now = millis();
        _repeatIsNav   = graphics::isMessagesScreenActive();
        _repeatStartMs = now + (_repeatIsNav ? _repeatInitialDelayMsNav : _repeatInitialDelayMs);  // 150 ms
        _repeatNextMs  = _repeatStartMs;
        _handledNavOnPress = true;
        state = Held;
        return;
    }

    state = Held;

    uint32_t now = millis();
    tap_interval = now - last_tap;

    if (tap_interval < 0) { last_tap = 0; state = Busy; return; }

    if (next_key != last_key || tap_interval > _TCA8418_MULTI_TAP_THRESHOLD) {
        char_idx = 0;
    } else {
        char_idx += 1;
    }

    last_key = next_key;
    last_tap = now;

    // (Re)arm auto-repeat if this key supports it
    if (keyIsRepeatable(last_key)) {
        _repeatStartMs = now + _repeatInitialDelayMs;
        _repeatNextMs  = _repeatStartMs;
    } else {
        _repeatStartMs = _repeatNextMs = 0;
    }
}

void CardputerAdvKeyboard::released()
{
    // If we somehow got here without a carried key index, fall back safely.
    const int key_idx = (_released_key_idx >= 0) ? _released_key_idx : last_key;

    // --- If the released key is NOT the active composing key, do nothing. ---
    // This prevents the bug where: hold 'd' -> press 'f' (types 'f') -> release 'd' (would wrongly type 'f' again).
    if (_released_key_idx >= 0 && _released_key_idx != last_key) {
        // We only clear repeat timers if this released key was the one arming repeat (it isn't, by definition),
        // so nothing else to do. Keep state/last_key as-is (another key may still be held).
        return;
    }

    if (key_idx < 0 || key_idx >= _TCA8418_NUM_KEYS) {
        last_key = -1;
        state = Idle;
        return;
    }

    uint32_t now = millis();
    last_tap = now;

    // choose output by current held-mod index, with nav<->fn swap rule preserved
    unsigned char out = resolveOutput(key_idx);
    if (out == 0x00 || out == Key::BL_TOGGLE) {
        // No output for unmapped/disabled keys
        // Clear timers if this was the active one
        if (key_idx == last_key) { _repeatStartMs = _repeatNextMs = 0; }
        // If we just released the active key, mark idle
        if (key_idx == last_key) { last_key = -1; state = Idle; }
        return;
    }

    if ((graphics::isMessagesScreenActive() && !graphics::isOverlayActive()) && (out == Key::UP || out == Key::DOWN)) return;

    // Not on messages screen (or not an arrow): emit the key normally
    queueEvent(out);

    // If we just released the active key, stop repeating & go idle.
    if (key_idx == last_key) {
        _repeatStartMs = _repeatNextMs = 0;
        last_key = -1;
        state = Idle;
    }
}

// Returns true if we handled a FN shortcut on key press and consumed the event.
bool CardputerAdvKeyboard::handleFnShortcutOnPress(uint8_t key_idx) {
    if (!fnHeld) return false;
    if (key_idx >= _TCA8418_NUM_KEYS) return false;

    unsigned char base = CardputerAdvTapMap[key_idx][0];

    // fn + s  → toggle wake_on_tap_or_motion
    if (base == 's' || base == 'S') {
        config.display.wake_on_tap_or_motion = !config.display.wake_on_tap_or_motion;
        service->reloadConfig(SEGMENT_CONFIG);
        // play the same sounds as the GPS toggle
        if (config.display.wake_on_tap_or_motion) {
           playGPSEnableBeep();
        } else {
           playGPSDisableBeep();
        }
        IF_SCREEN(screen->showSimpleBanner(
            config.display.wake_on_tap_or_motion ?
            "Wake on Tap/Motion: ON" : "Wake on Tap/Motion: OFF", 1000));
        return true;  // consumed
    }

    // fn + c  → calibrate compass (30s)
    if (base == 'c' || base == 'C') {
        if (accelerometerThread) { accelerometerThread->calibrate(30); }
        return true;  // consumed
    }

    return false;
}

void CardputerAdvKeyboard::setModifierOn(uint8_t key) {
    if (key == modifierShiftKey) shiftHeld = true;
    else if (key == modifierFnKey) fnHeld = true;
}

void CardputerAdvKeyboard::setModifierOff(uint8_t key) {
    if (key == modifierShiftKey) shiftHeld = false;
    else if (key == modifierFnKey) fnHeld = false;
}

bool CardputerAdvKeyboard::isModifierKey(uint8_t key) {
    return (key == modifierShiftKey || key == modifierFnKey);
}

// Kept for API compatibility; no longer used for toggling
void CardputerAdvKeyboard::updateModifierFlag(uint8_t) { /* no-op */ }

bool CardputerAdvKeyboard::isNavOrEscKey(uint8_t key) const {
    if (key >= _TCA8418_NUM_KEYS) return false;
    unsigned char alt = CardputerAdvTapMap[key][2];
    return (alt == Key::LEFT || alt == Key::RIGHT || alt == Key::UP ||
            alt == Key::DOWN || alt == Key::ESC);
}

// --- helpers ---
unsigned char CardputerAdvKeyboard::resolveOutput(uint8_t key) const {
    uint8_t idx = modIndex();  // 0=base, 1=shift, 2=fn (based on *current* held flags)

    // Reverse base<->fn for keys whose 3rd entry is nav/ESC
    if (isNavOrEscKey(key)) {
        if (idx == 0)      idx = 2;
        else if (idx == 2) idx = 0;
    }

    uint8_t modCount = CardputerAdvTapMod[key];
    if (idx >= modCount) idx = 0;

    return CardputerAdvTapMap[key][idx];
}

bool CardputerAdvKeyboard::keyIsRepeatable(uint8_t key) const {
    if (key >= _TCA8418_NUM_KEYS) return false;
    unsigned char base = CardputerAdvTapMap[key][0];
    unsigned char alt  = CardputerAdvTapMap[key][2];
    // Repeat printable ASCII & BSP, and also NAV keys (UP/DOWN/LEFT/RIGHT)
    bool printable = (base == Key::BSP) || (base >= 0x20 && base <= 0x7E);
    bool isNav     = (alt == Key::UP || alt == Key::DOWN || alt == Key::LEFT || alt == Key::RIGHT);
    return printable || isNav;
}

void CardputerAdvKeyboard::maybeAutoRepeat() {
    if (state != Held) return;
    if (last_key < 0 || last_key >= _TCA8418_NUM_KEYS) return;
    if (!keyIsRepeatable(last_key)) return;
    uint32_t now = millis();
    if (_repeatStartMs == 0) return;

    if (now >= _repeatNextMs) {
        unsigned char out = resolveOutput(last_key);
        if (graphics::isMessagesScreenActive() && !graphics::isOverlayActive()) {
            if (out == Key::UP) {
                graphics::MessageRenderer::scrollUp();
            } else if (out == Key::DOWN) {
                graphics::MessageRenderer::scrollDown();
            } else if (out != 0x00 && out != Key::BL_TOGGLE) {
                // normal repeat behavior for other keys
                queueEvent(out);
            }
        } else if (out != 0x00 && out != Key::BL_TOGGLE) {
            // not in messages: always emit repeats
            queueEvent(out);
        }
        _repeatNextMs = now + (_repeatIsNav ? _repeatRateMsNav : _repeatRateMs);
    }
}

#endif