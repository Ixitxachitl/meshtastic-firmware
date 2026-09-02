// Covers the tile-index blob format's zoom-range table: validation, and the index arithmetic the
// two readers (MapTileSourceFile/MapTileSourceSD) build every tile lookup on.
//
// Worth testing here rather than on-device because the failure mode is silent. A range table that
// validates but whose arithmetic is off doesn't crash or log - it returns a wrong tile index, and
// the map just draws the wrong piece of the world. The readers themselves need a filesystem and a
// real MAP.BIN, but MapTileBlobFormat.h is a dependency-free header (stddef/stdint only), so the
// part that can silently corrupt a render is exactly the part that tests cheaply here.
//
// No feature guard: unlike the readers, this header isn't behind BASEUI_HAS_MAP, so this suite
// runs on every native build rather than compiling away to an empty pass.

// MeshTypes.h before TestUtil.h, per test/README.md - it's what pulls in Arduino.h, whose
// extern "C" declarations of setup()/loop() the portduino test runner links against. The header
// under test needs none of it (stddef/stdint only), so without this the suite compiles but
// fails to link.
#include "MeshTypes.h"
#include "TestUtil.h"
#include "graphics/niche/Map/MapTileBlobFormat.h"
#include <cstdio>
#include <unity.h>

using namespace NicheGraphics::MapTiles;

// The shape a "bake one region at each of a few zooms" file has: one range per zoom, ascending.
static const TileBlobZoomRange kSingleRegionPerZoom[] = {
    {8, 100, 200, 2, 2},  // 4 tiles, base 0
    {9, 200, 400, 3, 1},  // 3 tiles, base 4
    {10, 400, 800, 3, 2}, // 6 tiles, base 7
};
static constexpr int kSingleRegionCount = 3;
static constexpr uint32_t kSingleRegionTiles = 4 + 3 + 6;

// Two disjoint regions sharing z10 - the multi-region case. Deliberately ordered so the second
// z10 range is neither first nor last in the table.
static const TileBlobZoomRange kTwoRegionsAtZ10[] = {
    {8, 100, 200, 2, 2},  // 4 tiles, base 0
    {10, 400, 800, 3, 2}, // 6 tiles, base 4
    {10, 500, 900, 2, 3}, // 6 tiles, base 10
    {11, 50, 60, 1, 1},   // 1 tile,  base 16
};
static constexpr int kTwoRegionsCount = 4;
static constexpr uint32_t kTwoRegionsTiles = 4 + 6 + 6 + 1;

// Every tile index in a table must decode back to coordinates that map to that same index -
// the property both readers depend on, since indexOf() picks the entry to seek to and
// tileZoomAt/TxAt/TyAt name the tile a given entry holds.
static void assertIndexRoundTrips(const TileBlobZoomRange *ranges, int count, uint32_t total)
{
    for (uint32_t i = 0; i < total; i++) {
        const int zoom = tileBlobZoomAt(ranges, count, (int)i);
        const int tx = tileBlobTxAt(ranges, count, (int)i);
        const int ty = tileBlobTyAt(ranges, count, (int)i);
        TEST_ASSERT_EQUAL_INT((int)i, tileBlobIndexOf(ranges, count, total, zoom, tx, ty));
    }
}

// --- Validation ---

void test_validate_accepts_one_range_per_zoom()
{
    TEST_ASSERT_TRUE(validateTileBlobZoomRanges(kSingleRegionPerZoom, kSingleRegionCount, kSingleRegionTiles));
}

void test_validate_accepts_disjoint_ranges_sharing_a_zoom()
{
    TEST_ASSERT_TRUE(validateTileBlobZoomRanges(kTwoRegionsAtZ10, kTwoRegionsCount, kTwoRegionsTiles));
}

