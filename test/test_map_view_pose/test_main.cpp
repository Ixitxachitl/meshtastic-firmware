// Covers src/graphics/niche/Map/MapViewPose.h: the camera for the heading-up and tilted map views.
//
// Why it matters: the basemap is drawn by walking each view row across the ground (rowToWorld), while every marker,
// the route and the user's own arrow are placed the other way (toScreen). If the two disagree, the route slides off
// the streets it follows and pins drift from their places - worst far ahead, where the tilt compresses the ground.
//
// Regressions guarded:
//   - North-up drifting from the plain offset the flat map has always used.
//   - Heading-up turning the wrong way (east of travel must land to the right).
//   - The two directions of the tilted mapping disagreeing anywhere below the horizon.
//   - Points behind the camera, or past the horizon, being placed on screen instead of refused.
//
// No feature guard: the header has no dependencies, so this runs on every native build.

// MeshTypes.h before TestUtil.h, per test/README.md - it pulls in Arduino.h, which the portduino runner links
// against. The header under test needs none of it.
#include "MeshTypes.h"
#include "TestUtil.h"
#include "graphics/niche/Map/MapViewPose.h"
#include <cmath>
#include <unity.h>

using NicheGraphics::MapTiles::ViewPose;

// This Unity build has double support off, so compare by hand.
#define EXPECT_NEAR(expected, actual, tol, msg)                                                                                  \
    TEST_ASSERT_TRUE_MESSAGE(fabs((double)(expected) - (double)(actual)) <= (tol), msg)

static constexpr float kPi = 3.14159265f;

static void test_northUp_is_a_plain_offset()
{
    const ViewPose pose = ViewPose::northUp(1000.0, 2000.0, 120.0f, 160.0f);
    float sx, sy;
    TEST_ASSERT_TRUE(pose.toScreen(1010.0, 1990.0, sx, sy));
    EXPECT_NEAR(130.0, sx, 1e-4, "x");
    EXPECT_NEAR(150.0, sy, 1e-4, "y");

    double wx, wy;
    float stepX, stepY, scale;
    TEST_ASSERT_TRUE(pose.rowToWorld(150.0f, wx, wy, stepX, stepY, scale));
    EXPECT_NEAR(1000.0 - 120.0, wx, 1e-4, "row start x");
    EXPECT_NEAR(1990.0, wy, 1e-4, "row y");
    EXPECT_NEAR(1.0, stepX, 1e-6, "step x");
    EXPECT_NEAR(0.0, stepY, 1e-6, "step y");
}

static void test_heading_up_puts_the_direction_of_travel_up()
{
    // Heading east: a point east of us is straight up the screen, one to the south is to the right.
    ViewPose pose = ViewPose::northUp(0.0, 0.0, 100.0f, 100.0f);
    pose.setHeading(kPi / 2);
    float sx, sy;
    TEST_ASSERT_TRUE(pose.toScreen(50.0, 0.0, sx, sy)); // east
    EXPECT_NEAR(100.0, sx, 1e-3, "east x");
    EXPECT_NEAR(50.0, sy, 1e-3, "east y");
    TEST_ASSERT_TRUE(pose.toScreen(0.0, 30.0, sx, sy)); // south (world y grows south)
    EXPECT_NEAR(130.0, sx, 1e-3, "south x");
    EXPECT_NEAR(100.0, sy, 1e-3, "south y");
}

static void test_rotated_row_walk_matches_toScreen()
{
    ViewPose pose = ViewPose::northUp(5000.0, 7000.0, 120.0f, 200.0f);
    pose.setHeading(0.7f);
    for (float row : {10.0f, 120.0f, 199.0f, 300.0f}) {
        double wx, wy;
        float stepX, stepY, scale;
        TEST_ASSERT_TRUE(pose.rowToWorld(row, wx, wy, stepX, stepY, scale));
        for (int col : {0, 57, 239}) {
            float sx, sy;
            TEST_ASSERT_TRUE(pose.toScreen(wx + stepX * col, wy + stepY * col, sx, sy));
            EXPECT_NEAR(col, sx, 1e-2, "column");
            EXPECT_NEAR(row, sy, 1e-2, "row");
        }
    }
}

static void test_tilted_row_walk_matches_toScreen()
{
    ViewPose pose = ViewPose::northUp(5000.0, 7000.0, 160.0f, 360.0f);
    pose.setHeading(-1.1f);
    pose.setTilt(50.0f * kPi / 180.0f, 600.0f, 1e6f);
    for (float row : {5.0f, 100.0f, 250.0f, 359.0f, 470.0f}) {
        double wx, wy;
        float stepX, stepY, scale;
        TEST_ASSERT_TRUE_MESSAGE(pose.rowToWorld(row, wx, wy, stepX, stepY, scale), "row below the horizon");
        for (int col : {0, 100, 319}) {
            float sx, sy;
            TEST_ASSERT_TRUE(pose.toScreen(wx + stepX * col, wy + stepY * col, sx, sy));
            EXPECT_NEAR(col, sx, 0.05, "column");
            EXPECT_NEAR(row, sy, 0.05, "row");
        }
    }
}

static void test_tilt_shrinks_the_ground_ahead()
{
    ViewPose pose = ViewPose::northUp(0.0, 0.0, 160.0f, 360.0f);
    pose.setHeading(0.0f);
    pose.setTilt(50.0f * kPi / 180.0f, 600.0f, 1e6f);
    double wx, wy;
    float stepX, stepY, nearScale, farScale;
    TEST_ASSERT_TRUE(pose.rowToWorld(470.0f, wx, wy, stepX, stepY, nearScale)); // below the anchor: nearer
    TEST_ASSERT_TRUE(pose.rowToWorld(5.0f, wx, wy, stepX, stepY, farScale));    // top of the view: far ahead
    TEST_ASSERT_TRUE(nearScale < 1.0f);
    TEST_ASSERT_TRUE(farScale > 1.5f);
}

static void test_horizon_and_behind_camera_are_refused()
{
    ViewPose pose = ViewPose::northUp(0.0, 0.0, 160.0f, 360.0f);
    pose.setHeading(0.0f);
    pose.setTilt(60.0f * kPi / 180.0f, 300.0f, 2000.0f);
    double wx, wy;
    float stepX, stepY, scale;
    // The horizon sits depth*cot(tilt) above the anchor; a row well above it has no ground
    TEST_ASSERT_FALSE(pose.rowToWorld(360.0f - 300.0f / tanf(60.0f * kPi / 180.0f) - 20.0f, wx, wy, stepX, stepY, scale));

    float sx, sy;
    TEST_ASSERT_FALSE(pose.toScreen(0.0, 5000.0, sx, sy));  // far behind: z goes negative
    TEST_ASSERT_FALSE(pose.toScreen(0.0, -5000.0, sx, sy)); // ahead, but past maxAhead
    TEST_ASSERT_TRUE(pose.toScreen(0.0, -500.0, sx, sy));
}

void setUp(void) {}
void tearDown(void) {}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();

    RUN_TEST(test_northUp_is_a_plain_offset);
    RUN_TEST(test_heading_up_puts_the_direction_of_travel_up);
    RUN_TEST(test_rotated_row_walk_matches_toScreen);
    RUN_TEST(test_tilted_row_walk_matches_toScreen);
    RUN_TEST(test_tilt_shrinks_the_ground_ahead);
    RUN_TEST(test_horizon_and_behind_camera_are_refused);

    exit(UNITY_END());
}

void loop() {}
