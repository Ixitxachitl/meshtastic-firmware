#include "configuration.h"
#if HAS_SCREEN
#include "CompassRenderer.h"

// Memory optimization: Use smaller cache on memory-constrained devices
// Also use simplified compass for small displays
#if defined(ARCH_RP2040) || defined(ARCH_NRF52) || defined(M5STACK_UNITC6L)
#define COMPASS_MEMORY_OPTIMIZED 1
#else
#define COMPASS_MEMORY_OPTIMIZED 0
#endif
#include "Math3D.h"
#include "NodeDB.h"
#include "UIRenderer.h"
#include "configuration.h"
#include "gps/GeoCoord.h"
#include "graphics/ScreenFonts.h"
#include "graphics/SharedUIDisplay.h"
#include "motion/BMI270Sensor.h" // to fetch attitude quaternion
#include <Arduino.h>             // for millis()
#include <cmath>
extern "C" Quat GetAttitudeForRenderer(); // accessor provided by BMI270Sensor.cpp (see patch 2)
extern "C" Vec3 GetGravityForRenderer();
extern "C" float GetHeadingRadiansForRenderer();

namespace
{
// Simple Point helper class for fallback compass (used on low-resolution displays)
struct SimplePoint {
    float x, y;
    SimplePoint(float x, float y) : x(x), y(y) {}

    void rotate(float angle)
    {
        float cos_a = cos(angle);
        float sin_a = sin(angle);
        float new_x = x * cos_a - y * sin_a;
        float new_y = x * sin_a + y * cos_a;
        x = new_x;
        y = new_y;
    }

    void scale(float factor)
    {
        x *= factor;
        y *= factor;
    }

    void translate(float dx, float dy)
    {
        x += dx;
        y += dy;
    }
};

// Cached sphere geometry to avoid repeated trigonometric calculations
struct SphereGeometry {
#if COMPASS_MEMORY_OPTIMIZED
    static constexpr int MAX_MERIDIANS = 6; // Ultra-low memory: 6 meridians
    static constexpr int MAX_PARALLELS = 4; // Ultra-low memory: 4 parallels
    static constexpr int MAX_SEGMENTS = 16; // Ultra-low memory: 16 segments
#else
    static constexpr int MAX_MERIDIANS = 12; // Reduced from 12 (-33% memory)
    static constexpr int MAX_PARALLELS = 6;  // Reduced from 8 (-25% memory)
    static constexpr int MAX_SEGMENTS = 24;  // Reduced from 32 (-25% memory)
#endif

    Vec3 meridians[MAX_MERIDIANS][MAX_SEGMENTS + 1];
    Vec3 parallels[MAX_PARALLELS][MAX_SEGMENTS + 1];
    bool initialized = false;
    float cached_radius = 0.0f; // Track radius to avoid unnecessary recomputation