void test_validate_accepts_touching_ranges_sharing_a_zoom()
{
    // Abutting edge-to-edge is not overlapping: [400,403) then [403,405).
    const TileBlobZoomRange touching[] = {{10, 400, 800, 3, 2}, {10, 403, 800, 2, 2}};
    TEST_ASSERT_TRUE(validateTileBlobZoomRanges(touching, 2, 6 + 4));
}

void test_validate_rejects_overlapping_ranges_sharing_a_zoom()
{
    // Overlap makes a tile's index ambiguous - two ranges would both claim (402, 801).
    const TileBlobZoomRange overlapping[] = {{10, 400, 800, 3, 2}, {10, 402, 801, 2, 2}};
    TEST_ASSERT_FALSE(validateTileBlobZoomRanges(overlapping, 2, 6 + 4));
}

void test_validate_rejects_ranges_overlapping_on_one_axis_only()
{
    // Shares an x span but no y span, and vice versa - neither is an overlap.
    const TileBlobZoomRange xOnly[] = {{10, 400, 800, 3, 2}, {10, 400, 900, 3, 2}};
    TEST_ASSERT_TRUE(validateTileBlobZoomRanges(xOnly, 2, 6 + 6));
    const TileBlobZoomRange yOnly[] = {{10, 400, 800, 3, 2}, {10, 500, 800, 3, 2}};
    TEST_ASSERT_TRUE(validateTileBlobZoomRanges(yOnly, 2, 6 + 6));
}

void test_validate_ignores_overlap_across_different_zooms()
{
    // Identical rectangles at different zooms address different tiles entirely.
    const TileBlobZoomRange sameRectDifferentZooms[] = {{9, 100, 200, 2, 2}, {10, 100, 200, 2, 2}};
    TEST_ASSERT_TRUE(validateTileBlobZoomRanges(sameRectDifferentZooms, 2, 4 + 4));
}

void test_validate_rejects_descending_zoom()
{
    // Non-decreasing is what keeps a zoom's ranges adjacent, which the overlap scan relies on.
    const TileBlobZoomRange descending[] = {{10, 400, 800, 1, 1}, {8, 100, 200, 1, 1}};
    TEST_ASSERT_FALSE(validateTileBlobZoomRanges(descending, 2, 2));
}

void test_validate_rejects_non_adjacent_repeat_of_a_zoom()
{
    // Same zoom split by another zoom: the disjointness scan stops at the zoom change, so this
    // ordering could sneak an overlapping pair past it - the ordering check must catch it first.
    const TileBlobZoomRange interleaved[] = {{10, 400, 800, 1, 1}, {11, 50, 60, 1, 1}, {10, 400, 800, 1, 1}};
    TEST_ASSERT_FALSE(validateTileBlobZoomRanges(interleaved, 3, 3));
}

void test_validate_rejects_area_mismatch()
{
    TEST_ASSERT_FALSE(validateTileBlobZoomRanges(kTwoRegionsAtZ10, kTwoRegionsCount, kTwoRegionsTiles - 1));
    TEST_ASSERT_FALSE(validateTileBlobZoomRanges(kTwoRegionsAtZ10, kTwoRegionsCount, kTwoRegionsTiles + 1));
    TEST_ASSERT_FALSE(validateTileBlobZoomRanges(kTwoRegionsAtZ10, kTwoRegionsCount, 0));
}

void test_validate_rejects_empty_rectangles()
{
    const TileBlobZoomRange zeroWidth[] = {{10, 400, 800, 0, 2}};
    TEST_ASSERT_FALSE(validateTileBlobZoomRanges(zeroWidth, 1, 0));
    const TileBlobZoomRange zeroHeight[] = {{10, 400, 800, 2, 0}};
    TEST_ASSERT_FALSE(validateTileBlobZoomRanges(zeroHeight, 1, 0));
}

