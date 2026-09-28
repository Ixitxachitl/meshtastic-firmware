// Covers src/graphics/niche/Map/MapGeocodeParse.h: building the address-search URL for the map's Navigate > Address
// and reading the places out of a Nominatim reply.
//
// Why it matters: the query is whatever someone typed, and the reply comes off the network, so neither is trusted.
// The URL template can also come from a hand-written card file. A reply misread into the wrong coordinates would
// set a navigation target somewhere plausible but wrong, and a parser that overruns on a long or odd reply would
// take the device down mid-search.
//
// Regressions guarded:
//   - Reserved and non-ASCII characters in a query reaching the URL unencoded, or an overflow leaving half a URL.
//   - A template without {q} or with a non-http scheme being used anyway.
//   - The real Nominatim reply shape (string coordinates, nested arrays, escapes, UTF-8) not parsing, or places
//     without a usable position being returned as if they had one.
//   - Malformed replies being partly accepted instead of refused, and long names cutting a UTF-8 character in half.
//
// No feature guard: the header has no dependencies, so this runs on every native build.

// MeshTypes.h before TestUtil.h, per test/README.md - it pulls in Arduino.h, which the portduino runner links
// against. The header under test needs none of it.
#include "MeshTypes.h"
#include "TestUtil.h"
#include "graphics/niche/Map/MapGeocodeParse.h"
#include <cmath>
#include <cstring>
#include <unity.h>

using namespace NicheGraphics::MapTiles::Geocode;

// This Unity build has double support off, so compare doubles by hand to keep the full precision under test.
#define EXPECT_NEAR(expected, actual, tol, msg)                                                                                  \
    TEST_ASSERT_TRUE_MESSAGE(fabs((double)(expected) - (double)(actual)) <= (tol), msg)

