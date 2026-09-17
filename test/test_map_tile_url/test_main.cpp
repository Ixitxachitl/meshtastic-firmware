// Covers expandTileUrl() in src/graphics/niche/Map/MapTileUrl.h: turning the slippy-map URL template found in
// /maps/<style>/.url into the URL for one tile.
//
// Why it matters: the template is data on the SD card, written by hand or by device-ui, not something the build
// controls. A malformed one must fail closed rather than produce a URL that fetches the wrong tile or a
// half-substituted address, because the result is fed straight to HTTPClient and whatever comes back is cached
// to the card under the requested tile's name - a wrong tile would then persist and look like map corruption.
//
// Regressions guarded:
//   - A template missing {z}, {x} or {y} silently producing a URL anyway, so every tile resolves to the same one.
//   - Substitution running off the end of the caller's buffer, or leaving a truncated URL behind on overflow.
//   - Negative or multi-digit coordinates being mangled (y is signed in the reader's world-pixel arithmetic).
//   - A non-http scheme reaching HTTPClient, which this build cannot speak.
//
// No feature guard: the header has no BASEUI_MAP_ONLINE_TILES gate and no dependencies, so this runs on every
// native build.

// MeshTypes.h before TestUtil.h, per test/README.md - it pulls in Arduino.h, which the portduino runner links
// against. The header under test needs none of it.
#include "MeshTypes.h"
#include "TestUtil.h"
#include "graphics/niche/Map/MapTileUrl.h"
#include <cstdio>
#include <cstring>
#include <unity.h>

using namespace NicheGraphics::MapTiles::Fetch;

static const char *kOsm = "https://tile.openstreetmap.org/{z}/{x}/{y}.png";

static void test_expands_the_osm_template()
{
    char out[128];
    TEST_ASSERT_TRUE(expandTileUrl(kOsm, 13, 4194, 2725, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://tile.openstreetmap.org/13/4194/2725.png", out);
}

static void test_handles_zero_and_negative_coordinates()
{
    char out[128];
    TEST_ASSERT_TRUE(expandTileUrl(kOsm, 0, 0, 0, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://tile.openstreetmap.org/0/0/0.png", out);

    TEST_ASSERT_TRUE(expandTileUrl(kOsm, 5, -3, 12, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://tile.openstreetmap.org/5/-3/12.png", out);
}

static void test_placeholders_may_repeat_and_reorder()
{
    char out[128];
    TEST_ASSERT_TRUE(expandTileUrl("http://host/{y}/{x}/{z}/{z}.png", 7, 1, 2, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("http://host/2/1/7/7.png", out);
}

static void test_missing_placeholder_is_refused()
{
    char out[128];
    // Without {y} every row would resolve to the same tile, and it would be cached under the right name
    TEST_ASSERT_FALSE(expandTileUrl("https://host/{z}/{x}.png", 3, 4, 5, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_FALSE(expandTileUrl("https://host/tiles.png", 3, 4, 5, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_non_http_scheme_is_refused()
{
    char out[128];
    TEST_ASSERT_FALSE(expandTileUrl("ftp://host/{z}/{x}/{y}.png", 1, 2, 3, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_FALSE(expandTileUrl("/maps/{z}/{x}/{y}.png", 1, 2, 3, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_overflow_fails_closed()
{
    char small[20];
    TEST_ASSERT_FALSE(expandTileUrl(kOsm, 13, 4194, 2725, small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("", small); // never a truncated URL, which would fetch something else
}

static void test_null_and_empty_inputs()
{
    char out[64];
    TEST_ASSERT_FALSE(expandTileUrl(nullptr, 1, 2, 3, out, sizeof(out)));
    TEST_ASSERT_FALSE(expandTileUrl(kOsm, 1, 2, 3, nullptr, 10));
    TEST_ASSERT_FALSE(expandTileUrl(kOsm, 1, 2, 3, out, 0));
    TEST_ASSERT_FALSE(expandTileUrl("", 1, 2, 3, out, sizeof(out)));
}

void setUp(void) {}
void tearDown(void) {}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();

    printf("\n=== Expansion ===\n");
    RUN_TEST(test_expands_the_osm_template);
    RUN_TEST(test_handles_zero_and_negative_coordinates);
    RUN_TEST(test_placeholders_may_repeat_and_reorder);

    printf("\n=== Fails closed ===\n");
    RUN_TEST(test_missing_placeholder_is_refused);
    RUN_TEST(test_non_http_scheme_is_refused);
    RUN_TEST(test_overflow_fails_closed);
    RUN_TEST(test_null_and_empty_inputs);

    exit(UNITY_END());
}

void loop() {}
