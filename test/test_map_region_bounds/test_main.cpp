// Covers MapRegionBounds::regionBounds() and MapRegionBounds::fit() in src/graphics/draw/MapRegionBounds.h: the
// view the BaseUI Map frame falls back to when there is no live fix, no saved home and no positioned node.
//
// Why it is required: with nothing positioned, the frame used to show only "No node positions yet". It now
// frames the LoRa region the radio is set for (config.lora.region), so the fallback has to know every region
// code, and has to frame each one correctly in Web Mercator at whatever viewport the board has.
//
// Regressions guarded:
//   - A region code added to the protobufs with no entry here. regionBounds() returns false, and devices in
//     that region silently get the empty-frame text again - nothing else would notice.
//   - A box with south/north or west/east swapped, or out of range, which fit() would turn into a world view
//     or a view of the wrong place.
//   - fit() picking a zoom that overflows the viewport, or one step further out than it needs to.
//   - fit() centring on the degree midpoint: in Mercator that sits visibly below the middle of a
//     high-latitude box like Europe or Russia, cutting off its north.
//
// No feature guard: the header has no BASEUI_HAS_MAP gate, so this runs on every native build.

// MeshTypes.h before TestUtil.h, per test/README.md - it pulls in Arduino.h, which the portduino runner links
// against.
#include "MeshTypes.h"
#include "TestUtil.h"
#include "graphics/draw/MapRegionBounds.h"
#include <cmath>
#include <cstdio>
#include <unity.h>

using namespace graphics::MapRegionBounds;

static constexpr int kMinZoom = 0;
static constexpr int kMaxZoom = 18; // MapRenderer::kMinZoom/kMaxZoom

// Map viewports worth covering: the M9's landscape panel under the header, a portrait panel, a tiny one.
static constexpr int kViewports[][2] = {{320, 219}, {240, 299}, {128, 53}};

static bool fitsAt(const Bounds &b, int zoom, int viewWidth, int viewHeight)
{
    const double worldPx = 256.0 * (double)(1u << zoom);
    const double width = (b.east - b.west) / 360.0 * worldPx;
    const double height = (mercatorY(b.south) - mercatorY(b.north)) * worldPx;
    return width <= viewWidth * kFitFraction && height <= viewHeight * kFitFraction;
}

static meshtastic_Config_LoRaConfig_RegionCode regionAt(int code)
{
    return static_cast<meshtastic_Config_LoRaConfig_RegionCode>(code);
}

static void test_every_region_code_has_bounds()
{
    for (int code = _meshtastic_Config_LoRaConfig_RegionCode_MIN; code <= _meshtastic_Config_LoRaConfig_RegionCode_MAX; code++) {
        if (code == meshtastic_Config_LoRaConfig_RegionCode_UNSET)
            continue;
        Bounds b;
        char message[48];
        snprintf(message, sizeof(message), "region code %d has no bounds", code);
        TEST_ASSERT_TRUE_MESSAGE(regionBounds(regionAt(code), b), message);
    }
}

static void test_unset_has_no_bounds()
{
    Bounds b;
    TEST_ASSERT_FALSE(regionBounds(meshtastic_Config_LoRaConfig_RegionCode_UNSET, b));
}

static void test_every_box_is_well_formed()
{
    for (int code = _meshtastic_Config_LoRaConfig_RegionCode_MIN; code <= _meshtastic_Config_LoRaConfig_RegionCode_MAX; code++) {
        Bounds b;
        if (!regionBounds(regionAt(code), b))
            continue;
        char message[48];
        snprintf(message, sizeof(message), "region code %d", code);
        TEST_ASSERT_TRUE_MESSAGE(b.south < b.north, message);
        TEST_ASSERT_TRUE_MESSAGE(b.west < b.east, message);
        TEST_ASSERT_TRUE_MESSAGE(b.south >= -kMaxMercatorLat && b.north <= kMaxMercatorLat, message);
        TEST_ASSERT_TRUE_MESSAGE(b.west >= -180.0f && b.east <= 180.0f, message);
    }
}

static void test_fit_is_the_highest_zoom_that_fits()
{
    for (int code = _meshtastic_Config_LoRaConfig_RegionCode_MIN; code <= _meshtastic_Config_LoRaConfig_RegionCode_MAX; code++) {
        Bounds b;
        if (!regionBounds(regionAt(code), b))
            continue;
        for (const auto &viewport : kViewports) {
            const View v = fit(b, viewport[0], viewport[1], kMinZoom, kMaxZoom);
            char message[64];
            snprintf(message, sizeof(message), "region code %d at %dx%d, zoom %d", code, viewport[0], viewport[1], v.zoom);
            TEST_ASSERT_TRUE_MESSAGE(v.zoom >= kMinZoom && v.zoom <= kMaxZoom, message);
            // The minimum zoom is the floor for a box too big for any zoom (the whole world); it need not fit.
            if (v.zoom > kMinZoom)
                TEST_ASSERT_TRUE_MESSAGE(fitsAt(b, v.zoom, viewport[0], viewport[1]), message);
            if (v.zoom < kMaxZoom)
                TEST_ASSERT_FALSE_MESSAGE(fitsAt(b, v.zoom + 1, viewport[0], viewport[1]), message);
        }
    }
}

static void test_fit_centres_on_the_visual_middle()
{
    Bounds europe;
    TEST_ASSERT_TRUE(regionBounds(meshtastic_Config_LoRaConfig_RegionCode_EU_868, europe));
    const View v = fit(europe, 320, 219, kMinZoom, kMaxZoom);

    const double visualMiddle = (mercatorY(europe.north) + mercatorY(europe.south)) / 2.0;
    // Unity is built without double precision here, so the comparison is spelled out.
    TEST_ASSERT_TRUE(std::fabs(mercatorY(v.centerLat) - visualMiddle) < 1e-6);
    // At these latitudes the visual middle is well north of the degree midpoint (53.0).
    TEST_ASSERT_TRUE(v.centerLat > (europe.south + europe.north) / 2.0f + 2.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, (europe.west + europe.east) / 2.0f, v.centerLng);
}

static void test_small_regions_zoom_in_further_than_large_ones()
{
    Bounds singapore, russia;
    TEST_ASSERT_TRUE(regionBounds(meshtastic_Config_LoRaConfig_RegionCode_SG_923, singapore));
    TEST_ASSERT_TRUE(regionBounds(meshtastic_Config_LoRaConfig_RegionCode_RU, russia));
    TEST_ASSERT_GREATER_THAN(fit(russia, 320, 219, kMinZoom, kMaxZoom).zoom, fit(singapore, 320, 219, kMinZoom, kMaxZoom).zoom);
}

void setUp(void) {}
void tearDown(void) {}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();

    printf("\n=== Region table ===\n");
    RUN_TEST(test_every_region_code_has_bounds);
    RUN_TEST(test_unset_has_no_bounds);
    RUN_TEST(test_every_box_is_well_formed);

    printf("\n=== Fit ===\n");
    RUN_TEST(test_fit_is_the_highest_zoom_that_fits);
    RUN_TEST(test_fit_centres_on_the_visual_middle);
    RUN_TEST(test_small_regions_zoom_in_further_than_large_ones);

    exit(UNITY_END());
}

void loop() {}
