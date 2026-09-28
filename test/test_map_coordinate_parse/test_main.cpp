// Covers MapCoordinateParse::parse() in src/graphics/draw/MapCoordinateParse.h: turning what someone typed into
// Navigate > Coordinates into a latitude and longitude.
//
// Why it matters: the result becomes a navigation target the map draws a line and distance to, and is saved to
// flash to be offered again after a reboot. A misread - axes swapped, a hemisphere dropped, minutes taken as
// decimal degrees - points the user somewhere plausible but wrong, which is worse than refusing the input.
//
// Regressions guarded:
//   - The common forms people paste or type (decimal, hemisphere letters either side, degrees-minutes-seconds with
//     or without symbols) being rejected.
//   - Letters that name the axes being ignored, so "121W 39N" lands in the ocean instead of California.
//   - Ambiguous input (odd bare-number counts, a sign and a letter together, minutes >= 60, a fraction before the
//     last part, anything with other letters in it) being guessed at instead of refused.
//   - Out-of-range degrees being accepted, and a refused parse overwriting the caller's previous values.
//
// No feature guard: the header has no dependencies, so this runs on every native build.

// MeshTypes.h before TestUtil.h, per test/README.md - it pulls in Arduino.h, which the portduino runner links
// against. The header under test needs none of it.
#include "MeshTypes.h"
#include "TestUtil.h"
#include "graphics/draw/MapCoordinateParse.h"
#include <cmath>
#include <unity.h>

using graphics::MapCoordinateParse::parse;

static constexpr double kTol = 1e-6;

// This Unity build has double support off, so compare doubles by hand to keep the full precision under test.
#define EXPECT_NEAR(expected, actual, tol, msg)                                                                                  \
    TEST_ASSERT_TRUE_MESSAGE(fabs((double)(expected) - (double)(actual)) <= (tol), msg)

static void expectPosition(const char *text, double lat, double lon)
{
    double gotLat = 999, gotLon = 999;
    TEST_ASSERT_TRUE_MESSAGE(parse(text, gotLat, gotLon), text);
    EXPECT_NEAR(lat, gotLat, kTol, text);
    EXPECT_NEAR(lon, gotLon, kTol, text);
}

static void expectRefused(const char *text)
{
    double lat = 12.5, lon = 34.5;
    TEST_ASSERT_FALSE_MESSAGE(parse(text, lat, lon), text);
    // A refused parse leaves the caller's values alone
    EXPECT_NEAR(12.5, lat, 0.0, "caller value changed");
    EXPECT_NEAR(34.5, lon, 0.0, "caller value changed");
}

static void test_decimal_degrees()
{
    expectPosition("39.2, -121.3", 39.2, -121.3);
    expectPosition("39.2,-121.3", 39.2, -121.3);
    expectPosition("  39.2   -121.3  ", 39.2, -121.3);
    expectPosition("-33.8688; 151.2093", -33.8688, 151.2093);
    expectPosition("+0.5, .25", 0.5, 0.25);
}

static void test_hemisphere_letters_trailing_and_leading()
{
    expectPosition("39.2N 121.3W", 39.2, -121.3);
    expectPosition("39.2 n 121.3 w", 39.2, -121.3);
    expectPosition("N39.2 W121.3", 39.2, -121.3);
    expectPosition("39.2 N, 121.3 W", 39.2, -121.3);
    expectPosition("S 33.8688, E 151.2093", -33.8688, 151.2093);
    // One letter is enough to place the pair
    expectPosition("39.2N 121.3", 39.2, 121.3);
}

static void test_letters_override_the_order()
{
    expectPosition("121.3W 39.2N", 39.2, -121.3);
    expectPosition("W121.3 N39.2", 39.2, -121.3);
    expectPosition("121.3 W, 39.2", 39.2, -121.3);
}

static void test_degrees_minutes_seconds()
{
    expectPosition("39\xC2\xB0"
                   "12'30\"N 121\xC2\xB0"
                   "18'0\"W",
                   39.0 + 12.0 / 60 + 30.0 / 3600, -(121.0 + 18.0 / 60));
    expectPosition("39 12 30 N 121 18 0 W", 39.0 + 12.0 / 60 + 30.0 / 3600, -(121.0 + 18.0 / 60));
    expectPosition("39 12.5, -121 18", 39.0 + 12.5 / 60, -(121.0 + 18.0 / 60));
    // Bare numbers split evenly between the two
    expectPosition("39 12 -121 18", 39.2, -121.3);
    expectPosition("39 12 30 121 18 0", 39.0 + 12.0 / 60 + 30.0 / 3600, 121.3);
}

static void test_ambiguous_input_is_refused()
{
    expectRefused("39.2");
    expectRefused("39 12 121");          // three bare numbers split no sensible way
    expectRefused("-39.2 S, 121.3 E");   // sign and letter both give the hemisphere
    expectRefused("39.2N 121.3N");       // two latitudes
    expectRefused("39 60, 121 0");       // minutes must be under 60
    expectRefused("39.5 12, 121 0");     // only the last part may have a fraction
    expectRefused("39, -121, 5");        // two separators
    expectRefused("39 -12, 121");        // minutes carry no sign
    expectRefused("Main Street 12, 34"); // an address, not a position
    expectRefused("1e5, 3");
    expectRefused("");
    expectRefused(",");
    expectRefused("N W");
}

static void test_out_of_range_is_refused()
{
    expectRefused("91, 0");
    expectRefused("0, 181");
    expectRefused("-90.0001, 0");
    expectPosition("90, 180", 90, 180);
    expectPosition("-90, -180", -90, -180);
}

static void test_null_input()
{
    double lat = 1, lon = 2;
    TEST_ASSERT_FALSE(parse(nullptr, lat, lon));
}

void setUp(void) {}
void tearDown(void) {}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();

    printf("\n=== Accepted forms ===\n");
    RUN_TEST(test_decimal_degrees);
    RUN_TEST(test_hemisphere_letters_trailing_and_leading);
    RUN_TEST(test_letters_override_the_order);
    RUN_TEST(test_degrees_minutes_seconds);

    printf("\n=== Fails closed ===\n");
    RUN_TEST(test_ambiguous_input_is_refused);
    RUN_TEST(test_out_of_range_is_refused);
    RUN_TEST(test_null_input);

    exit(UNITY_END());
}

void loop() {}
