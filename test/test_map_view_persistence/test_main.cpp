// Covers MapViewPersistence::decide() in src/graphics/draw/MapViewPersistence.h: whether the BaseUI Map frame's
// zoom and our last known location have changed enough to write into uiconfig.map_data.home.
//
// Why the policy is required: MapRenderer::saveView() asks it on every clean shutdown, reboot and deep sleep, and
// on a two-hour autosave tick. Each yes rewrites /prefs/uiconfig.proto on flash, so it must say yes when the map
// would otherwise reopen somewhere stale, and no when nothing meaningful changed. device-ui reads the same field,
// so it must also never produce a home device-ui would misread.
//
// Regressions guarded:
//   - GPS jitter on a stationary device rewriting flash at every tick and shutdown (the 100 m threshold).
//   - A zoom saved with no location behind it, which leaves has_home set on lat/lng 0,0 - device-ui then
//     opens its map on Null Island.
//   - A first-ever home written without a zoom, which device-ui opens at world view (zoom 0).
//   - A location refresh overwriting the user's saved zoom with whatever the map currently shows.
//   - A lockdown build copying the device's location into uiconfig, the one file it leaves unencrypted.
//   - Follow Me (map_data.follow_gps) being unable to persist on a device with no position, or conversely
//     loosening the zoom rule above so a pending zoom rides out on a follow-me write with no home behind it.
//
// No feature guard: the header has no BASEUI_HAS_MAP gate and no dependencies, so this runs on every native build.

// MeshTypes.h before TestUtil.h, per test/README.md - it pulls in Arduino.h, which the portduino runner links
// against. The header under test needs none of it.
#include "MeshTypes.h"
#include "TestUtil.h"
#include "graphics/draw/MapViewPersistence.h"
#include <cstdio>
#include <unity.h>

using namespace graphics::MapViewPersistence;

// Nothing pending, nothing known, location permitted - each test turns on only what it is about.
static Inputs baseline()
{
    Inputs in{};
    in.locationAllowed = true;
    return in;
}

static void test_idle_never_writes()
{
    TEST_ASSERT_FALSE(decide(baseline()).write);
}

static void test_first_fix_with_no_home_writes_location_and_zoom()
{
    Inputs in = baseline();
    in.haveLivePosition = true;

    const Decision d = decide(in);
    TEST_ASSERT_TRUE(d.write);
    TEST_ASSERT_TRUE(d.writeLocation);
    TEST_ASSERT_TRUE(d.writeZoom); // otherwise device-ui opens this home at zoom 0
}

static void test_jitter_below_threshold_never_writes()
{
    Inputs in = baseline();
    in.haveLivePosition = true;
    in.haveSavedLocation = true;
    in.metersFromSaved = kLocationMinMoveMeters - 1.0f;

    TEST_ASSERT_FALSE(decide(in).write);
}

static void test_move_writes_location_only()
{
    Inputs in = baseline();
    in.haveLivePosition = true;
    in.haveSavedLocation = true;
    in.metersFromSaved = kLocationMinMoveMeters;

    const Decision d = decide(in);
    TEST_ASSERT_TRUE(d.writeLocation);
    TEST_ASSERT_FALSE(d.writeZoom); // the saved zoom is the user's; a location refresh must not overwrite it
}

static void test_zoom_writes_against_a_saved_location()
{
    Inputs in = baseline();
    in.haveSavedLocation = true;
    in.zoomPending = true;

    const Decision d = decide(in);
    TEST_ASSERT_TRUE(d.writeZoom);
    TEST_ASSERT_FALSE(d.writeLocation); // no live fix to copy, so the saved location stands
}

static void test_zoom_with_no_location_anywhere_stays_pending()
{
    Inputs in = baseline();
    in.zoomPending = true;

    TEST_ASSERT_FALSE(decide(in).write); // writing would set has_home on 0,0
}

static void test_zoom_write_refreshes_a_live_location_for_free()
{
    Inputs in = baseline();
    in.zoomPending = true;
    in.haveLivePosition = true;
    in.haveSavedLocation = true;
    in.metersFromSaved = 1.0f; // location on its own would not be due

    const Decision d = decide(in);
    TEST_ASSERT_TRUE(d.writeZoom);
    TEST_ASSERT_TRUE(d.writeLocation);
}

static void test_lockdown_never_writes_location()
{
    Inputs in = baseline();
    in.locationAllowed = false;
    in.haveLivePosition = true;

    TEST_ASSERT_FALSE(decide(in).write); // no home yet, and none may be created from our position

    in.haveSavedLocation = true;
    in.metersFromSaved = 10 * kLocationMinMoveMeters;
    in.zoomPending = true;
    const Decision d = decide(in);
    TEST_ASSERT_TRUE(d.writeZoom); // a home saved before lockdown can still carry the zoom
    TEST_ASSERT_FALSE(d.writeLocation);
}

static void test_follow_me_writes_with_no_location_at_all()
{
    Inputs in = baseline();
    in.followMePending = true;

    const Decision d = decide(in);
    TEST_ASSERT_TRUE(d.write);
    TEST_ASSERT_TRUE(d.writeFollowMe);
    // follow_gps is a field of Map, not of home, so it must not fabricate a home at 0,0 to save itself
    TEST_ASSERT_FALSE(d.writeLocation);
    TEST_ASSERT_FALSE(d.writeZoom);
}

static void test_follow_me_does_not_release_a_stuck_zoom()
{
    Inputs in = baseline();
    in.followMePending = true;
    in.zoomPending = true; // no live fix and no saved home, so the zoom still has nowhere to live

    const Decision d = decide(in);
    TEST_ASSERT_TRUE(d.writeFollowMe);
    TEST_ASSERT_FALSE(d.writeZoom);
}

static void test_follow_me_rides_along_with_a_location_write()
{
    Inputs in = baseline();
    in.followMePending = true;
    in.haveLivePosition = true; // first fix, nothing saved yet

    const Decision d = decide(in);
    TEST_ASSERT_TRUE(d.writeFollowMe);
    TEST_ASSERT_TRUE(d.writeLocation);
    TEST_ASSERT_TRUE(d.writeZoom); // a first-ever home still needs a zoom with it
}

void setUp(void) {}
void tearDown(void) {}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();

    printf("\n=== Location ===\n");
    RUN_TEST(test_idle_never_writes);
    RUN_TEST(test_first_fix_with_no_home_writes_location_and_zoom);
    RUN_TEST(test_jitter_below_threshold_never_writes);
    RUN_TEST(test_move_writes_location_only);

    printf("\n=== Zoom ===\n");
    RUN_TEST(test_zoom_writes_against_a_saved_location);
    RUN_TEST(test_zoom_with_no_location_anywhere_stays_pending);
    RUN_TEST(test_zoom_write_refreshes_a_live_location_for_free);

    printf("\n=== Follow Me ===\n");
    RUN_TEST(test_follow_me_writes_with_no_location_at_all);
    RUN_TEST(test_follow_me_does_not_release_a_stuck_zoom);
    RUN_TEST(test_follow_me_rides_along_with_a_location_write);

    printf("\n=== Lockdown ===\n");
    RUN_TEST(test_lockdown_never_writes_location);

    exit(UNITY_END());
}

void loop() {}
