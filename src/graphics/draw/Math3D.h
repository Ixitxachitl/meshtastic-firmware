#pragma once
#include <cmath> // Use C++ header for math functions

/**
 * @struct Vec3
 * @brief A simple 3D vector class with basic arithmetic operations.
 */
struct Vec3 {
    // --- Members ---
    float x, y, z;

    // --- Constructors ---
    Vec3() : x(0), y(0), z(0) {}
    Vec3(float X, float Y, float Z) : x(X), y(Y), z(Z) {}

    // --- Operators ---
    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }
    Vec3& operator+=(const Vec3& o) {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }

    // --- Methods ---
    float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const { return Vec3(y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x); }
    float norm() const { return sqrtf(x * x + y * y + z * z); }
    Vec3 normalized() const {
        float n = norm();
        return (n > 1e-6f) ? (*this) * (1.0f / n) : Vec3(0, 0, 0);
    }
};


/**
 * @struct Quat
 * @brief A quaternion class for handling 3D rotations. Follows Hamilton convention (w, x, y, z).
 */
struct Quat {
    // --- Members ---
    float w, x, y, z;

    // --- Constructors & Factories ---
    Quat() : w(1), x(0), y(0), z(0) {}
    Quat(float W, float X, float Y, float Z) : w(W), x(X), y(Y), z(Z) {}

    static Quat identity() { return Quat(); }
    static Quat fromAxisAngle(const Vec3& axis, float rad) {
        float a2 = rad * 0.5f;
        float s = sinf(a2);
        return Quat(cosf(a2), axis.x * s, axis.y * s, axis.z * s);
    }
    static Quat fromOmega(const Vec3& omegaRadPerSec, float dt) {
        float ang = omegaRadPerSec.norm() * dt;
        if (ang < 1e-9f) {
            return Quat::identity();
        }
        Vec3 ax = omegaRadPerSec.normalized();
        return fromAxisAngle(ax, ang);
    }

    // --- Operators ---
    Quat operator*(const Quat& o) const {
        return Quat(w * o.w - x * o.x - y * o.y - z * o.z,
                    w * o.x + x * o.w + y * o.z - z * o.y,
                    w * o.y - x * o.z + y * o.w + z * o.x,
                    w * o.z + x * o.y - y * o.x + z * o.w);
    }

    // --- Methods ---
    void normalize() {
        float n = sqrtf(w * w + x * x + y * y + z * z);
        if (n > 1e-9f) {
            w /= n;
            x /= n;
            y /= n;
            z /= n;
        }
    }

    // Rotates a vector using the formula: v' = q * v * q^-1
    Vec3 rotate(const Vec3& v) const {
        Vec3 qv(x, y, z);
        Vec3 t = qv.cross(v) * 2.0f;
        return v + t * w + qv.cross(t);
    }

    float dot(const Quat& other) const {
        return w * other.w + x * other.x + y * other.y + z * other.z;
    }

    Quat conjugate() const {
        return Quat(w, -x, -y, -z);
    }

    // For a unit quaternion, the inverse is its conjugate.
    Quat inverse() const {
        return conjugate();
    }
};


/**
 * @brief Projects a 3D point into 2D space using a simple perspective projection.
 *
 * Assumes a camera looking down the -Z axis, with +Y as up and +X as right.
 *
 * @param p The 3D point in camera space.
 * @param f The focal length.
 * @param zNear The near clipping plane.
 * @param outX Reference to the output 2D X coordinate.
 * @param outY Reference to the output 2D Y coordinate.
 * @param outDepth Reference to the output depth value.
 * @return True if the point is in front of the near clipping plane, false otherwise.
 */
inline bool projectPerspective(const Vec3& p, float f, float zNear,
                               float& outX, float& outY, float& outDepth) {
    // Reject points behind the near clipping plane
    if (p.z > -zNear) {
        return false;
    }
    
    // Perspective division
    outX = (f * p.x) / -p.z;
    outY = (f * p.y) / -p.z;
    outDepth = -p.z;
    
    return true;
}