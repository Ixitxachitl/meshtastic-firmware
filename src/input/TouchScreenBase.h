#pragma once

#include "InputBroker.h"
#include "concurrency/OSThread.h"
#include "mesh/NodeDB.h"
#include "time.h"

typedef struct _TouchEvent {
    const char *source;
    char touchEvent;
    uint16_t x;
    uint16_t y;
} TouchEvent;

class TouchScreenBase : public Observable<const InputEvent *>, public concurrency::OSThread
{
  public:
    explicit TouchScreenBase(const char *name, uint16_t width, uint16_t height);
    void init(bool hasTouch);

  protected:
    enum TouchScreenBaseStateType { TOUCH_EVENT_OCCURRED, TOUCH_EVENT_CLEARED };

    enum TouchScreenBaseEventType {
        TOUCH_ACTION_NONE,
        TOUCH_ACTION_UP,
        TOUCH_ACTION_DOWN,
        TOUCH_ACTION_LEFT,
        TOUCH_ACTION_RIGHT,
        TOUCH_ACTION_TAP,
        TOUCH_ACTION_LONG_PRESS,
        TOUCH_ACTION_DRAG,    // finger held down and moving - repeats, carries the current point
        TOUCH_ACTION_DRAG_END // finger lifted after a drag
    };

    virtual int32_t runOnce() override;

    virtual bool getTouch(int16_t &x, int16_t &y) = 0;
    virtual void onEvent(const TouchEvent &event) = 0;
    virtual bool fastTapModeEnabled() const;
    virtual bool longPressEnabled() const;
    // Whether to report TOUCH_ACTION_DRAG while the finger moves. Off in the base class so touch
    // panels that can't usefully redraw mid-gesture keep the release-only swipe behaviour.
    virtual bool dragEventsEnabled() const;

    volatile TouchScreenBaseStateType _state = TOUCH_EVENT_CLEARED;
    volatile TouchScreenBaseEventType _action = TOUCH_ACTION_NONE;

  protected:
    uint16_t _display_width;
    uint16_t _display_height;

  private:
    bool _touchedOld = false;  // previous touch state
    int16_t _first_x, _last_x; // horizontal swipe direction
    int16_t _first_y, _last_y; // vertical swipe direction
    time_t _start;             // for LONG_PRESS
    uint32_t _lastTouchSeenMs; // helps suppress brief touch-controller dropouts
    bool _tapped;              // for DOUBLE_TAP
    uint32_t _lastRun = 0;     // helps suppress too fast consecutive runOnce() executions

    bool _dragging = false; // a drag has been reported for the gesture currently in progress
    int16_t _drag_x = 0;    // last point reported as TOUCH_ACTION_DRAG, for the step threshold
    int16_t _drag_y = 0;

    const char *_originName;
};