static void test_encodeQuery_escapes_reserved_and_utf8()
{
    char out[128];
    TEST_ASSERT_TRUE(encodeQuery("12 Main St, Grass Valley", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("12%20Main%20St%2C%20Grass%20Valley", out);

    TEST_ASSERT_TRUE(encodeQuery("a&b=c?d#e/f", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("a%26b%3Dc%3Fd%23e%2Ff", out);

    TEST_ASSERT_TRUE(encodeQuery("M\xC3\xBCnchen", out, sizeof(out))); // München
    TEST_ASSERT_EQUAL_STRING("M%C3%BCnchen", out);

    TEST_ASSERT_TRUE(encodeQuery("keep-._~", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("keep-._~", out);
}

static void test_encodeQuery_overflow_fails_closed()
{
    char out[8];
    TEST_ASSERT_FALSE(encodeQuery("a b c d", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_TRUE(encodeQuery("abcdefg", out, sizeof(out)));
    TEST_ASSERT_FALSE(encodeQuery("abcdefgh", out, sizeof(out)));
}

static void test_expandSearchUrl_default_template()
{
    char out[256];
    TEST_ASSERT_TRUE(expandSearchUrl(kDefaultSearchUrl, "Grass%20Valley", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://nominatim.openstreetmap.org/search?format=jsonv2&limit=5&q=Grass%20Valley", out);
}

static void test_expandSearchUrl_fails_closed()
{
    char out[64];
    TEST_ASSERT_FALSE(expandSearchUrl("https://host/search?format=jsonv2", "x", out, sizeof(out))); // no {q}
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_FALSE(expandSearchUrl("ftp://host/?q={q}", "x", out, sizeof(out)));
    TEST_ASSERT_FALSE(
        expandSearchUrl("https://host/?q={q}", "a-very-long-query-that-does-not-fit-in-the-buffer-at-all", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_TRUE(expandSearchUrl("http://host/?q={q}&again={q}", "x", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("http://host/?q=x&again=x", out);
}

// Trimmed from a real jsonv2 reply: string coordinates, a nested boundingbox array, a slash escape.
static const char kNominatimReply[] =
    "[{\"place_id\":123,\"licence\":\"Data \\u00a9 OpenStreetMap contributors, ODbL 1.0. "
    "http:\\/\\/osm.org\\/copyright\",\"osm_type\":\"relation\",\"lat\":\"39.2190607\",\"lon\":\"-121.0610606\","
    "\"category\":\"boundary\",\"place_rank\":16,\"importance\":0.52,"
    "\"display_name\":\"Grass Valley, Nevada County, California, United States\","
    "\"boundingbox\":[\"39.1928\",\"39.2372\",\"-121.0851\",\"-121.0282\"]},"
    " {\"lat\":\"48.1371079\",\"lon\":\"11.5753822\",\"display_name\":\"M\xC3\xBCnchen, Bayern, Deutschland\"}]";

static void test_parseResults_reads_a_nominatim_reply()
{
    Result results[kMaxResults];
    const int n = parseResults(kNominatimReply, strlen(kNominatimReply), results, kMaxResults);
    TEST_ASSERT_EQUAL_INT(2, n);
    EXPECT_NEAR(39.2190607, results[0].lat, 1e-7, "results[0].lat");
    EXPECT_NEAR(-121.0610606, results[0].lon, 1e-7, "results[0].lon");
    TEST_ASSERT_EQUAL_STRING("Grass Valley, Nevada County, California, United States", results[0].name);
    TEST_ASSERT_EQUAL_STRING("M\xC3\xBCnchen, Bayern, Deutschland", results[1].name);
}

static void test_parseResults_empty_and_numeric_coordinates()
{
    Result results[kMaxResults];
    TEST_ASSERT_EQUAL_INT(0, parseResults("[]", 2, results, kMaxResults));
    TEST_ASSERT_EQUAL_INT(0, parseResults(" [ ] ", 5, results, kMaxResults));

    const char numeric[] = "[{\"lat\": 10.5, \"lon\": -20.25, \"display_name\": \"x\"}]";
    TEST_ASSERT_EQUAL_INT(1, parseResults(numeric, strlen(numeric), results, kMaxResults));
    EXPECT_NEAR(10.5, results[0].lat, 1e-9, "results[0].lat");
    EXPECT_NEAR(-20.25, results[0].lon, 1e-9, "results[0].lon");
}

static void test_parseResults_skips_places_without_a_position()
{
    Result results[kMaxResults];
    const char reply[] = "[{\"display_name\":\"no position\"},"
                         "{\"lat\":\"abc\",\"lon\":\"1\",\"display_name\":\"bad lat\"},"
                         "{\"lat\":\"95\",\"lon\":\"1\",\"display_name\":\"out of range\"},"
                         "{\"lat\":\"1\",\"lon\":\"2\",\"display_name\":\"good\"}]";
    TEST_ASSERT_EQUAL_INT(1, parseResults(reply, strlen(reply), results, kMaxResults));
    TEST_ASSERT_EQUAL_STRING("good", results[0].name);
}

static void test_parseResults_stops_at_max()
{
    Result results[2];
    const char reply[] = "[{\"lat\":\"1\",\"lon\":\"1\"},{\"lat\":\"2\",\"lon\":\"2\"},{\"lat\":\"3\",\"lon\":\"3\"}]";
    TEST_ASSERT_EQUAL_INT(2, parseResults(reply, strlen(reply), results, 2));
    EXPECT_NEAR(2.0, results[1].lat, 1e-9, "results[1].lat");
}

static void test_parseResults_long_name_truncates_on_a_character()
{
    Result results[1];
    // 62 ASCII bytes then a 2-byte character: it would straddle the 63-byte limit, so it is dropped whole
    char reply[256];
    snprintf(reply, sizeof(reply), "[{\"lat\":\"1\",\"lon\":\"1\",\"display_name\":\"%s\xC3\xBCzz\"}]",
             "12345678901234567890123456789012345678901234567890123456789012");
    TEST_ASSERT_EQUAL_INT(1, parseResults(reply, strlen(reply), results, 1));
    TEST_ASSERT_EQUAL_size_t(62, strlen(results[0].name));
}

static void test_parseResults_malformed_is_refused()
{
    Result results[kMaxResults];
    const char *bad[] = {
        "",
        "{\"lat\":\"1\"}",                                // an object, not an array
        "[{\"lat\":\"1\",\"lon\":\"2\"}",                 // unterminated array
        "[{\"lat\":\"1\",\"lon\":\"2}]",                  // unterminated string
        "[{\"lat\" \"1\"}]",                              // missing colon
        "[{\"display_name\":\"bad \\x escape\"}]",        // invalid escape
        "<html>429 Too Many Requests</html>",             // a rate-limit page
        "[{\"boundingbox\":[\"1\",\"2\"},\"lat\":\"1\"]", // mismatched nesting
    };
    for (const char *reply : bad)
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, parseResults(reply, strlen(reply), results, kMaxResults), reply);
}

static void test_parseResults_null_inputs()
{
    Result results[1];
    TEST_ASSERT_EQUAL_INT(-1, parseResults(nullptr, 0, results, 1));
    TEST_ASSERT_EQUAL_INT(-1, parseResults("[]", 2, nullptr, 1));
    TEST_ASSERT_EQUAL_INT(-1, parseResults("[]", 2, results, 0));
}

// The Census reply for 3123 Professional Dr, Auburn, CA 95603 - an address OpenStreetMap has no house number for.
static const char kCensusReply[] =
    "{\"result\":{\"input\":{\"address\":{\"address\":\"3123 Professional Dr, Auburn, CA 95603\"}},"
    "\"addressMatches\":[{\"tigerLine\":{\"side\":\"L\",\"tigerLineId\":\"106780434\"},"
    "\"coordinates\":{\"x\":-121.100849708036,\"y\":38.940771593878},"
    "\"addressComponents\":{\"zip\":\"95603\",\"streetName\":\"PROFESSIONAL\",\"fromAddress\":\"3115\"},"
    "\"matchedAddress\":\"3123 PROFESSIONAL DR, AUBURN, CA, 95603\"}]}}";

static void test_parseCensus_reads_address_matches()
{
    Result results[kMaxResults];
    const int n = parseCensus(kCensusReply, strlen(kCensusReply), results, kMaxResults);
    TEST_ASSERT_EQUAL_INT(1, n);
    EXPECT_NEAR(38.940771593878, results[0].lat, 1e-9, "lat");
    EXPECT_NEAR(-121.100849708036, results[0].lon, 1e-9, "lon"); // x is longitude
    TEST_ASSERT_EQUAL_STRING("3123 PROFESSIONAL DR, AUBURN, CA, 95603", results[0].name);

    const char none[] = "{\"result\":{\"input\":{},\"addressMatches\":[]}}";
    TEST_ASSERT_EQUAL_INT(0, parseCensus(none, strlen(none), results, kMaxResults));
    TEST_ASSERT_EQUAL_INT(-1, parseCensus("[]", 2, results, kMaxResults));
}

// Photon's reply for the same query: GeoJSON, [lon, lat], and the name assembled from the address properties.
static const char kPhotonReply[] =
    "{\"type\":\"FeatureCollection\",\"features\":[{\"type\":\"Feature\",\"properties\":{\"osm_type\":\"N\","
    "\"osm_id\":5470166947,\"name\":\"Sutter Auburn Surgery Center\",\"street\":\"Professional Drive\","
    "\"county\":\"Placer\",\"state\":\"California\",\"postcode\":\"95603\",\"countrycode\":\"US\"},"
    "\"geometry\":{\"type\":\"Point\",\"coordinates\":[-121.1012707,38.9411749]}},"
    "{\"geometry\":{\"coordinates\":[11.5,48.1]},\"properties\":{\"housenumber\":\"12\",\"street\":\"Marienplatz\","
    "\"city\":\"M\xC3\xBCnchen\",\"county\":\"Oberbayern\"}}]}";

static void test_parsePhoton_reads_features()
{
    Result results[kMaxResults];
    const int n = parsePhoton(kPhotonReply, strlen(kPhotonReply), results, kMaxResults);
    TEST_ASSERT_EQUAL_INT(2, n);
    EXPECT_NEAR(38.9411749, results[0].lat, 1e-9, "lat");
    EXPECT_NEAR(-121.1012707, results[0].lon, 1e-9, "lon");
    // "California" would not fit whole, so it is left off rather than cut
    TEST_ASSERT_EQUAL_STRING("Sutter Auburn Surgery Center, Professional Drive, Placer", results[0].name);
    // The city wins over the county, and a house number leads its street
    TEST_ASSERT_EQUAL_STRING("12 Marienplatz, M\xC3\xBCnchen", results[1].name);
}

static void test_parsePhoton_long_name_cuts_on_a_character()
{
    Result results[1];
    // 62 ASCII bytes then a 2-byte character straddling the 63-byte limit: it is dropped whole
    const char reply[] = "{\"features\":[{\"geometry\":{\"coordinates\":[1,2]},\"properties\":{"
                         "\"name\":\"12345678901234567890123456789012345678901234567890123456789012\xC3\xBCz\"}}]}";
    TEST_ASSERT_EQUAL_INT(1, parsePhoton(reply, strlen(reply), results, 1));
    TEST_ASSERT_EQUAL_size_t(62, strlen(results[0].name));
}

static void test_parseFor_picks_the_provider_parser()
{
    Result results[kMaxResults];
    TEST_ASSERT_EQUAL_INT(1, parseFor(Provider::Census, kCensusReply, strlen(kCensusReply), results, kMaxResults));
    TEST_ASSERT_EQUAL_INT(2, parseFor(Provider::Photon, kPhotonReply, strlen(kPhotonReply), results, kMaxResults));
    TEST_ASSERT_EQUAL_INT(-1, parseFor(Provider::Nominatim, kPhotonReply, strlen(kPhotonReply), results, kMaxResults));
}

void setUp(void) {}
void tearDown(void) {}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();

    printf("\n=== Request URL ===\n");
    RUN_TEST(test_encodeQuery_escapes_reserved_and_utf8);
    RUN_TEST(test_encodeQuery_overflow_fails_closed);
    RUN_TEST(test_expandSearchUrl_default_template);
    RUN_TEST(test_expandSearchUrl_fails_closed);

    printf("\n=== Reply parse ===\n");
    RUN_TEST(test_parseResults_reads_a_nominatim_reply);
    RUN_TEST(test_parseResults_empty_and_numeric_coordinates);
    RUN_TEST(test_parseResults_skips_places_without_a_position);
    RUN_TEST(test_parseResults_stops_at_max);
    RUN_TEST(test_parseResults_long_name_truncates_on_a_character);

    printf("\n=== Fallback providers ===\n");
    RUN_TEST(test_parseCensus_reads_address_matches);
    RUN_TEST(test_parsePhoton_reads_features);
    RUN_TEST(test_parsePhoton_long_name_cuts_on_a_character);
    RUN_TEST(test_parseFor_picks_the_provider_parser);

    printf("\n=== Fails closed ===\n");
    RUN_TEST(test_parseResults_malformed_is_refused);
    RUN_TEST(test_parseResults_null_inputs);

    exit(UNITY_END());
}

void loop() {}
