// Covers src/graphics/niche/Map/MapRouteTiles.h: which map tiles Saved Routes > Download tiles fetches.
//
// Why it matters: this list is what the device will have to show once it is out of WiFi range. A gap along the route
// leaves a blank patch exactly where the user is driving; a walk that wanders off, or repeats itself, spends minutes
// of downloading - and a tile server's goodwill - on tiles nobody will see.
//
// Regressions guarded:
//   - A single point not getting its full neighbourhood, or a neighbourhood given twice.
//   - A long straight segment between two thinned points skipping the tiles in between.
//   - Zoom levels being dropped, or the walk not ending.
//   - A route across the antimeridian being walked the long way round the world.
//   - Rows past the poles being given.
//
// No feature guard: the header has no dependencies, so this runs on every native build.

// MeshTypes.h before TestUtil.h, per test/README.md - it pulls in Arduino.h, which the portduino runner links
// against. The header under test needs none of it.
#include "MeshTypes.h"
#include "TestUtil.h"
#include "graphics/niche/Map/MapRouteTiles.h"
#include <set>
#include <tuple>
#include <unity.h>

using NicheGraphics::MapTiles::RouteTileWalker;
using Tile = std::tuple<int, int32_t, int32_t>;

static std::multiset<Tile> walk(const int32_t *lat, const int32_t *lon, uint32_t count, std::initializer_list<uint8_t> zooms,
                                int radius)
{
    uint8_t zs[8];
    int n = 0;
    for (uint8_t z : zooms)
        zs[n++] = z;
    RouteTileWalker w;
    w.start(lat, lon, count, zs, n, radius);
    std::multiset<Tile> tiles;
    int z;
    int32_t x, y;
    for (int guard = 0; guard < 100000 && w.next(z, x, y); guard++)
        tiles.insert(Tile{z, x, y});
    return tiles;
}

static void test_single_point_gets_its_neighbourhood_once()
{
    const int32_t lat[] = {389407716}, lon[] = {-1211008497};
    const auto tiles = walk(lat, lon, 1, {14}, 1);
    TEST_ASSERT_EQUAL_size_t(9, tiles.size());
    TEST_ASSERT_EQUAL_size_t(9, std::set<Tile>(tiles.begin(), tiles.end()).size());
    double fx, fy;
    RouteTileWalker::tileOf(lat[0], lon[0], 14, fx, fy);
    TEST_ASSERT_EQUAL_size_t(1, tiles.count(Tile{14, (int32_t)fx, (int32_t)fy}));
    TEST_ASSERT_EQUAL_size_t(1, tiles.count(Tile{14, (int32_t)fx + 1, (int32_t)fy + 1}));
}

static void test_long_segment_skips_no_tile()
{
    // Two points about 6 tiles apart at z12, due east: every tile between them must be there.
    const int32_t lat[] = {389000000, 389000000}, lon[] = {-1215000000, -1210000000};
    const auto tiles = walk(lat, lon, 2, {12}, 0);
    double ax, ay, bx, by;
    RouteTileWalker::tileOf(lat[0], lon[0], 12, ax, ay);
    RouteTileWalker::tileOf(lat[1], lon[1], 12, bx, by);
    TEST_ASSERT_TRUE((int)bx - (int)ax >= 5);
    for (int32_t x = (int32_t)ax; x <= (int32_t)bx; x++)
        TEST_ASSERT_EQUAL_size_t_MESSAGE(1, tiles.count(Tile{12, x, (int32_t)ay}), "a tile along the segment");
    TEST_ASSERT_EQUAL_size_t((size_t)((int32_t)bx - (int32_t)ax + 1), tiles.size()); // and nothing off it
}

static void test_every_zoom_is_walked()
{
    const int32_t lat[] = {389000000, 389100000, 389200000}, lon[] = {-1210000000, -1210500000, -1211000000};
    const auto tiles = walk(lat, lon, 3, {12, 13, 14, 15, 16}, 1);
    for (int z = 12; z <= 16; z++) {
        bool found = false;
        for (const Tile &t : tiles)
            found |= std::get<0>(t) == z;
        TEST_ASSERT_TRUE_MESSAGE(found, "a zoom level with no tiles");
    }
    // A short route: the recent-tile memory is enough that nothing repeats
    TEST_ASSERT_EQUAL_size_t(std::set<Tile>(tiles.begin(), tiles.end()).size(), tiles.size());
}

static void test_antimeridian_is_crossed_the_short_way()
{
    const int32_t lat[] = {0, 0}, lon[] = {1799000000, -1799000000};
    const auto tiles = walk(lat, lon, 2, {10}, 0);
    TEST_ASSERT_TRUE(tiles.size() <= 3); // a couple of tiles, not a trip round the world
    for (const Tile &t : tiles) {
        const int32_t x = std::get<1>(t);
        TEST_ASSERT_TRUE(x == 0 || x == 1023);
    }
}

static void test_no_rows_past_the_poles()
{
    const int32_t lat[] = {850000000}, lon[] = {0};
    const auto tiles = walk(lat, lon, 1, {3}, 1);
    for (const Tile &t : tiles)
        TEST_ASSERT_TRUE(std::get<2>(t) >= 0 && std::get<2>(t) < 8);
    TEST_ASSERT_EQUAL_size_t(6, tiles.size()); // the top row's neighbours above it don't exist
}

static void test_empty_route_walks_nothing()
{
    const auto tiles = walk(nullptr, nullptr, 0, {12}, 1);
    TEST_ASSERT_EQUAL_size_t(0, tiles.size());
}

void setUp(void) {}
void tearDown(void) {}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();

    RUN_TEST(test_single_point_gets_its_neighbourhood_once);
    RUN_TEST(test_long_segment_skips_no_tile);
    RUN_TEST(test_every_zoom_is_walked);
    RUN_TEST(test_antimeridian_is_crossed_the_short_way);
    RUN_TEST(test_no_rows_past_the_poles);
    RUN_TEST(test_empty_route_walks_nothing);

    exit(UNITY_END());
}

void loop() {}
