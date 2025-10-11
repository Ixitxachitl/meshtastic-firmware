#include "CompassRenderer.h"
#include "NodeDB.h"
#include "UIRenderer.h"
#include "configuration.h"
#include "gps/GeoCoord.h"
#include "graphics/ScreenFonts.h"
#include "graphics/SharedUIDisplay.h"
#include <cmath>
#include "Math3D.h"
#include "motion/BMI270Sensor.h"    // to fetch attitude quaternion
extern "C" Quat GetAttitudeForRenderer();  // accessor provided by BMI270Sensor.cpp (see patch 2)
extern "C" Vec3 GetGravityForRenderer();
extern "C" float GetHeadingRadiansForRenderer();

namespace {
// rotate unit vector a onto unit vector b
static inline Quat quatBetweenUnit(const Vec3& a, const Vec3& b) {
    float d = a.dot(b);
    Vec3  v = a.cross(b);
    float w = 1.0f + d;
    if (w < 1e-6f) { // 180°
        Vec3 axis = (fabsf(a.x) < 0.5f) ? Vec3(1,0,0) : Vec3(0,1,0);
        v = a.cross(axis).normalized();
        return Quat(0, v.x, v.y, v.z);
    }
    Quat q(w, v.x, v.y, v.z); q.normalize(); return q;
}

// Project with ORTHO and keep depth so we can cull/clamp to the front hemisphere.
struct ProjPt {
    int x, y;
    float z;
    bool ok;
};
static inline ProjPt projectOrtho(const Quat& q, const Vec3& p, int cx, int cy) {
    Vec3 pc = q.rotate(p);
    ProjPt r;
    r.x = cx + (int)std::lround(pc.x);
    r.y = cy + (int)std::lround(pc.y);
    r.z = pc.z;  // view dir = -Z; front = z <= 0
    r.ok = true;
    return r;
}

// ---------- Startup-stable render transform (shared by sphere & labels) ----------
static inline float wrapPi(float a) {
    while (a >  M_PI) a -= 2.0f*M_PI;
    while (a <= -M_PI) a += 2.0f*M_PI;
    return a;
}

// Build a stable render quaternion for a "world view" compass.
// - Tilt is based ONLY on gravity, keeping the globe level with the world.
// - Yaw is based ONLY on the magnetic heading.
static Quat buildSmoothedRenderQuat(const Quat& /* attitude unused */) {
    static bool init = false;
    static Quat qRenderFilt(1, 0, 0, 0);

    // 1. Calculate Tilt based on Gravity
    // We want the globe's North Pole (+Y) to point opposite to gravity.
    // The screen's "up" is -Y, so we align the screen's up with anti-gravity.
    Vec3 g_body = GetGravityForRenderer();
    Vec3 anti_g = g_body * -1.0f;
    Quat qTilt = quatBetweenUnit(Vec3(0, -1, 0), anti_g.normalized());

    // 2. Calculate Yaw based on Heading
    // We rotate the globe around its pole axis (now Y) by the heading.
    float heading_rad = GetHeadingRadiansForRenderer();
    Quat qYaw = Quat::fromAxisAngle(Vec3(0, 1, 0), -heading_rad); // -rad for clockwise heading

    // 3. Combine them: First orient to gravity, then rotate to North.
    Quat qRender = qTilt * qYaw;

    // 4. Apply smoothing to the final quaternion
    const float alpha = init ? 0.25f : 1.0f;

    // NLERP for smooth animation
    if (qRender.dot(qRenderFilt) < 0.0f) {
        // Negate the quaternion to ensure the shortest rotational path
        qRenderFilt.w = -qRenderFilt.w;
        qRenderFilt.x = -qRenderFilt.x;
        qRenderFilt.y = -qRenderFilt.y;
        qRenderFilt.z = -qRenderFilt.z;
    }
    Quat qBlend(
        (1.0f - alpha) * qRenderFilt.w + alpha * qRender.w,
        (1.0f - alpha) * qRenderFilt.x + alpha * qRender.x,
        (1.0f - alpha) * qRenderFilt.y + alpha * qRender.y,
        (1.0f - alpha) * qRenderFilt.z + alpha * qRender.z
    );
    qBlend.normalize();
    qRenderFilt = qBlend;

    init = true;
    return qRenderFilt;
}

// Draw a ring but only the parts on the FRONT hemisphere.
// If a segment crosses the rim (z==0), we clip to the rim for a clean edge.
static inline void drawPolylineFront(OLEDDisplay* d, const Quat& q,
                                     int cx, int cy, const Vec3* pts, int count,
                                     bool drawDots /*filled verts on front*/)
{
    if (count < 2) return;
    ProjPt prev = projectOrtho(q, pts[0], cx, cy);
    bool prevFront = prev.ok && (prev.z <= 0.0f);

    // Optional vertex dot for first point
    if (drawDots && prevFront) d->fillCircle(prev.x, prev.y, 1);

    for (int i = 1; i < count; ++i) {
        ProjPt curr = projectOrtho(q, pts[i], cx, cy);
        bool currFront = curr.ok && (curr.z <= 0.0f);

        if (prevFront && currFront) {
            d->drawLine(prev.x, prev.y, curr.x, curr.y);
        } else if (prevFront != currFront) {
            // Clip to z=0 in MODEL space (linear on segment), then project once.
            const Vec3& A = pts[i-1];
            const Vec3& B = pts[i];
            // Solve for t where z crosses 0 after rotation:
            // We approximate by interpolating in model space and projecting.
            // Find t using depths we already computed (prev.z, curr.z).
            float denom = (prev.z - curr.z);
            float t = (fabsf(denom) > 1e-6f) ? (prev.z / denom) : 0.0f;
            if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
            Vec3 M = Vec3(A.x + t*(B.x - A.x),
                          A.y + t*(B.y - A.y),
                          A.z + t*(B.z - A.z));
            ProjPt clip = projectOrtho(q, M, cx, cy);

            if (prevFront) d->drawLine(prev.x, prev.y, clip.x, clip.y);
            else           d->drawLine(clip.x, clip.y, curr.x, curr.y);
        }

        if (drawDots && currFront) d->fillCircle(curr.x, curr.y, 1);

        prev = curr;
        prevFront = currFront;
    }
}

} // namespace
    
