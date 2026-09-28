// Covers src/graphics/niche/Map/MapRouteParse.h: the Valhalla request URL for Map > Navigate's street routing, and
// the parse of its reply into route points and turn instructions.
//
// Why it matters: the reply comes off the network and drives what the map tells someone to do next. A misdecoded
// shape draws the route through the wrong streets; a maneuver pinned to the wrong point announces a turn at the
// wrong place; an overrun on a long route would take the device down mid-trip.
//
// Regressions guarded:
//   - The request losing a coordinate's precision, the travel mode, or its URL encoding.
//   - Encoded polylines misdecoded, including the JSON-escaped backslash a polyline can contain.
//   - Maneuvers losing their instruction, type or start point, or legs after the first being mixed in.
//   - A route longer than the caller's buffer overflowing it instead of being thinned, with maneuvers remapped
//     onto the thinned points and the last point always kept.
//   - Malformed replies being partly accepted.
//
// No feature guard: the header has no dependencies, so this runs on every native build.

// MeshTypes.h before TestUtil.h, per test/README.md - it pulls in Arduino.h, which the portduino runner links
// against. The header under test needs none of it.
#include "MeshTypes.h"
#include "TestUtil.h"
#include "graphics/niche/Map/MapRouteParse.h"
#include <cstring>
#include <string>
#include <unity.h>

using namespace NicheGraphics::MapTiles::Route;

struct Buffers {
    int32_t lat[64], lon[64];
    Maneuver maneuvers[8];
    Result result;
    explicit Buffers(uint32_t cap = 64) : result{lat, lon, cap, 0, maneuvers, 8, 0, 0, 0} {}
};

// Four points near Grass Valley, precision 6.
static const char kShape[] = "ivvxiAhk~{eFmZiaAg^_|BclA}L";

static std::string reply(const char *shape, const char *maneuvers)
{
    return std::string("{\"trip\":{\"locations\":[{\"lat\":39.2,\"lon\":-121.0}],\"legs\":[{\"maneuvers\":[") + maneuvers +
           "],\"summary\":{\"length\":0.4},\"shape\":\"" + shape +
           "\"}],\"summary\":{\"length\":0.412,\"time\":95.5},\"status\":0,\"units\":\"kilometers\"},\"id\":\"x\"}";
}

static const char kManeuvers[] =
    "{\"type\":1,\"instruction\":\"Drive east on Main Street.\",\"length\":0.2,\"begin_shape_index\":0,"
    "\"end_shape_index\":2,\"street_names\":[\"Main Street\"],\"verbal_pre_transition_instruction\":\"Drive east.\"},"
    "{\"type\":10,\"instruction\":\"Turn right onto Mill Street.\",\"length\":0.212,\"begin_shape_index\":2},"
    "{\"type\":4,\"instruction\":\"You have arrived at your destination.\",\"length\":0,\"begin_shape_index\":3}";

