#pragma once

// The map tiles along a route, for Saved Routes > Download tiles: every tile the route passes through at each zoom,
// with a ring of neighbours either side. Dependency-free so the walk is unit-tested.

#include <math.h>
#include <stdint.h>

namespace NicheGraphics::MapTiles
{

class RouteTileWalker
{
  public:
    static constexpr int kMaxZooms = 8;

    // The route stays the caller's and must outlive the walk. `radius` tiles either side of the route are included.
    void start(const int32_t *latE7, const int32_t *lonE7, uint32_t count, const uint8_t *zooms, int zoomCount, int radius)
    {
        lat = latE7;
        lon = lonE7;
        points = count;
        zoomTotal = zoomCount < kMaxZooms ? zoomCount : kMaxZooms;
        for (int i = 0; i < zoomTotal; i++)
            zoomList[i] = zooms[i];
        this->radius = radius < 0 ? 0 : radius;
        zoomIndex = 0;
        beginZoom();
    }

    // The next tile, or false once the whole route has been walked at every zoom. Tiles recently given are not given
    // again; one further back may be, so the caller should still skip a tile it already has.
    bool next(int &z, int32_t &x, int32_t &y)
    {
        while (zoomIndex < zoomTotal) {
            // Neighbours of the current centre still to give.
            while (neighbour < side * side) {
                const int32_t nx = centerX + neighbour % side - radius, ny = centerY + neighbour / side - radius;
                neighbour++;
                if (ny < 0 || ny >= tilesPerSide)
                    continue;
                const int32_t wx = ((nx % tilesPerSide) + tilesPerSide) % tilesPerSide; // longitude wraps
                if (seen(wx, ny))
                    continue;
                remember(wx, ny);
                z = zoomList[zoomIndex];
                x = wx;
                y = ny;
                return true;
            }
            if (!advance()) {
                zoomIndex++;
                if (zoomIndex < zoomTotal)
                    beginZoom();
            }
        }
        return false;
    }

    // Fractional tile coordinates of a position at zoom z (Web Mercator).
    static void tileOf(int32_t latE7, int32_t lonE7, int z, double &fx, double &fy)
    {
        const double n = (double)(1u << z);
        double latDeg = latE7 * 1e-7;
        if (latDeg > 85.05112878)
            latDeg = 85.05112878;
        if (latDeg < -85.05112878)
            latDeg = -85.05112878;
        const double s = sin(latDeg * M_PI / 180.0);
        fx = (lonE7 * 1e-7 + 180.0) / 360.0 * n;
        fy = (0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * M_PI)) * n;
    }

  private:
    static constexpr int kRecent = 64;

    const int32_t *lat = nullptr, *lon = nullptr;
    uint32_t points = 0;
    uint8_t zoomList[kMaxZooms] = {};
    int zoomTotal = 0, zoomIndex = 0, radius = 0, side = 1;
    int32_t tilesPerSide = 1;

    // Walking the route: the segment from point `segment`, sampled `step` of `steps` along it.
    uint32_t segment = 0;
    int step = 0, steps = 0;
    int32_t centerX = 0, centerY = 0;
    int neighbour = 0;

    int32_t recentX[kRecent] = {}, recentY[kRecent] = {};
    int recentCount = 0, recentNext = 0;

    void beginZoom()
    {
        tilesPerSide = (int32_t)1 << zoomList[zoomIndex];
        side = 2 * radius + 1;
        segment = 0;
        step = 0;
        steps = 0;
        recentCount = 0;
        recentNext = 0;
        centerX = centerY = INT32_MIN; // so the first point always counts as a new centre
        neighbour = side * side;       // nothing pending until it does
        if (points == 0) {
            zoomIndex = zoomTotal;
            return;
        }
        double fx, fy;
        tileOf(lat[0], lon[0], zoomList[zoomIndex], fx, fy);
        setCentre(fx, fy);
    }

    // A new centre queues its neighbourhood; the same one again queues nothing.
    bool setCentre(double fx, double fy)
    {
        const int32_t cx = (int32_t)floor(fx), cy = (int32_t)floor(fy);
        if (cx == centerX && cy == centerY)
            return false;
        centerX = cx;
        centerY = cy;
        neighbour = 0;
        return true;
    }

    // To the next sample along the route; half a tile apart at most, so a long straight segment skips none. False at
    // the end of the route.
    bool advance()
    {
        while (segment + 1 < points) {
            double ax, ay, bx, by;
            tileOf(lat[segment], lon[segment], zoomList[zoomIndex], ax, ay);
            tileOf(lat[segment + 1], lon[segment + 1], zoomList[zoomIndex], bx, by);
            if (bx - ax > tilesPerSide / 2.0) // across the antimeridian: the short way round
                bx -= tilesPerSide;
            else if (ax - bx > tilesPerSide / 2.0)
                bx += tilesPerSide;
            if (steps == 0)
                steps = 1 + (int)(2.0 * fmax(fabs(bx - ax), fabs(by - ay)));
            if (step < steps) {
                step++;
                const double t = (double)step / steps;
                if (setCentre(ax + (bx - ax) * t, ay + (by - ay) * t))
                    return true;
                continue;
            }
            segment++;
            step = 0;
            steps = 0;
        }
        return false;
    }

    bool seen(int32_t x, int32_t y) const
    {
        for (int i = 0; i < recentCount; i++) {
            if (recentX[i] == x && recentY[i] == y)
                return true;
        }
        return false;
    }

    void remember(int32_t x, int32_t y)
    {
        recentX[recentNext] = x;
        recentY[recentNext] = y;
        recentNext = (recentNext + 1) % kRecent;
        if (recentCount < kRecent)
            recentCount++;
    }
};

} // namespace NicheGraphics::MapTiles