    void initialize(float radius)
    {
        if (initialized && fabsf(cached_radius - radius) < 0.1f)
            return; // Skip if already initialized with same radius

        cached_radius = radius;

        // Pre-compute meridians
        for (int m = 0; m < MAX_MERIDIANS; ++m) {
            float phi = (2.0f * (float)M_PI * m) / MAX_MERIDIANS;
            float cos_phi = std::cos(phi);
            float sin_phi = std::sin(phi);

            for (int i = 0; i <= MAX_SEGMENTS; ++i) {
                float t = ((float)i / MAX_SEGMENTS) * (float)M_PI - (float)M_PI / 2.0f;
                float cos_t = std::cos(t);
                float sin_t = std::sin(t);
                meridians[m][i] = Vec3(cos_t * cos_phi, sin_t, -cos_t * sin_phi) * radius;
            }
        }

        // Pre-compute parallels with equator included in even spacing
        for (int n = 0; n < MAX_PARALLELS; ++n) {
            float theta;

            if (MAX_PARALLELS == 1) {
                theta = 0.0f; // Single parallel is equator
            } else if (MAX_PARALLELS % 2 == 1) {
                // Odd number: center parallel is exactly equator (theta = 0)
                int center = MAX_PARALLELS / 2;
                theta = ((float)(n - center) / center) * (float)M_PI / 3.0f; // ±60° range
            } else {
                // Even number: create symmetric spacing around equator, force one to be equator
                if (n == MAX_PARALLELS / 2) {
                    theta = 0.0f; // Force this one to be equator
                } else {
                    // Space others evenly around it
                    int offset = (n < MAX_PARALLELS / 2) ? (n - MAX_PARALLELS / 2) : (n - MAX_PARALLELS / 2);
                    theta = ((float)offset / (MAX_PARALLELS / 2)) * (float)M_PI / 3.0f;
                }
            }

            float sin_theta = std::sin(theta);
            float cos_theta = std::cos(theta);

            for (int i = 0; i <= MAX_SEGMENTS; ++i) {
                float a = (2.0f * (float)M_PI * i) / MAX_SEGMENTS;
                float cos_a = std::cos(a);
                float sin_a = std::sin(a);
                parallels[n][i] = Vec3(cos_theta * cos_a, sin_theta, -cos_theta * sin_a) * radius;
            }
        }

        initialized = true;
    }
};

static SphereGeometry g_sphereCache;

// rotate unit vector a onto unit vector b
static inline Quat quatBetweenUnit(const Vec3 &a, const Vec3 &b)
{
    float d = a.dot(b);
    Vec3 v = a.cross(b);
    float w = 1.0f + d;
    if (w < 1e-6f) { // 180°
        Vec3 axis = (fabsf(a.x) < 0.5f) ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
        v = a.cross(axis).normalized();
        return Quat(0, v.x, v.y, v.z);
    }
    Quat q(w, v.x, v.y, v.z);
    q.normalize();
    return q;
}

// Project with ORTHO and keep depth so we can cull/clamp to the front hemisphere.
struct ProjPt {
    int x, y;
    float z;
    bool ok;
};
static inline ProjPt projectOrtho(const Quat &q, const Vec3 &p, int cx, int cy)
{
    Vec3 pc = q.rotate(p);
    ProjPt r;
    r.x = cx + (int)std::lround(pc.x);
    r.y = cy + (int)std::lround(pc.y);
    r.z = pc.z; // view dir = -Z; front = z <= 0
    r.ok = true;
    return r;
}

// ---------- Startup-stable render transform (shared by sphere & labels) ----------
static inline float wrapPi(float a)
{
    while (a > M_PI)
        a -= 2.0f * M_PI;
    while (a <= -M_PI)
        a += 2.0f * M_PI;
    return a;
}

// Global flag to force top-down view for FIXED_RING mode
static bool g_forceTopDownView = false;

// Build a stable render quaternion for a "world view" compass.
// - Tilt is based ONLY on gravity, keeping the globe level with the world.
// - Yaw is based ONLY on the magnetic heading.
// - If g_forceTopDownView is true, ignore gravity and use top-down view
static Quat buildSmoothedRenderQuat(const Quat &attitude)
{
    static bool init = false;
    static Quat qRenderFilt(1, 0, 0, 0);

    Quat qRender;

    if (g_forceTopDownView) {
        // For FIXED_RING mode: create top-down view looking down from above
        // Rotate 90 degrees around X-axis to look down at the compass from above
        // DO NOT apply heading rotation - keep the ring fixed, let needle rotate instead
        qRender = Quat::fromAxisAngle(Vec3(1, 0, 0), M_PI / 2.0f);
    } else {
        // Normal mode: Calculate Tilt based on Gravity
        // We want the globe's North Pole (+Y) to point opposite to gravity.
        // The screen's "up" is -Y, so we align the screen's up with anti-gravity.
        Vec3 g_body = GetGravityForRenderer();
        Vec3 anti_g = g_body * -1.0f;
        Quat qTilt = quatBetweenUnit(Vec3(0, -1, 0), anti_g.normalized());

        // Calculate Yaw based on Heading
        // We rotate the globe around its pole axis (now Y) by the heading.
        float heading_rad = GetHeadingRadiansForRenderer();
        Quat qYaw = Quat::fromAxisAngle(Vec3(0, 1, 0), -heading_rad); // -rad for clockwise heading

        // Combine them: First orient to gravity, then rotate to North.
        qRender = qTilt * qYaw;
    }

    // 4. Apply simple smoothing to the final quaternion
    const float alpha = init ? 0.25f : 1.0f;

    // NLERP for smooth animation
    if (qRender.dot(qRenderFilt) < 0.0f) {
        // Negate the quaternion to ensure the shortest rotational path
        qRenderFilt.w = -qRenderFilt.w;
        qRenderFilt.x = -qRenderFilt.x;
        qRenderFilt.y = -qRenderFilt.y;
        qRenderFilt.z = -qRenderFilt.z;
    }
    Quat qBlend((1.0f - alpha) * qRenderFilt.w + alpha * qRender.w, (1.0f - alpha) * qRenderFilt.x + alpha * qRender.x,
                (1.0f - alpha) * qRenderFilt.y + alpha * qRender.y, (1.0f - alpha) * qRenderFilt.z + alpha * qRender.z);
    qBlend.normalize();
    qRenderFilt = qBlend;
    init = true;

    return qRenderFilt;
}

// Fast front hemisphere check using dot product with view direction
static inline bool isFrontFacing(const Vec3 &rotated_pt)
{
    return rotated_pt.z <= 0.0f; // View direction is -Z
}

// Optimized polyline drawing with bulk front/back testing
static inline void drawPolylineFront(OLEDDisplay *d, const Quat &q, int cx, int cy, const Vec3 *pts, int count, bool drawDots)
{
    if (count < 2)
        return;

    // Pre-transform all points and test front-facing in batches
    ProjPt projPts[64]; // Stack buffer for moderate polylines
    bool *isFront = nullptr;
    bool frontFlags[64];

    if (count <= 64) {
        isFront = frontFlags;
    } else {
        // For very large polylines, use simplified approach
        // Just draw segments that are likely visible
        for (int i = 0; i < count - 1; i += 4) { // Skip every 4th for performance
            ProjPt p1 = projectOrtho(q, pts[i], cx, cy);
            ProjPt p2 = projectOrtho(q, pts[i + 1], cx, cy);

            if (p1.ok && p2.ok && p1.z <= 0.0f && p2.z <= 0.0f) {
                d->drawLine(p1.x, p1.y, p2.x, p2.y);
            }
        }
        return;
    }

    // Transform all points once
    for (int i = 0; i < count; ++i) {
        projPts[i] = projectOrtho(q, pts[i], cx, cy);
        isFront[i] = projPts[i].ok && projPts[i].z <= 0.0f;
    }

    // Draw dots for front-facing vertices
    if (drawDots) {
        for (int i = 0; i < count; ++i) {
            if (isFront[i]) {
                d->fillCircle(projPts[i].x, projPts[i].y, 1);
            }
        }
    }

    // Draw line segments
    for (int i = 0; i < count - 1; ++i) {
        bool prevFront = isFront[i];
        bool currFront = isFront[i + 1];

        if (prevFront && currFront) {
            // Both points visible - draw full segment
            d->drawLine(projPts[i].x, projPts[i].y, projPts[i + 1].x, projPts[i + 1].y);
        } else if (prevFront != currFront) {
            // Crossing front/back boundary - clip to horizon
            float denom = projPts[i].z - projPts[i + 1].z;
            if (fabsf(denom) > 1e-6f) {
                float t = projPts[i].z / denom;
                t = (t < 0.0f) ? 0.0f : (t > 1.0f) ? 1.0f : t;

                // Interpolate clip point in screen space for efficiency
                int clipX = (int)(projPts[i].x + t * (projPts[i + 1].x - projPts[i].x));
                int clipY = (int)(projPts[i].y + t * (projPts[i + 1].y - projPts[i].y));

                if (prevFront) {
                    d->drawLine(projPts[i].x, projPts[i].y, clipX, clipY);
                } else {
                    d->drawLine(clipX, clipY, projPts[i + 1].x, projPts[i + 1].y);
                }
            }
        }
        // Skip segments where both points are behind
    }
}

} // namespace

