#include "TouchScreenBase.h"
#include "main.h"

#if defined(RAK14014) && !defined(MESHTASTIC_EXCLUDE_CANNEDMESSAGES)
#include "modules/CannedMessageModule.h"
#endif

#ifndef TIME_LONG_PRESS
#define TIME_LONG_PRESS 400
#endif

// Touch sampling cadence (milliseconds).
// Can be overridden by board variants for faster touch panels.
#ifndef TOUCH_POLL_INTERVAL_IDLE
#define TOUCH_POLL_INTERVAL_IDLE 100
#endif

#ifndef TOUCH_POLL_INTERVAL_ACTIVE
#define TOUCH_POLL_INTERVAL_ACTIVE 20
#endif

#ifndef TOUCH_POLL_INTERVAL_RELEASE
#define TOUCH_POLL_INTERVAL_RELEASE 50
#endif

// Faster cadence used for keyboard-like tap-heavy UIs.
#ifndef TOUCH_POLL_INTERVAL_ACTIVE_FAST
#define TOUCH_POLL_INTERVAL_ACTIVE_FAST TOUCH_POLL_INTERVAL_ACTIVE
#endif

#ifndef TOUCH_POLL_INTERVAL_RELEASE_FAST
#define TOUCH_POLL_INTERVAL_RELEASE_FAST TOUCH_POLL_INTERVAL_RELEASE
#endif

// Ignore very short "finger lifted" glitches from noisy touch controllers.
// A release is only accepted once we've seen no-touch for at least this duration.
#ifndef TOUCH_RELEASE_GRACE_MS
#define TOUCH_RELEASE_GRACE_MS 35
#endif

// move a minimum distance over the screen to detect a "swipe"
#ifndef TOUCH_THRESHOLD_X
#define TOUCH_THRESHOLD_X 30
#endif

#ifndef TOUCH_THRESHOLD_Y
#define TOUCH_THRESHOLD_Y 20
#endif

// Movement (in pixels, from the touch-down point) before a held finger is treated as a drag rather
// than a tap still in progress. Deliberately below TOUCH_THRESHOLD_X/Y: once we're dragging, the
// release is reported as TOUCH_ACTION_DRAG_END instead of being classified as a swipe, so this
// threshold is what decides which of the two gesture models a given touch belongs to.
#ifndef TOUCH_DRAG_START_THRESHOLD
#define TOUCH_DRAG_START_THRESHOLD 8
#endif

// Minimum movement between consecutive TOUCH_ACTION_DRAG reports. Keeps a resting finger on a noisy
// panel from emitting a drag event every poll; the consumer still gets absolute positions, so a
// coarser step here costs tracking smoothness, not accuracy.
#ifndef TOUCH_DRAG_MIN_STEP
#define TOUCH_DRAG_MIN_STEP 2
#endif

TouchScreenBase::TouchScreenBase(const char *name, uint16_t width, uint16_t height)
    : concurrency::OSThread(name), _display_width(width), _display_height(height), _first_x(0), _last_x(0), _first_y(0),
      _last_y(0), _start(0), _lastTouchSeenMs(0), _tapped(false), _originName(name)
{
}

void TouchScreenBase::init(bool hasTouch)
{
    if (hasTouch) {
        LOG_INFO("TouchScreen initialized %d %d", TOUCH_THRESHOLD_X, TOUCH_THRESHOLD_Y);
        this->setInterval(TOUCH_POLL_INTERVAL_IDLE);
    } else {
        disable();
        this->setInterval(UINT_MAX);
    }
}