namespace graphics
{
namespace CompassRenderer
{

namespace {
// Rotate + project (ORTHOGRAPHIC): ignore depth, no foreshortening
inline bool projectPoint(const Quat& qAtt, const Vec3& pModel,
                         int cx, int cy, int& outX, int& outY)
{
    Vec3 pc = qAtt.rotate(pModel); // Y up in model -> Y increases downward on screen
    outX = cx + (int)std::lround(pc.x);
    outY = cy + (int)std::lround(pc.y);
    return true;
}

// Draw one closed polyline (front-hemisphere only; simple reject if behind)
inline void drawPolylineProjected(OLEDDisplay* d, const Quat& qAtt,
                                  int cx, int cy,
                                  const Vec3* pts, int count)
{
    int px=0, py=0; bool hasPrev=false;
    for (int i=0; i<count; ++i) {
        int sx, sy;
        if (projectPoint(qAtt, pts[i], cx, cy, sx, sy)) {
            if (hasPrev) d->drawLine(px, py, sx, sy);
            px=sx; py=sy; hasPrev=true;
        } else {
            hasPrev=false;
        }
    }
}
} // anon

void drawCenterNeedle3D(OLEDDisplay* display, int16_t cx, int16_t cy, uint16_t radius,
                        const Quat& attitude, float bearingRad, float elevRad)
{
    const uint16_t rDraw   = (uint16_t)std::max<int>(1, (int)radius);
    const int16_t  cxShift = (int16_t)(cx - (int)(rDraw * 0.14f)); // same left shift

    // Build the shared render transform, then CANCEL yaw so the needle is ring-locked
    const Quat  qFull     = buildSmoothedRenderQuat(attitude);     // (tilt * yaw(-heading))
    const float heading   = GetHeadingRadiansForRenderer();        // 0 = North, +CW
    const Quat  qYawInv   = Quat::fromAxisAngle(Vec3(0,1,0), +heading);
    const Quat  qTiltOnly = qFull * qYawInv;                       // remove yaw

    // Relative bearing (same convention as the little compass): (bearing - heading), wrapped
    float rel = bearingRad - heading;
    while (rel >  M_PI) rel -= 2.0f * M_PI;
    while (rel <= -M_PI) rel += 2.0f * M_PI;

    // Target vector in COMPASS frame (X=East, Y=Up, Z=North-on-compass).
    // With camera forward = -Z, use -cos(rel)*cos(elev) for Z.
    const float ca = std::cos(rel), sa = std::sin(rel);
    const float ce = std::cos(elevRad), se = std::sin(elevRad);
    Vec3 vComp(sa*ce, se, +ca*ce);

    // Camera-space forward unit vector (tilt only)
    Vec3 vCam = qTiltOnly.rotate(vComp);
    float n3 = std::sqrt(vCam.x*vCam.x + vCam.y*vCam.y + vCam.z*vCam.z);
    if (n3 < 1e-6f) return;
    vCam.x/=n3; vCam.y/=n3; vCam.z/=n3;

    // Compass-locked left/right axis: Up × Forward (with pole fallback), then tilt to screen
    Vec3 upC(0.f, 1.f, 0.f);
    Vec3 pComp = upC.cross(vComp);
    float pn = std::sqrt(pComp.x*pComp.x + pComp.y*pComp.y + pComp.z*pComp.z);
    if (pn < 1e-6f) {
        pComp = Vec3(1.f,0.f,0.f).cross(vComp);
        pn = std::sqrt(pComp.x*pComp.x + pComp.y*pComp.y + pComp.z*pComp.z);
        if (pn < 1e-6f) return;
    }
    pComp.x/=pn; pComp.y/=pn; pComp.z/=pn;

    Vec3 pCam = qTiltOnly.rotate(pComp);
    float lenP = std::sqrt(pCam.x*pCam.x + pCam.y*pCam.y);
    const float px = (lenP > 1e-6f) ? (pCam.x/lenP) : 1.f;
    const float py = (lenP > 1e-6f) ? (pCam.y/lenP) : 0.f;

    // ---------------- geometry (unchanged) ----------------
    const float frontInsetFrac = 0.015f;
    const float edgeBackFrac   = 0.75f;
    const float backSurfaceInsetFrac = 0.015f;

    const float rFrontEdge = rDraw * (1.f - frontInsetFrac);
    const float rBackEdge  = rDraw * edgeBackFrac;
    const float rBackSurf  = rDraw * (1.f - backSurfaceInsetFrac);

    const int16_t xEfront = cxShift + (int16_t)std::lround(+rFrontEdge * vCam.x);
    const int16_t yEfront = cy      + (int16_t)std::lround(+rFrontEdge * vCam.y);
    const int16_t xEback  = cxShift + (int16_t)std::lround(-rBackEdge  * vCam.x);
    const int16_t yEback  = cy      + (int16_t)std::lround(-rBackEdge  * vCam.y);

    const float backSpread = rDraw * 0.38f;
    const int16_t xBL = cxShift + (int16_t)std::lround(-rBackSurf * vCam.x - backSpread * px);
    const int16_t yBL = cy      + (int16_t)std::lround(-rBackSurf * vCam.y - backSpread * py);
    const int16_t xBR = cxShift + (int16_t)std::lround(-rBackSurf * vCam.x + backSpread * px);
    const int16_t yBR = cy      + (int16_t)std::lround(-rBackSurf * vCam.y + backSpread * py);

    display->drawTriangle(xEfront, yEfront, xEback, yEback, xBL, yBL);
#ifdef USE_EINK
    display->drawTriangle(xEfront, yEfront, xBR, yBR, xEback, yEback);
#else
    display->fillTriangle(xEfront, yEfront, xBR, yBR, xEback, yEback);
#endif
}


void drawCompassSphere(OLEDDisplay* d, int16_t cx, int16_t cy,
                       uint16_t radius, const Quat& attitude)
{
    // size & position (keep in sync with drawNodeHeading)
    const uint16_t rDraw   = (uint16_t)std::max<int>(1, (int)(radius)); 
    const int16_t  cxShift = (int16_t)(cx - (int)(rDraw * 0.14f));              // a bit left
    const int      LONGS = 12, LATS = 8, SEG = 48;

    // Use the shared, smoothed render transform (tilt-only + yaw(-heading))
    Quat qRender = buildSmoothedRenderQuat(attitude);

    // -------- ORTHOGRAPHIC helpers already in your file --------
    // projectPoint(qAtt, pModel, cx, cy, outX, outY)
    // drawPolylineProjected(d, qAtt, cx, cy, pts, count)

    // --- Always-on border around the sphere (draw last so it sits on top) ---
    d->drawCircle(cxShift, cy, rDraw);

    // Meridians
    for (int m = 0; m < LONGS; ++m) {
        float phi = (2.0f * (float)M_PI * m) / LONGS;
        Vec3 ring[SEG + 1];
        for (int i = 0; i <= SEG; ++i) {
            float t = ((float)i / SEG) * (float)M_PI - (float)M_PI/2.0f;
            float c = std::cos(t), s = std::sin(t);
            ring[i] = Vec3(c*std::cos(phi), s, -c*std::sin(phi)) * (float)rDraw;
        }
        drawPolylineFront(d, qRender, cxShift, cy, ring, SEG + 1, /*drawDots=*/false);
    }

    // Parallels
    for (int n = 1; n <= LATS; ++n) {
        float theta = ((float)n / (LATS + 1)) * (float)M_PI - (float)M_PI/2.0f;
        float s = std::sin(theta), c = std::cos(theta);
        Vec3 ring[SEG + 1];
        for (int i = 0; i <= SEG; ++i) {
            float a = (2.0f * (float)M_PI * i) / SEG;
            ring[i] = Vec3(c*std::cos(a), s, -c*std::sin(a)) * (float)rDraw;
        }
        drawPolylineFront(d, qRender, cxShift, cy, ring, SEG + 1, /*drawDots=*/false);
    }

    // Equator (slightly thicker: draw twice; add vertex dots if desired)
    {
        Vec3 ring[SEG + 1];
        for (int i = 0; i <= SEG; ++i) {
            float a = (2.0f * (float)M_PI * i) / SEG;
            ring[i] = Vec3(std::cos(a), 0.0f, -std::sin(a)) * (float)rDraw;
        }
        drawPolylineFront(d, qRender, cxShift, cy, ring, SEG + 1, /*drawDots=*/true);
        drawPolylineFront(d, qRender, cxShift, cy, ring, SEG + 1, /*drawDots=*/false); // overdraw = thicker
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
    // Show the compass heading (not implemented in original)
    // This could draw a "N" indicator or north arrow
    // For now, we'll draw a simple north indicator
    // const float radius = 17.0f;
    if (isHighResolution) {
        radius += 4;
    }
    Point north(0, -radius);
    if (uiconfig.compass_mode != meshtastic_CompassMode_FIXED_RING)
        north.rotate(-myHeading);
    north.translate(compassX, compassY);

    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    display->setColor(BLACK);
    if (isHighResolution) {
        display->fillRect(north.x - 8, north.y - 1, display->getStringWidth("N") + 3, FONT_HEIGHT_SMALL - 6);
    } else {
        display->fillRect(north.x - 4, north.y - 1, display->getStringWidth("N") + 2, FONT_HEIGHT_SMALL - 6);
    }
    display->setColor(WHITE);
    display->drawString(north.x, north.y - 3, "N");
}

void drawNodeHeading(OLEDDisplay *display, int16_t compassX, int16_t compassY,
                     uint16_t compassDiam, float /*headingRadian unused*/)
{
    const Quat attitude = GetAttitudeForRenderer();
    const uint16_t radius = compassDiam / 2;

    // draw the globe first
    drawCompassSphere(display, compassX, compassY, radius, attitude);

    // same geometry constants used by the globe
    const uint16_t rDraw   = (uint16_t)std::max<int>(1, (int)(radius));
    const int16_t  cxShift = (int16_t)(compassX - (int)(rDraw * 0.14f));
    const int16_t  cy      = compassY;
    const float    rLabel  = rDraw * 1.06f;  // just outside equator

    // Same smoothed transform as the sphere so labels stay locked to the equator
    const Quat qLabel = buildSmoothedRenderQuat(attitude);

    // Choose your basis (forward = −Z). If your build uses +Z forward, flip signs as discussed.
    struct Mark { const char* t; Vec3 p; };
    Mark marks[] = {
        {"N", Vec3( 0, 0, 1)}, {"E", Vec3( 1, 0, 0)},
        {"S", Vec3( 0, 0,-1)}, {"W", Vec3(-1, 0, 0)},
    };

    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_CENTER);

    for (auto& m : marks) {
        int x, y;
        if (projectPoint(qLabel, m.p * (float)rLabel, cxShift, cy, x, y)) {
            display->drawString(x, y - (FONT_HEIGHT_SMALL / 2), m.t);
        }
    }
}

void drawArrowToNode(OLEDDisplay *display, int16_t x, int16_t y, int16_t size, float bearing)
{
    float radians = bearing * DEG_TO_RAD;

    Point tip(0, -size / 2);
    Point left(-size / 6, size / 4);
    Point right(size / 6, size / 4);
    Point tail(0, size / 4.5);

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

} // namespace CompassRenderer
} // namespace graphics