namespace graphics
{
namespace CompassRenderer
{

namespace
{
// Rotate + project (ORTHOGRAPHIC): ignore depth, no foreshortening
inline bool projectPoint(const Quat &qAtt, const Vec3 &pModel, int cx, int cy, int &outX, int &outY)
{
    Vec3 pc = qAtt.rotate(pModel); // Y up in model -> Y increases downward on screen
    outX = cx + (int)std::lround(pc.x);
    outY = cy + (int)std::lround(pc.y);
    return true;
}

// Draw one closed polyline (front-hemisphere only; simple reject if behind)
inline void drawPolylineProjected(OLEDDisplay *d, const Quat &qAtt, int cx, int cy, const Vec3 *pts, int count)
{
    int px = 0, py = 0;
    bool hasPrev = false;
    for (int i = 0; i < count; ++i) {
        int sx, sy;
        if (projectPoint(qAtt, pts[i], cx, cy, sx, sy)) {
            if (hasPrev)
                d->drawLine(px, py, sx, sy);
            px = sx;
            py = sy;
            hasPrev = true;
        } else {
            hasPrev = false;
        }
    }
}
} // namespace

void drawCenterNeedle3D(OLEDDisplay *display, int16_t cx, int16_t cy, uint16_t radius, const Quat &attitude, float bearingRad,
                        float elevRad)
{
    const uint16_t rDraw = (uint16_t)std::max<int>(1, (int)radius);
    const int16_t cxShift = (int16_t)(cx - (int)(rDraw * 0.14f)); // same left shift

    // Build the shared render transform, then CANCEL yaw so the needle is ring-locked
    const Quat qFull = buildSmoothedRenderQuat(attitude); // (tilt * yaw(-heading))
    const float heading = GetHeadingRadiansForRenderer(); // 0 = North, +CW
    const Quat qYawInv = Quat::fromAxisAngle(Vec3(0, 1, 0), +heading);
    const Quat qTiltOnly = qFull * qYawInv; // remove yaw

    // Relative bearing (same convention as the little compass): (bearing - heading), wrapped
    float rel = bearingRad - heading;
    while (rel > M_PI)
        rel -= 2.0f * M_PI;
    while (rel <= -M_PI)
        rel += 2.0f * M_PI;

    // Target vector in COMPASS frame (X=East, Y=Up, Z=North-on-compass).
    // With camera forward = -Z, use -cos(rel)*cos(elev) for Z.
    const float ca = std::cos(rel), sa = std::sin(rel);
    const float ce = std::cos(elevRad), se = std::sin(elevRad);
    Vec3 vComp(sa * ce, se, +ca * ce);

    // Camera-space forward unit vector (tilt only)
    Vec3 vCam = qTiltOnly.rotate(vComp);
    float n3 = std::sqrt(vCam.x * vCam.x + vCam.y * vCam.y + vCam.z * vCam.z);
    if (n3 < 1e-6f)
        return;
    vCam.x /= n3;
    vCam.y /= n3;
    vCam.z /= n3;

    // Compass-locked left/right axis: Up × Forward (with pole fallback), then tilt to screen
    Vec3 upC(0.f, 1.f, 0.f);
    Vec3 pComp = upC.cross(vComp);
    float pn = std::sqrt(pComp.x * pComp.x + pComp.y * pComp.y + pComp.z * pComp.z);
    if (pn < 1e-6f) {
        pComp = Vec3(1.f, 0.f, 0.f).cross(vComp);
        pn = std::sqrt(pComp.x * pComp.x + pComp.y * pComp.y + pComp.z * pComp.z);
        if (pn < 1e-6f)
            return;
    }
    pComp.x /= pn;
    pComp.y /= pn;
    pComp.z /= pn;

    Vec3 pCam = qTiltOnly.rotate(pComp);
    float lenP = std::sqrt(pCam.x * pCam.x + pCam.y * pCam.y);
    const float px = (lenP > 1e-6f) ? (pCam.x / lenP) : 1.f;
    const float py = (lenP > 1e-6f) ? (pCam.y / lenP) : 0.f;

    // Original needle geometry with containment fix
    const float frontInsetFrac = 0.015f;
    const float edgeBackFrac = 0.65f; // Reduced from 0.75f to keep tail inside
    const float backSurfaceInsetFrac = 0.015f;

    const float rFrontEdge = rDraw * (1.f - frontInsetFrac);
    const float rBackEdge = rDraw * edgeBackFrac;
    const float rBackSurf = rDraw * (1.f - backSurfaceInsetFrac);

    const int16_t xEfront = cxShift + (int16_t)std::lround(+rFrontEdge * vCam.x);
    const int16_t yEfront = cy + (int16_t)std::lround(+rFrontEdge * vCam.y);
    const int16_t xEback = cxShift + (int16_t)std::lround(-rBackEdge * vCam.x);
    const int16_t yEback = cy + (int16_t)std::lround(-rBackEdge * vCam.y);

    const float backSpread = rDraw * 0.38f;
    const int16_t xBL = cxShift + (int16_t)std::lround(-rBackSurf * vCam.x - backSpread * px);
    const int16_t yBL = cy + (int16_t)std::lround(-rBackSurf * vCam.y - backSpread * py);
    const int16_t xBR = cxShift + (int16_t)std::lround(-rBackSurf * vCam.x + backSpread * px);
    const int16_t yBR = cy + (int16_t)std::lround(-rBackSurf * vCam.y + backSpread * py);

    display->drawTriangle(xEfront, yEfront, xEback, yEback, xBL, yBL);
#ifdef USE_EINK
    display->drawTriangle(xEfront, yEfront, xBR, yBR, xEback, yEback);
#else
    display->fillTriangle(xEfront, yEfront, xBR, yBR, xEback, yEback);
#endif
}

void drawCompassSphere(OLEDDisplay *d, int16_t cx, int16_t cy, uint16_t radius, const Quat &attitude)
{
    const uint16_t rDraw = (uint16_t)std::max<int>(1, (int)(radius));
    const int16_t cxShift = (int16_t)(cx - (int)(rDraw * 0.14f));

    // Initialize cached geometry if needed
    g_sphereCache.initialize((float)rDraw);

    // Use the shared, smoothed render transform
    Quat qRender = buildSmoothedRenderQuat(attitude);

    // Always-on border around the sphere
    d->drawCircle(cxShift, cy, rDraw);

    // Draw meridians using cached geometry
    for (int m = 0; m < g_sphereCache.MAX_MERIDIANS; ++m) {
        drawPolylineFront(d, qRender, cxShift, cy, g_sphereCache.meridians[m], g_sphereCache.MAX_SEGMENTS + 1, false);
    }

    // Draw parallels using cached geometry (includes equator)
    for (int n = 0; n < g_sphereCache.MAX_PARALLELS; ++n) {
        // Check if this is the equator - simple and explicit
        bool isEquator = false;
        if (g_sphereCache.MAX_PARALLELS == 1) {
            isEquator = (n == 0); // Single parallel is equator
        } else if (g_sphereCache.MAX_PARALLELS % 2 == 1) {
            isEquator = (n == g_sphereCache.MAX_PARALLELS / 2); // Center parallel for odd count
        } else {
            isEquator = (n == g_sphereCache.MAX_PARALLELS / 2); // Forced equator for even count
        }

        if (isEquator) {
            // Draw complete equator circle - NO culling, always show entire circle
            for (int i = 0; i < g_sphereCache.MAX_SEGMENTS; ++i) {
                ProjPt p1 = projectOrtho(qRender, g_sphereCache.parallels[n][i], cxShift, cy);
                ProjPt p2 = projectOrtho(qRender, g_sphereCache.parallels[n][i + 1], cxShift, cy);

                if (p1.ok && p2.ok) { // Only check screen bounds, ignore z-depth completely
                    d->drawLine(p1.x, p1.y, p2.x, p2.y);
                }
            }
            // Draw equator twice for emphasis (thicker line)
            for (int i = 0; i < g_sphereCache.MAX_SEGMENTS; ++i) {
                ProjPt p1 = projectOrtho(qRender, g_sphereCache.parallels[n][i], cxShift, cy);
                ProjPt p2 = projectOrtho(qRender, g_sphereCache.parallels[n][i + 1], cxShift, cy);

                if (p1.ok && p2.ok) {
                    d->drawLine(p1.x, p1.y, p2.x, p2.y);
                }
            }
        } else {
            // Draw other parallels with normal front-face culling
            drawPolylineFront(d, qRender, cxShift, cy, g_sphereCache.parallels[n], g_sphereCache.MAX_SEGMENTS + 1, false);
        }
    }
}

// Point helper class for compass calculations
struct Point {
    float x, y;
    Point(float x, float y) : x(x), y(y) {}

