#pragma once
#include "Observer.h"
#include "SinglePortModule.h"

/**
 * Waypoint message handling for meshtastic
 */
class WaypointModule : public SinglePortModule, public Observable<const UIFrameEvent *>
{
  public:
    /** Constructor
     * name is for debugging output
     */
    WaypointModule() : SinglePortModule("waypoint", meshtastic_PortNum_WAYPOINT_APP) {}
#if HAS_SCREEN
    bool shouldDraw();
    void onDeviceTimeChanged();

    // Touch scroll - moves the list by an exact finger displacement in screen pixels, for hardware
    // that reports a continuous drag (BASEUI_HAS_TOUCH_DRAG). Pass the delta between consecutive
    // drag reports, not the offset from where the finger landed.
    //
    // The list follows the finger, so dragging down walks back towards the top of the list. Clamped
    // against the geometry the last drawFrame() actually laid out with; a no-op until then, and on
    // a list short enough to fit.
    void scrollByFingerDelta(float dyPx);
#endif
#if !MESHTASTIC_EXCLUDE_WAYPOINT
    /// Broadcast an expired copy of the waypoint so the mesh (and we) discard it.
    bool broadcastDelete(uint32_t waypointId);
#endif
  protected:
    /** Called to handle a particular incoming message

    @return ProcessMessage::STOP if you've guaranteed you've handled this message and no other handlers should be considered for
    it
    */

    virtual Observable<const UIFrameEvent *> *getUIFrameObservable() override { return this; }
#if HAS_SCREEN
    virtual bool wantUIFrame() override { return this->shouldDraw(); }
    virtual void drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) override;
#endif
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
};

extern WaypointModule *waypointModule;
