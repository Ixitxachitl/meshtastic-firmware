#include "TCA8418KeyboardBase.h"

class CardputerAdvKeyboard : public TCA8418KeyboardBase
{
  public:
    CardputerAdvKeyboard();
    void reset(void);
    void trigger(void) override;
    virtual ~CardputerAdvKeyboard() {}

  protected:
    void pressed(uint8_t key) override;
    void released(void) override; // NOTE: unchanged signature (base class compat)

    // Kept for compatibility (no-op now unless you reuse it)
    void updateModifierFlag(uint8_t key);
    bool isModifierKey(uint8_t key);

  private:
    // --- TRUE held-state for modifiers ---
    bool shiftHeld = false;
    bool fnHeld    = false;

    // --- Helper selection & classification ---
    inline uint8_t modIndex() const {
      // 0 = base, 1 = shift, 2 = fn
      if (shiftHeld) return 1;
      if (fnHeld)    return 2;
      return 0;
    }
    bool isNavOrEscKey(uint8_t key) const; // checks if TapMap[key][2] is nav or ESC
    void setModifierOn(uint8_t key);
    void setModifierOff(uint8_t key);
    bool handleFnShortcutOnPress(uint8_t key_idx);

    // --- Auto-repeat support ---
    bool keyIsRepeatable(uint8_t key) const;
    void maybeAutoRepeat();
    unsigned char resolveOutput(uint8_t key) const;

    // --- Existing fields kept ---
    int8_t  last_key;
    int8_t  next_key;
    uint32_t last_tap;
    uint8_t  char_idx;
    int32_t  tap_interval;

    // carry which physical key was just released into released()
    uint8_t _released_key_raw = 0;   // raw key num from TCA8418 (1..70)
    int8_t  _released_key_idx = -1;  // matrix -> [0.._TCA8418_NUM_KEYS-1]

    // auto-repeat timers
    uint32_t _repeatStartMs = 0;
    uint32_t _repeatNextMs  = 0;
    static constexpr uint16_t _repeatInitialDelayMs = 400; // first delay
    static constexpr uint16_t _repeatRateMs         = 45;  // between repeats
};