void test_validate_rejects_rectangle_outside_its_zoom_grid()
{
    // z2 is a 4x4 grid, so x 3..4 runs off the east edge.
    const TileBlobZoomRange offGrid[] = {{2, 3, 0, 2, 1}};
    TEST_ASSERT_FALSE(validateTileBlobZoomRanges(offGrid, 1, 2));
    const TileBlobZoomRange originOffGrid[] = {{2, 4, 0, 1, 1}};
    TEST_ASSERT_FALSE(validateTileBlobZoomRanges(originOffGrid, 1, 1));
    // Exactly filling the grid is fine.
    const TileBlobZoomRange wholeGrid[] = {{2, 0, 0, 4, 4}};
    TEST_ASSERT_TRUE(validateTileBlobZoomRanges(wholeGrid, 1, 16));
}

void test_validate_rejects_implausible_zoom()
{
    // Guards the 1u << zoom below it from becoming a shift count >= 32.
    const TileBlobZoomRange tooDeep[] = {{kTileBlobMaxPlausibleZoom + 1, 0, 0, 1, 1}};
    TEST_ASSERT_FALSE(validateTileBlobZoomRanges(tooDeep, 1, 1));
    const TileBlobZoomRange deepestAllowed[] = {{kTileBlobMaxPlausibleZoom, 0, 0, 1, 1}};
    TEST_ASSERT_TRUE(validateTileBlobZoomRanges(deepestAllowed, 1, 1));
}

void test_validate_rejects_implausible_range_count()
{
    TEST_ASSERT_FALSE(validateTileBlobZoomRanges(kSingleRegionPerZoom, 0, kSingleRegionTiles));
    TEST_ASSERT_FALSE(validateTileBlobZoomRanges(kSingleRegionPerZoom, -1, kSingleRegionTiles));
    TEST_ASSERT_FALSE(validateTileBlobZoomRanges(kSingleRegionPerZoom, kTileBlobMaxZoomRanges + 1, kSingleRegionTiles));
}

void test_validate_accepts_a_full_table_of_same_zoom_ranges()
{
    // The wire format's u8 count ceiling, all at one zoom - the worst case for the O(n^2)
    // disjointness scan, and the shape the baker's own 255-range limit allows.
    static TileBlobZoomRange many[kTileBlobMaxZoomRanges];
    for (int i = 0; i < kTileBlobMaxZoomRanges; i++)
        many[i] = {10, (uint16_t)(i * 2), 800, 1, 1}; // one tile each, two apart: never touching
    TEST_ASSERT_TRUE(validateTileBlobZoomRanges(many, kTileBlobMaxZoomRanges, kTileBlobMaxZoomRanges));

    // One duplicate anywhere in that table must still be caught.
    many[kTileBlobMaxZoomRanges - 1] = many[0];
    TEST_ASSERT_FALSE(validateTileBlobZoomRanges(many, kTileBlobMaxZoomRanges, kTileBlobMaxZoomRanges));
}

// --- Index arithmetic ---

void test_index_of_one_range_per_zoom()
{
    const TileBlobZoomRange *r = kSingleRegionPerZoom;
    // First tile of each range lands on that range's base.
    TEST_ASSERT_EQUAL_INT(0, tileBlobIndexOf(r, kSingleRegionCount, kSingleRegionTiles, 8, 100, 200));
    TEST_ASSERT_EQUAL_INT(4, tileBlobIndexOf(r, kSingleRegionCount, kSingleRegionTiles, 9, 200, 400));
    TEST_ASSERT_EQUAL_INT(7, tileBlobIndexOf(r, kSingleRegionCount, kSingleRegionTiles, 10, 400, 800));
    // Raster order within a rectangle: tx fastest, then ty.
    TEST_ASSERT_EQUAL_INT(1, tileBlobIndexOf(r, kSingleRegionCount, kSingleRegionTiles, 8, 101, 200));
    TEST_ASSERT_EQUAL_INT(2, tileBlobIndexOf(r, kSingleRegionCount, kSingleRegionTiles, 8, 100, 201));
    TEST_ASSERT_EQUAL_INT(12, tileBlobIndexOf(r, kSingleRegionCount, kSingleRegionTiles, 10, 402, 801));
}

