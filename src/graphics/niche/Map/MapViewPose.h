#pragma once

// Where the map's camera is: north-up, turned so the direction of travel is up, or turned and tilted back like a car
// navigator. Maps world pixels to the view and back, one screen row at a time for the basemap. Dependency-free so the
// geometry is unit-tested.

#include <math.h>

namespace NicheGraphics::MapTiles
{

struct ViewPose {
    double centerX = 0, centerY = 0; // world pixels at the map's zoom: the point that sits at the anchor
    float anchorX = 0, anchorY = 0;  // in view pixels
    bool turned = false;
    float cosH = 1, sinH = 0; // heading, clockwise from north: that direction is up
    bool tilted = false;
    float depth = 1;          // camera distance, in view pixels; larger is a flatter perspective
    float sinT = 0, cosT = 1; // tilt back from straight down
    float maxAhead = 1e30f;   // ground further ahead than this, in world pixels, is past the horizon

    static ViewPose northUp(double cx, double cy, float ax, float ay)
    {
        ViewPose p;
        p.centerX = cx;
        p.centerY = cy;
        p.anchorX = ax;
        p.anchorY = ay;
        return p;
    }

    void setHeading(float headingRad)
    {
        turned = true;
        cosH = cosf(headingRad);
        sinH = sinf(headingRad);
    }

    // Leans the ground away by tiltRad, seen from depthPx back; nothing past aheadPx world pixels ahead is drawn.
    void setTilt(float tiltRad, float depthPx, float aheadPx)
    {
        tilted = tiltRad > 0.0f;
        sinT = sinf(tiltRad);
        cosT = cosf(tiltRad);
        depth = depthPx;
        maxAhead = aheadPx;
    }

    // World pixel to view pixel. False when it is behind the camera or past the horizon.
    bool toScreen(double wx, double wy, float &sx, float &sy) const
    {
        const float dx = (float)(wx - centerX), dy = (float)(wy - centerY);
        if (!turned) {
            sx = anchorX + dx;
            sy = anchorY + dy;
            return true;
        }
        const float u = dx * cosH + dy * sinH; // to the right of the direction of travel
        const float v = dx * sinH - dy * cosH; // ahead
        if (!tilted) {
            sx = anchorX + u;
            sy = anchorY - v;
            return true;
        }
        const float z = depth + v * sinT;
        if (z < depth * 0.05f || v > maxAhead)
            return false;
        sx = anchorX + depth * u / z;
        sy = anchorY - depth * v * cosT / z;
        return true;
    }

    // The ground under the left edge of view row sy, and how far one view pixel to the right moves across it.
    // `scale` is world pixels per view pixel along the row: above 1 the ground is being shrunk. False above the
    // horizon.
    bool rowToWorld(float sy, double &wx, double &wy, float &stepX, float &stepY, float &scale) const
    {
        const float up = anchorY - sy;
        float v = up;
        scale = 1.0f;
        if (tilted) {
            const float denom = depth * cosT - up * sinT;
            if (denom <= depth * 0.02f)
                return false;
            v = up * depth / denom;
            if (v > maxAhead)
                return false;
            scale = (depth + v * sinT) / depth;
        }
        const float u = -anchorX * scale;
        wx = centerX + u * cosH + v * sinH;
        wy = centerY + u * sinH - v * cosH;
        stepX = scale * cosH;
        stepY = scale * sinH;
        return true;
    }
};

} // namespace NicheGraphics::MapTiles
