#include "TCA8418KeyboardBase.h"

// The T-SF32-Keyboard module's 4x5 pad: a phone dialpad plus a column of
// navigation keys. Letters are silkscreened on the number keys, so text is
// entered by multi-tap - the same scheme as MPR121Keyboard - with the round
// key acting as shift. Frames are navigated by touch, not from here.
class TDisplaySF32Keyboard : public TCA8418KeyboardBase
{
  public:
    TDisplaySF32Keyboard();
    void reset(void) override;

  protected:
    void pressed(uint8_t key) override;
    void released(void) override;

  private:
    uint8_t last_key;
    uint32_t last_tap;
    uint8_t char_idx;
    bool shift_pending;
};