void test_index_of_reaches_a_later_range_at_the_same_zoom()
{
    // The multi-region case: matching on zoom alone would stop at the first z10 range and miss
    // every tile of the second, which is what a single-region-per-zoom reader does.
    const TileBlobZoomRange *r = kTwoRegionsAtZ10;
    TEST_ASSERT_EQUAL_INT(10, tileBlobIndexOf(r, kTwoRegionsCount, kTwoRegionsTiles, 10, 500, 900));
    TEST_ASSERT_EQUAL_INT(11, tileBlobIndexOf(r, kTwoRegionsCount, kTwoRegionsTiles, 10, 501, 900));
    TEST_ASSERT_EQUAL_INT(15, tileBlobIndexOf(r, kTwoRegionsCount, kTwoRegionsTiles, 10, 501, 902));
    // The first z10 range still resolves correctly alongside it.
    TEST_ASSERT_EQUAL_INT(4, tileBlobIndexOf(r, kTwoRegionsCount, kTwoRegionsTiles, 10, 400, 800));
    TEST_ASSERT_EQUAL_INT(9, tileBlobIndexOf(r, kTwoRegionsCount, kTwoRegionsTiles, 10, 402, 801));
    // Ranges on either side are unaffected by the repeat.
    TEST_ASSERT_EQUAL_INT(0, tileBlobIndexOf(r, kTwoRegionsCount, kTwoRegionsTiles, 8, 100, 200));
    TEST_ASSERT_EQUAL_INT(16, tileBlobIndexOf(r, kTwoRegionsCount, kTwoRegionsTiles, 11, 50, 60));
}

void test_index_of_rejects_tiles_outside_every_range()
{
    const TileBlobZoomRange *r = kTwoRegionsAtZ10;
    // The gap between the two z10 regions - present at that zoom, but in neither rectangle. This
    // is the case that must not resolve to a neighbouring region's tile.
    TEST_ASSERT_EQUAL_INT(-1, tileBlobIndexOf(r, kTwoRegionsCount, kTwoRegionsTiles, 10, 450, 850));
    TEST_ASSERT_EQUAL_INT(-1, tileBlobIndexOf(r, kTwoRegionsCount, kTwoRegionsTiles, 10, 403, 800)); // one east of range 1
    TEST_ASSERT_EQUAL_INT(-1, tileBlobIndexOf(r, kTwoRegionsCount, kTwoRegionsTiles, 10, 399, 800)); // one west of range 1
    TEST_ASSERT_EQUAL_INT(-1, tileBlobIndexOf(r, kTwoRegionsCount, kTwoRegionsTiles, 10, 400, 802)); // one south of range 1
    TEST_ASSERT_EQUAL_INT(-1, tileBlobIndexOf(r, kTwoRegionsCount, kTwoRegionsTiles, 10, 400, 799)); // one north of range 1
    // A zoom with no ranges at all.
    TEST_ASSERT_EQUAL_INT(-1, tileBlobIndexOf(r, kTwoRegionsCount, kTwoRegionsTiles, 9, 400, 800));
    // An empty table.
    TEST_ASSERT_EQUAL_INT(-1, tileBlobIndexOf(r, 0, 0, 10, 400, 800));
}

void test_index_of_refuses_to_exceed_the_files_tile_count()
{
    // A range table claiming more tiles than the file holds must not hand back an index the
    // caller would seek past the end of the tile-entry table with.
    TEST_ASSERT_EQUAL_INT(-1, tileBlobIndexOf(kTwoRegionsAtZ10, kTwoRegionsCount, 5, 10, 501, 902));
}

void test_index_round_trips_across_every_tile()
{
    assertIndexRoundTrips(kSingleRegionPerZoom, kSingleRegionCount, kSingleRegionTiles);
    assertIndexRoundTrips(kTwoRegionsAtZ10, kTwoRegionsCount, kTwoRegionsTiles);
}