static void test_buildRequestUrl_encodes_the_request()
{
    char out[1024];
    TEST_ASSERT_TRUE(
        buildRequestUrl(kDefaultRouteUrl, 39.219061, -121.061061, 39.221234, -121.057777, Mode::Bicycle, out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(0, strncmp(out, "https://valhalla1.openstreetmap.de/route?json=%7B%22locations%22", 64));
    TEST_ASSERT_NOT_NULL(strstr(out, "%22lat%22%3A39.219061%2C%22lon%22%3A-121.061061"));
    TEST_ASSERT_NOT_NULL(strstr(out, "%22costing%22%3A%22bicycle%22"));
    TEST_ASSERT_NULL(strchr(out, ' '));
    TEST_ASSERT_NULL(strchr(out, '"'));

    TEST_ASSERT_TRUE(buildRequestUrl(kDefaultRouteUrl, 0, 0, 1, 1, Mode::Walk, out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "pedestrian"));
    TEST_ASSERT_TRUE(buildRequestUrl("http://host/route?json={json}&key=abc", 0, 0, 1, 1, Mode::Car, out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "%22auto%22"));
    TEST_ASSERT_EQUAL_STRING("&key=abc", out + strlen(out) - 8);
}

static void test_buildRequestUrl_fails_closed()
{
    char out[64];
    TEST_ASSERT_FALSE(buildRequestUrl("https://host/route", 0, 0, 1, 1, Mode::Car, out, sizeof(out))); // no {json}
    TEST_ASSERT_FALSE(buildRequestUrl("ftp://host/{json}", 0, 0, 1, 1, Mode::Car, out, sizeof(out)));
    TEST_ASSERT_FALSE(buildRequestUrl(kDefaultRouteUrl, 0, 0, 1, 1, Mode::Car, out, sizeof(out))); // too small
    TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_parseValhalla_reads_points_maneuvers_and_summary()
{
    Buffers b;
    const std::string json = reply(kShape, kManeuvers);
    TEST_ASSERT_TRUE(parseValhalla(json.c_str(), json.size(), b.result));

    TEST_ASSERT_EQUAL_UINT32(4, b.result.pointCount);
    TEST_ASSERT_EQUAL_INT32(392190610, b.lat[0]);
    TEST_ASSERT_EQUAL_INT32(-1210610610, b.lon[0]);
    TEST_ASSERT_EQUAL_INT32(392212340, b.lat[3]);
    TEST_ASSERT_EQUAL_INT32(-1210577770, b.lon[3]);

    TEST_ASSERT_EQUAL_UINT16(3, b.result.maneuverCount);
    TEST_ASSERT_EQUAL_UINT16(10, b.maneuvers[1].type);
    TEST_ASSERT_EQUAL_UINT32(2, b.maneuvers[1].beginIndex);
    TEST_ASSERT_EQUAL_STRING("Turn right onto Mill Street.", b.maneuvers[1].instruction);
    TEST_ASSERT_EQUAL_UINT16(4, b.maneuvers[2].type);
    TEST_ASSERT_TRUE(b.result.lengthKm > 0.411f && b.result.lengthKm < 0.413f); // the trip's, not the leg's
    TEST_ASSERT_TRUE(b.result.timeSec > 95.4f && b.result.timeSec < 95.6f);
}

static void test_parseValhalla_decodes_an_escaped_backslash()
{
    // (10, 10) then (10.000464, 10): the second latitude delta encodes to a backslash, escaped in the JSON.
    Buffers b;
    const std::string json = reply("_gjaR_gjaR_\\\\?", "");
    TEST_ASSERT_TRUE(parseValhalla(json.c_str(), json.size(), b.result));
    TEST_ASSERT_EQUAL_UINT32(2, b.result.pointCount);
    TEST_ASSERT_EQUAL_INT32(100000000, b.lat[0]);
    TEST_ASSERT_EQUAL_INT32(100004640, b.lat[1]);
    TEST_ASSERT_EQUAL_INT32(100000000, b.lon[1]);
}

static void test_parseValhalla_thins_a_long_route_to_fit()
{
    // 40 points into room for 8: every stride-th kept, the last always, and never more than fits.
    std::string shape;
    for (int i = 0; i < 40; i++)
        shape += i == 0 ? "_gjaR_gjaR" : "o}@?"; // start at (10, 10), then 0.001 degrees north each step
    const std::string json = reply(shape.c_str(), "{\"type\":10,\"instruction\":\"Turn right.\",\"begin_shape_index\":20},"
                                                  "{\"type\":4,\"instruction\":\"Arrive.\",\"begin_shape_index\":39}");
    Buffers b(8);
    TEST_ASSERT_TRUE(parseValhalla(json.c_str(), json.size(), b.result));
    TEST_ASSERT_TRUE(b.result.pointCount <= 8);
    TEST_ASSERT_TRUE(b.result.pointCount >= 6);
    TEST_ASSERT_EQUAL_INT32(100000000, b.lat[0]);
    TEST_ASSERT_EQUAL_INT32(100390000, b.lat[b.result.pointCount - 1]); // the last point survives
    TEST_ASSERT_EQUAL_UINT32(b.result.pointCount - 1, b.maneuvers[1].beginIndex);
    TEST_ASSERT_TRUE(b.maneuvers[0].beginIndex > 0 && b.maneuvers[0].beginIndex < b.result.pointCount - 1);
}

static void test_parseValhalla_reads_only_the_first_leg()
{
    const std::string json = "{\"trip\":{\"legs\":[{\"shape\":\"" + std::string(kShape) +
                             "\",\"maneuvers\":[{\"type\":1,\"begin_shape_index\":0}]},"
                             "{\"shape\":\"_gjaR_gjaR\",\"maneuvers\":[{\"type\":4,\"begin_shape_index\":0}]}]}}";
    Buffers b;
    TEST_ASSERT_TRUE(parseValhalla(json.c_str(), json.size(), b.result));
    TEST_ASSERT_EQUAL_UINT32(4, b.result.pointCount);
    TEST_ASSERT_EQUAL_UINT16(1, b.result.maneuverCount);
}

static void test_parseValhalla_malformed_is_refused()
{
    const char *bad[] = {
        "",
        "{\"error_code\":442,\"error\":\"No path could be found for input\"}", // no shape
        "{\"trip\":{\"legs\":[{\"shape\":\"ivvxiAhk~{eF\"}]}}",                // a single point is no route
        "{\"trip\":{\"legs\":[{\"shape\":\"ivvx\"}]}}",                        // cut mid-number
        "{\"trip\":{\"legs\":[{\"shape\":\"ab\\\\u0041\"}]}}",                 // an escape a polyline can't hold
        "{\"trip\":{\"legs\":[{\"shape\":\"ivvxiAhk~{eFmZiaA\"",               // cut mid-reply
        "<html>502 Bad Gateway</html>",
    };
    for (const char *json : bad) {
        Buffers b;
        TEST_ASSERT_FALSE_MESSAGE(parseValhalla(json, strlen(json), b.result), json);
    }
}

static void test_parseValhalla_keeps_to_the_maneuver_cap()
{
    std::string maneuvers;
    for (int i = 0; i < 12; i++)
        maneuvers += std::string(i ? "," : "") + "{\"type\":8,\"begin_shape_index\":1}";
    const std::string json = reply(kShape, maneuvers.c_str());
    Buffers b;
    TEST_ASSERT_TRUE(parseValhalla(json.c_str(), json.size(), b.result));
    TEST_ASSERT_EQUAL_UINT16(8, b.result.maneuverCount);
}

void setUp(void) {}
void tearDown(void) {}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();

    printf("\n=== Request ===\n");
    RUN_TEST(test_buildRequestUrl_encodes_the_request);
    RUN_TEST(test_buildRequestUrl_fails_closed);

    printf("\n=== Reply ===\n");
    RUN_TEST(test_parseValhalla_reads_points_maneuvers_and_summary);
    RUN_TEST(test_parseValhalla_decodes_an_escaped_backslash);
    RUN_TEST(test_parseValhalla_thins_a_long_route_to_fit);
    RUN_TEST(test_parseValhalla_reads_only_the_first_leg);
    RUN_TEST(test_parseValhalla_keeps_to_the_maneuver_cap);

    printf("\n=== Fails closed ===\n");
    RUN_TEST(test_parseValhalla_malformed_is_refused);

    exit(UNITY_END());
}

void loop() {}