int32_t TouchScreenBase::runOnce()
{
    uint32_t nowMs = millis();
    if (nowMs - _lastRun < 20) { // suppress too fast consecutive runOnce() executions
        return 20;
    }
    _lastRun = nowMs;
    TouchEvent e;
    e.touchEvent = static_cast<char>(TOUCH_ACTION_NONE);
    this->setInterval(TOUCH_POLL_INTERVAL_IDLE);
    const bool fastTapMode = fastTapModeEnabled();
    const bool allowLongPress = longPressEnabled();

    // process touch events
    int16_t x, y;
    bool touched = getTouch(x, y);
    if (x < 0 || y < 0) // T-deck can emit phantom touch events with a negative value when turning off the screen
        touched = false;
    if (touched) {
        _lastTouchSeenMs = millis();
        this->setInterval(fastTapMode ? TOUCH_POLL_INTERVAL_ACTIVE_FAST : TOUCH_POLL_INTERVAL_ACTIVE);
        _last_x = x;
        _last_y = y;
    } else if (_touchedOld && ((uint32_t)millis() - _lastTouchSeenMs) < TOUCH_RELEASE_GRACE_MS) {
        // Treat brief no-touch samples as continuous touch to preserve long-press detection.
        touched = true;
    }
    if (touched != _touchedOld) {
        if (touched) {
            hapticFeedback();
            _state = TOUCH_EVENT_OCCURRED;
            _start = millis();
            _first_x = x;
            _first_y = y;
            _dragging = false;
            _drag_x = x;
            _drag_y = y;
        } else {
            _state = TOUCH_EVENT_CLEARED;
            time_t duration = millis() - _start;
            x = _last_x;
            y = _last_y;
            this->setInterval(fastTapMode ? TOUCH_POLL_INTERVAL_RELEASE_FAST : TOUCH_POLL_INTERVAL_RELEASE);

            // If a drag was reported for this gesture, tell the consumer the finger is gone.
            // Dispatched immediately and separately, because the release is ALSO still classified
            // below: everything that predates drag support - frame paging, menu navigation, node
            // list scrolling, the games module's D-pad - listens for that swipe and has to keep
            // receiving it. A consumer acting on the drag stream owns ignoring the swipe that
            // follows it.
            if (_dragging) {
                _dragging = false;
                TouchEvent de;
                de.source = this->_originName;
                de.touchEvent = static_cast<char>(TOUCH_ACTION_DRAG_END);
                de.x = x;
                de.y = y;
                LOG_DEBUG("action DRAG END(%d/%d)", x, y);
                onEvent(de);
            }

            {
                // compute distance
                int16_t dx = x - _first_x;
                int16_t dy = y - _first_y;
                uint16_t adx = abs(dx);
                uint16_t ady = abs(dy);

                // swipe horizontal
                if (adx > ady && adx > TOUCH_THRESHOLD_X) {
                    if (0 > dx) { // swipe right to left
                        e.touchEvent = static_cast<char>(TOUCH_ACTION_LEFT);
                        LOG_DEBUG("action SWIPE: right to left");
                    } else { // swipe left to right
                        e.touchEvent = static_cast<char>(TOUCH_ACTION_RIGHT);
                        LOG_DEBUG("action SWIPE: left to right");
                    }
                }
                // swipe vertical
                else if (ady > adx && ady > TOUCH_THRESHOLD_Y) {
                    if (0 > dy) { // swipe bottom to top
                        e.touchEvent = static_cast<char>(TOUCH_ACTION_UP);
                        LOG_DEBUG("action SWIPE: bottom to top");
                    } else { // swipe top to bottom
                        e.touchEvent = static_cast<char>(TOUCH_ACTION_DOWN);
                        LOG_DEBUG("action SWIPE: top to bottom");
                    }
                }
                // tap
                else {
                    if (duration > 0 && (duration < TIME_LONG_PRESS || !allowLongPress)) {
                        if (_tapped) {
                            _tapped = false;
                        } else {
                            _tapped = true;
                        }
                    } else {
                        _tapped = false;
                    }
                }
            }
        }
    } else if (touched && dragEventsEnabled()) {
        // Finger still down. Report movement as it happens, so a consumer can track the finger
        // rather than waiting for the release-time direction classification below. Uses
        // _last_x/_last_y rather than x/y because those are only refreshed on a genuine touch
        // sample - during the TOUCH_RELEASE_GRACE_MS dropout window x/y are stale.
        if (!_dragging) {
            // Not a drag until the finger has actually travelled: this is what separates a drag
            // from a tap whose finger wobbled a pixel or two.
            if (abs(_last_x - _first_x) >= TOUCH_DRAG_START_THRESHOLD || abs(_last_y - _first_y) >= TOUCH_DRAG_START_THRESHOLD) {
                _dragging = true;
                _drag_x = _last_x;
                _drag_y = _last_y;
                e.touchEvent = static_cast<char>(TOUCH_ACTION_DRAG);
                LOG_DEBUG("action DRAG START(%d/%d)", _last_x, _last_y);
            }
        } else if (abs(_last_x - _drag_x) >= TOUCH_DRAG_MIN_STEP || abs(_last_y - _drag_y) >= TOUCH_DRAG_MIN_STEP) {
            _drag_x = _last_x;
            _drag_y = _last_y;
            e.touchEvent = static_cast<char>(TOUCH_ACTION_DRAG);
        }
    }
    _touchedOld = touched;

#if defined RAK14014
    // Speed up the processing speed of the keyboard in virtual keyboard mode
    auto state = cannedMessageModule->getRunState();
    if (state == CANNED_MESSAGE_RUN_STATE_FREETEXT) {
        if (_tapped) {
            _tapped = false;
            e.touchEvent = static_cast<char>(TOUCH_ACTION_TAP);
            LOG_DEBUG("action TAP(%d/%d)", _last_x, _last_y);
        }
    } else {
        if (_tapped && (time_t(millis()) - _start) > TIME_LONG_PRESS - 50) {
            _tapped = false;
            e.touchEvent = static_cast<char>(TOUCH_ACTION_TAP);
            LOG_DEBUG("action TAP(%d/%d)", _last_x, _last_y);
        }
    }
#else
    // fire TAP event when no 2nd tap occurred within time
    if (_tapped) {
        _tapped = false;
        e.touchEvent = static_cast<char>(TOUCH_ACTION_TAP);
        LOG_DEBUG("action TAP(%d/%d)", _last_x, _last_y);
    }
#endif

    // fire LONG_PRESS event without the need for release
    // Never mid-drag: the finger having travelled is exactly what says this gesture isn't a press,
    // and firing here would also clobber a TOUCH_ACTION_DRAG set for this same poll.
    if (allowLongPress && touched && !_dragging && (time_t(millis()) - _start) > TIME_LONG_PRESS) {
        // tricky: prevent reoccurring events and another touch event when releasing
        _start = millis() + 30000;
        e.touchEvent = static_cast<char>(TOUCH_ACTION_LONG_PRESS);
        LOG_DEBUG("action LONG PRESS(%d/%d)", _last_x, _last_y);
    }

    if (e.touchEvent != TOUCH_ACTION_NONE) {
        e.source = this->_originName;
        e.x = _last_x;
        e.y = _last_y;
        onEvent(e);
    }

    return interval;
}

void TouchScreenBase::hapticFeedback()
{
#if defined(T_WATCH_S3) || defined(T_WATCH_ULTRA)
    drv.setWaveform(0, 75);
    drv.setWaveform(1, 0); // end waveform
    drv.go();
#endif
}

bool TouchScreenBase::fastTapModeEnabled() const
{
    return false;
}

bool TouchScreenBase::longPressEnabled() const
{
    return true;
}

bool TouchScreenBase::dragEventsEnabled() const
{
    return false;
}