void test_index_round_trips_across_a_dense_pyramid()
{
    // The classic worldwide bake: every zoom is its full 2^zoom grid, z0..z4.
    static TileBlobZoomRange pyramid[5];
    uint32_t total = 0;
    for (int z = 0; z <= 4; z++) {
        const uint16_t side = (uint16_t)(1u << z);
        pyramid[z] = {(uint8_t)z, 0, 0, side, side};
        total += (uint32_t)side * side;
    }
    TEST_ASSERT_TRUE(validateTileBlobZoomRanges(pyramid, 5, total));
    assertIndexRoundTrips(pyramid, 5, total);
}

// --- Distinct zooms (what zoomCount()/zoomAt() report) ---

void test_distinct_zooms_collapses_repeats()
{
    uint8_t zooms[kTileBlobMaxDistinctZooms] = {};
    TEST_ASSERT_EQUAL_INT(3, tileBlobDistinctZooms(kTwoRegionsAtZ10, kTwoRegionsCount, zooms));
    TEST_ASSERT_EQUAL_UINT8(8, zooms[0]);
    TEST_ASSERT_EQUAL_UINT8(10, zooms[1]); // once, not twice
    TEST_ASSERT_EQUAL_UINT8(11, zooms[2]);
}

void test_distinct_zooms_passes_through_already_distinct_tables()
{
    uint8_t zooms[kTileBlobMaxDistinctZooms] = {};
    TEST_ASSERT_EQUAL_INT(3, tileBlobDistinctZooms(kSingleRegionPerZoom, kSingleRegionCount, zooms));
    TEST_ASSERT_EQUAL_UINT8(8, zooms[0]);
    TEST_ASSERT_EQUAL_UINT8(9, zooms[1]);
    TEST_ASSERT_EQUAL_UINT8(10, zooms[2]);
    TEST_ASSERT_EQUAL_INT(0, tileBlobDistinctZooms(kSingleRegionPerZoom, 0, zooms));
}

void test_distinct_zooms_never_overruns_its_output_buffer()
{
    // The output is sized one slot per plausible zoom; a full 255-range table all at one zoom
    // must still write exactly one.
    static TileBlobZoomRange many[kTileBlobMaxZoomRanges];
    for (int i = 0; i < kTileBlobMaxZoomRanges; i++)
        many[i] = {10, (uint16_t)(i * 2), 800, 1, 1};
    uint8_t zooms[kTileBlobMaxDistinctZooms] = {};
    TEST_ASSERT_EQUAL_INT(1, tileBlobDistinctZooms(many, kTileBlobMaxZoomRanges, zooms));

    // And one range per plausible zoom fills it exactly.
    static TileBlobZoomRange everyZoom[kTileBlobMaxDistinctZooms];
    for (int z = 0; z < kTileBlobMaxDistinctZooms; z++)
        everyZoom[z] = {(uint8_t)z, 0, 0, 1, 1};
    TEST_ASSERT_EQUAL_INT(kTileBlobMaxDistinctZooms, tileBlobDistinctZooms(everyZoom, kTileBlobMaxDistinctZooms, zooms));
}

// --- Record decoding ---

void test_decode_zoom_range_record()
{
    // 9 bytes little-endian: u8 zoom, u16 xMin, u16 yMin, u16 width, u16 height.
    const uint8_t buf[kTileBlobZoomRangeEntrySize] = {10, 0x90, 0x01, 0x20, 0x03, 0x03, 0x00, 0x02, 0x00};
    TileBlobZoomRange r{};
    decodeTileBlobZoomRange(buf, r);
    TEST_ASSERT_EQUAL_UINT8(10, r.zoom);
    TEST_ASSERT_EQUAL_UINT16(400, r.xMin);
    TEST_ASSERT_EQUAL_UINT16(800, r.yMin);
    TEST_ASSERT_EQUAL_UINT16(3, r.width);
    TEST_ASSERT_EQUAL_UINT16(2, r.height);
}