    void rotate(float angle)
    {
        float cos_a = cos(angle);
        float sin_a = sin(angle);
        float new_x = x * cos_a - y * sin_a;
        float new_y = x * sin_a + y * cos_a;
        x = new_x;
        y = new_y;
    }

    void scale(float factor)
    {
        x *= factor;
        y *= factor;
    }

    void translate(float dx, float dy)
    {
        x += dx;
        y += dy;
    }
};

void drawCompassNorth(OLEDDisplay *display, int16_t compassX, int16_t compassY, float myHeading, int16_t radius)
{
    // Simple compass north indicator (works on all display types)
#if !defined(USE_EINK)
    if (currentResolution == ScreenResolution::High) {
        radius += 4;
    }
#endif
    SimplePoint north(0, -radius);
    if (uiconfig.compass_mode != meshtastic_CompassMode_FIXED_RING)
        north.rotate(-myHeading);
    north.translate(compassX, compassY);

    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    display->setColor(BLACK);
#if !defined(USE_EINK)
    if (currentResolution == ScreenResolution::High) {
        display->fillRect(north.x - 8, north.y - 1, display->getStringWidth("N") + 3, FONT_HEIGHT_SMALL - 6);
    } else
#endif
    {
        display->fillRect(north.x - 4, north.y - 1, display->getStringWidth("N") + 2, FONT_HEIGHT_SMALL - 6);
    }
    display->setColor(WHITE);
    display->drawString(north.x, north.y - 3, "N");
}

void drawNodeHeading(OLEDDisplay *display, int16_t compassX, int16_t compassY, uint16_t compassDiam, float headingRadian)
{
#if !defined(USE_EINK)
    if (isHighResolution()) {
        // Use 3D compass for high-resolution displays (OLEDs)
        const Quat attitude = GetAttitudeForRenderer();
        const uint16_t radius = compassDiam / 2;

        // draw the globe first
        drawCompassSphere(display, compassX, compassY, radius, attitude);

        // same geometry constants used by the globe
        const uint16_t rDraw = (uint16_t)std::max<int>(1, (int)(radius));
        const int16_t cxShift = (int16_t)(compassX - (int)(rDraw * 0.14f));
        const int16_t cy = compassY;
        const float rLabel = rDraw * 1.06f; // just outside equator

        // Same smoothed transform as the sphere so labels stay locked to the equator
        const Quat qLabel = buildSmoothedRenderQuat(attitude);

        // Choose your basis (forward = −Z). If your build uses +Z forward, flip signs as discussed.
        struct Mark {
            const char *t;
            Vec3 p;
        };
        Mark marks[] = {
            {"N", Vec3(0, 0, 1)},
            {"E", Vec3(1, 0, 0)},
            {"S", Vec3(0, 0, -1)},
            {"W", Vec3(-1, 0, 0)},
        };

        display->setFont(FONT_SMALL);
        display->setTextAlignment(TEXT_ALIGN_CENTER);

        for (auto &m : marks) {
            int x, y;
            if (projectPoint(qLabel, m.p * (float)rLabel, cxShift, cy, x, y)) {
                display->drawString(x, y - (FONT_HEIGHT_SMALL / 2), m.t);
            }
        }
    } else {
        // Simple compass for low-resolution displays (TFTs)
        SimplePoint tip(0.0f, -0.5f), tail(0.0f, 0.35f); // pointing up initially
        float arrowOffsetX = 0.14f, arrowOffsetY = 0.9f;
        SimplePoint leftArrow(tip.x - arrowOffsetX, tip.y + arrowOffsetY), rightArrow(tip.x + arrowOffsetX, tip.y + arrowOffsetY);

        SimplePoint *arrowPoints[] = {&tip, &tail, &leftArrow, &rightArrow};

        for (int i = 0; i < 4; i++) {
            arrowPoints[i]->rotate(headingRadian);
            arrowPoints[i]->scale(compassDiam * 0.6);
            arrowPoints[i]->translate(compassX, compassY);
        }

#ifdef USE_EINK
        display->drawTriangle(tip.x, tip.y, rightArrow.x, rightArrow.y, tail.x, tail.y);
#else
        display->fillTriangle(tip.x, tip.y, rightArrow.x, rightArrow.y, tail.x, tail.y);
#endif
        display->drawTriangle(tip.x, tip.y, leftArrow.x, leftArrow.y, tail.x, tail.y);
    }
#else
    // E-ink displays: use simple compass
    SimplePoint tip(0.0f, -0.5f), tail(0.0f, 0.35f); // pointing up initially
    float arrowOffsetX = 0.14f, arrowOffsetY = 0.9f;
    SimplePoint leftArrow(tip.x - arrowOffsetX, tip.y + arrowOffsetY), rightArrow(tip.x + arrowOffsetX, tip.y + arrowOffsetY);

    SimplePoint *arrowPoints[] = {&tip, &tail, &leftArrow, &rightArrow};

    for (int i = 0; i < 4; i++) {
        arrowPoints[i]->rotate(headingRadian);
        arrowPoints[i]->scale(compassDiam * 0.6);
        arrowPoints[i]->translate(compassX, compassY);
    }

    display->drawTriangle(tip.x, tip.y, rightArrow.x, rightArrow.y, tail.x, tail.y);
    display->drawTriangle(tip.x, tip.y, leftArrow.x, leftArrow.y, tail.x, tail.y);
#endif
}

void drawArrowToNode(OLEDDisplay *display, int16_t x, int16_t y, int16_t size, float bearing)
{
    float radians = bearing * DEG_TO_RAD;

    SimplePoint tip(0, -size / 2);
    SimplePoint left(-size / 6, size / 4);
    SimplePoint right(size / 6, size / 4);
    SimplePoint tail(0, size / 4.5);

    tip.rotate(radians);
    left.rotate(radians);
    right.rotate(radians);
    tail.rotate(radians);

    tip.translate(x, y);
    left.translate(x, y);
    right.translate(x, y);
    tail.translate(x, y);

    display->fillTriangle(tip.x, tip.y, left.x, left.y, tail.x, tail.y);
    display->fillTriangle(tip.x, tip.y, right.x, right.y, tail.x, tail.y);
}

float estimatedHeading(double lat, double lon)
{
    // Simple magnetic declination estimation
    // This is a very basic implementation - the original might be more sophisticated
    return 0.0f; // Return 0 for now, indicating no heading available
}

uint16_t getCompassDiam(uint32_t displayWidth, uint32_t displayHeight)
{
    // Calculate appropriate compass diameter based on display size
    uint16_t minDimension = (displayWidth < displayHeight) ? displayWidth : displayHeight;
    uint16_t maxDiam = minDimension / 3; // Use 1/3 of the smaller dimension

    // Ensure minimum and maximum bounds
    if (maxDiam < 16)
        maxDiam = 16;
    if (maxDiam > 64)
        maxDiam = 64;

    return maxDiam;
}

void setTopDownView(bool enable)
{
    g_forceTopDownView = enable;
}

// Runtime-detected compass functions (simple overloads)
void drawCompassSphere(OLEDDisplay *display, int16_t cx, int16_t cy, uint16_t radius)
{
#if !defined(USE_EINK)
    if (isHighResolution()) {
        // Use 3D sphere for high-resolution displays (OLEDs)
        const Quat attitude = GetAttitudeForRenderer();
        drawCompassSphere(display, cx, cy, radius, attitude);
    } else
#endif
    {
        // Simple circle for low-resolution displays (TFTs and e-ink)
        display->drawCircle(cx, cy, radius);
    }
}

void drawCenterNeedle3D(OLEDDisplay *display, int16_t cx, int16_t cy, uint16_t radius, float bearingRad, float elevRad)
{
#if !defined(USE_EINK)
    if (isHighResolution()) {
        // Use 3D needle for high-resolution displays (OLEDs)
        const Quat attitude = GetAttitudeForRenderer();
        drawCenterNeedle3D(display, cx, cy, radius, attitude, bearingRad, elevRad);
    } else
#endif
    {
        // Simple arrow for low-resolution displays (TFTs and e-ink)
        drawArrowToNode(display, cx, cy, radius, bearingRad * RAD_TO_DEG);
    }
}

} // namespace CompassRenderer
} // namespace graphics
#endif