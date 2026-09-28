// Covers src/graphics/niche/Map/MapRouteStore.h: the file format of Map > Navigate > Saved Routes.
//
// Why it matters: a saved route is what the device follows when it has no WiFi to fetch another. A route that reads
// back with its points shifted, a turn pinned to the wrong point, or a damaged file read as if it were whole, would
// send someone the wrong way with nothing to correct it.
//
// Regressions guarded:
//   - A route not coming back exactly as saved: points, turns, target, mode, totals.
//   - The listing's header-only read disagreeing with the full read.
//   - A file cut short, or with a flipped byte, being accepted.
//   - Longitude deltas across the antimeridian overflowing instead of wrapping back exactly.
//   - Arrays too small for the saved route being overrun instead of refused.
//
// No feature guard: the header has no dependencies, so this runs on every native build.

// MeshTypes.h before TestUtil.h, per test/README.md - it pulls in Arduino.h, which the portduino runner links
// against. The header under test needs none of it.
#include "MeshTypes.h"
#include "TestUtil.h"
#include "graphics/niche/Map/MapRouteStore.h"
#include <cstring>
#include <unity.h>

using namespace NicheGraphics::MapTiles;
using RouteStore::Header;

static Header makeHeader(uint32_t points, uint16_t turns)
{
    Header h{};
    h.sequence = 42;
    h.targetLatE7 = 389407716;
    h.targetLonE7 = -1211008497;
    h.nodeNum = 0x1234abcd;
    h.waypointId = 7;
    h.travelMode = 1;
    strncpy(h.name, "3123 Professional Dr", sizeof(h.name) - 1);
    h.lengthKm = 12.5f;
    h.timeSec = 900.0f;
    h.pointCount = points;
    h.maneuverCount = turns;
    return h;
}

static void test_round_trip_is_exact()
{
    const int32_t lat[] = {392087700, 392090000, 392100000, 391500000, 389407716};
    const int32_t lon[] = {-1207928010, -1207930000, -1207940000, -1210000000, -1211008497};
    Route::Maneuver turns[2] = {};
    turns[0] = {1, 0, 0.5f, "Drive west on Main Street."};
    turns[1] = {10, 3, 2.25f, "Turn right onto Professional Drive."};
    const Header h = makeHeader(5, 2);

    uint8_t buf[512];
    const size_t n = RouteStore::encode(h, lat, lon, turns, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > RouteStore::kHeaderBytes);
    TEST_ASSERT_TRUE(n <= RouteStore::maxEncodedSize(5, 2));

    Header back{};
    int32_t lat2[8], lon2[8];
    Route::Maneuver turns2[4];
    TEST_ASSERT_TRUE(RouteStore::decode(buf, n, back, lat2, lon2, 8, turns2, 4));
    TEST_ASSERT_EQUAL_UINT32(42, back.sequence);
    TEST_ASSERT_EQUAL_INT32(h.targetLatE7, back.targetLatE7);
    TEST_ASSERT_EQUAL_INT32(h.targetLonE7, back.targetLonE7);
    TEST_ASSERT_EQUAL_UINT32(h.nodeNum, back.nodeNum);
    TEST_ASSERT_EQUAL_UINT32(7, back.waypointId);
    TEST_ASSERT_EQUAL_UINT8(1, back.travelMode);
    TEST_ASSERT_EQUAL_STRING("3123 Professional Dr", back.name);
    TEST_ASSERT_EQUAL_UINT32(5, back.pointCount);
    TEST_ASSERT_EQUAL_INT32_ARRAY(lat, lat2, 5);
    TEST_ASSERT_EQUAL_INT32_ARRAY(lon, lon2, 5);
    TEST_ASSERT_EQUAL_UINT16(2, back.maneuverCount);
    TEST_ASSERT_EQUAL_UINT16(10, turns2[1].type);
    TEST_ASSERT_EQUAL_UINT32(3, turns2[1].beginIndex);
    TEST_ASSERT_EQUAL_STRING("Turn right onto Professional Drive.", turns2[1].instruction);
}

static void test_header_read_matches_full_read()
{
    const int32_t lat[] = {1, 2}, lon[] = {3, 4};
    const Header h = makeHeader(2, 0);
    uint8_t buf[256];
    const size_t n = RouteStore::encode(h, lat, lon, nullptr, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    Header listed{};
    TEST_ASSERT_TRUE(RouteStore::decodeHeader(buf, RouteStore::kHeaderBytes, listed)); // just the header's bytes
    TEST_ASSERT_EQUAL_STRING(h.name, listed.name);
    TEST_ASSERT_EQUAL_UINT32(2, listed.pointCount);
    TEST_ASSERT_FALSE(RouteStore::decodeHeader(buf, RouteStore::kHeaderBytes - 1, listed));
}

static void test_damage_is_refused()
{
    const int32_t lat[] = {100, 200, 300}, lon[] = {400, 500, 600};
    const Header h = makeHeader(3, 0);
    uint8_t buf[256];
    const size_t n = RouteStore::encode(h, lat, lon, nullptr, buf, sizeof(buf));
    Header back{};
    int32_t lat2[4], lon2[4];
    TEST_ASSERT_FALSE(RouteStore::decode(buf, n - 1, back, lat2, lon2, 4, nullptr, 0)); // cut short
    buf[RouteStore::kHeaderBytes + 1] ^= 0x10;                                          // a flipped bit
    TEST_ASSERT_FALSE(RouteStore::decode(buf, n, back, lat2, lon2, 4, nullptr, 0));
    TEST_ASSERT_FALSE(RouteStore::decode(buf, 3, back, lat2, lon2, 4, nullptr, 0));
}

static void test_antimeridian_deltas_wrap_back()
{
    const int32_t lat[] = {0, 0, 0}, lon[] = {1799999999, -1799999999, 1799999999};
    const Header h = makeHeader(3, 0);
    uint8_t buf[256];
    const size_t n = RouteStore::encode(h, lat, lon, nullptr, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    Header back{};
    int32_t lat2[4], lon2[4];
    TEST_ASSERT_TRUE(RouteStore::decode(buf, n, back, lat2, lon2, 4, nullptr, 0));
    TEST_ASSERT_EQUAL_INT32_ARRAY(lon, lon2, 3);
}

static void test_too_small_is_refused()
{
    int32_t lat[10], lon[10];
    for (int i = 0; i < 10; i++)
        lat[i] = lon[i] = i * 1000;
    const Header h = makeHeader(10, 0);
    uint8_t buf[256];
    TEST_ASSERT_EQUAL_size_t(0, RouteStore::encode(h, lat, lon, nullptr, buf, 20)); // the writer's buffer
    const size_t n = RouteStore::encode(h, lat, lon, nullptr, buf, sizeof(buf));
    Header back{};
    int32_t lat2[4], lon2[4];
    TEST_ASSERT_FALSE(RouteStore::decode(buf, n, back, lat2, lon2, 4, nullptr, 0)); // the reader's arrays
}

void setUp(void) {}
void tearDown(void) {}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();

    RUN_TEST(test_round_trip_is_exact);
    RUN_TEST(test_header_read_matches_full_read);
    RUN_TEST(test_damage_is_refused);
    RUN_TEST(test_antimeridian_deltas_wrap_back);
    RUN_TEST(test_too_small_is_refused);

    exit(UNITY_END());
}

void loop() {}