void test_decode_tile_entry_record()
{
    // 12 bytes: u8 zoom, u16 tx, u16 ty, u8 kind, u32 offset, u16 size.
    const uint8_t buf[kTileBlobEntrySize] = {10, 0x90, 0x01, 0x20, 0x03, 2, 0x78, 0x56, 0x34, 0x12, 0xFF, 0xFF};
    TileBlobEntry e{};
    decodeTileBlobEntry(buf, e);
    TEST_ASSERT_EQUAL_UINT8(10, e.zoom);
    TEST_ASSERT_EQUAL_UINT16(400, e.tx);
    TEST_ASSERT_EQUAL_UINT16(800, e.ty);
    TEST_ASSERT_EQUAL_UINT8(2, e.kind);
    TEST_ASSERT_EQUAL_UINT32(0x12345678, e.offset);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, e.size);
}

void test_little_endian_readers_handle_high_bits()
{
    const uint8_t u16Max[] = {0xFF, 0xFF};
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, readTileBlobU16LE(u16Max));
    const uint8_t u32Max[] = {0xFF, 0xFF, 0xFF, 0xFF};
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, readTileBlobU32LE(u32Max));
    // 'MTL2' as it sits in the file, versus the magic constant the readers compare against.
    const uint8_t magic[] = {'M', 'T', 'L', '2'};
    TEST_ASSERT_EQUAL_UINT32(kTileBlobMagic, readTileBlobU32LE(magic));
}

// --- Unity lifecycle ---

void setUp(void) {}
void tearDown(void) {}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();

    printf("\n=== Zoom-range table validation ===\n");
    RUN_TEST(test_validate_accepts_one_range_per_zoom);
    RUN_TEST(test_validate_accepts_disjoint_ranges_sharing_a_zoom);
    RUN_TEST(test_validate_accepts_touching_ranges_sharing_a_zoom);
    RUN_TEST(test_validate_rejects_overlapping_ranges_sharing_a_zoom);
    RUN_TEST(test_validate_rejects_ranges_overlapping_on_one_axis_only);
    RUN_TEST(test_validate_ignores_overlap_across_different_zooms);
    RUN_TEST(test_validate_rejects_descending_zoom);
    RUN_TEST(test_validate_rejects_non_adjacent_repeat_of_a_zoom);
    RUN_TEST(test_validate_rejects_area_mismatch);
    RUN_TEST(test_validate_rejects_empty_rectangles);
    RUN_TEST(test_validate_rejects_rectangle_outside_its_zoom_grid);
    RUN_TEST(test_validate_rejects_implausible_zoom);
    RUN_TEST(test_validate_rejects_implausible_range_count);
    RUN_TEST(test_validate_accepts_a_full_table_of_same_zoom_ranges);

    printf("\n=== Tile index arithmetic ===\n");
    RUN_TEST(test_index_of_one_range_per_zoom);
    RUN_TEST(test_index_of_reaches_a_later_range_at_the_same_zoom);
    RUN_TEST(test_index_of_rejects_tiles_outside_every_range);
    RUN_TEST(test_index_of_refuses_to_exceed_the_files_tile_count);
    RUN_TEST(test_index_round_trips_across_every_tile);
    RUN_TEST(test_index_round_trips_across_a_dense_pyramid);

    printf("\n=== Distinct zoom levels ===\n");
    RUN_TEST(test_distinct_zooms_collapses_repeats);
    RUN_TEST(test_distinct_zooms_passes_through_already_distinct_tables);
    RUN_TEST(test_distinct_zooms_never_overruns_its_output_buffer);

    printf("\n=== Record decoding ===\n");
    RUN_TEST(test_decode_zoom_range_record);
    RUN_TEST(test_decode_tile_entry_record);
    RUN_TEST(test_little_endian_readers_handle_high_bits);

    exit(UNITY_END());
}

void loop() {}
