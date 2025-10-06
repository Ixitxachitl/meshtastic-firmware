#pragma once
#include <math.h>


struct Vec3 {
float x, y, z;
Vec3(): x(0), y(0), z(0) {}
Vec3(float X, float Y, float Z): x(X), y(Y), z(Z) {}
Vec3 operator+(const Vec3& o) const { return Vec3(x+o.x, y+o.y, z+o.z); }
Vec3 operator-(const Vec3& o) const { return Vec3(x-o.x, y-o.y, z-o.z); }
Vec3 operator*(float s) const { return Vec3(x*s, y*s, z*s); }
Vec3& operator+=(const Vec3& o){ x+=o.x; y+=o.y; z+=o.z; return *this; }
float dot(const Vec3& o) const { return x*o.x + y*o.y + z*o.z; }
Vec3 cross(const Vec3& o) const { return Vec3(y*o.z - z*o.y, z*o.x - x*o.z, x*o.y - y*o.x); }
float norm() const { return sqrtf(x*x + y*y + z*z); }
Vec3 normalized() const { float n = norm(); return (n>1e-6f) ? (*this)*(1.0f/n) : Vec3(0,0,0); }
};


struct Quat {
// w + xi + yj + zk (Hamilton)
float w, x, y, z;
Quat(): w(1), x(0), y(0), z(0) {}
Quat(float W,float X,float Y,float Z): w(W),x(X),y(Y),z(Z) {}
static Quat identity(){ return Quat(); }
static Quat fromAxisAngle(const Vec3& axis, float rad){
float a2 = rad*0.5f; float s = sinf(a2); return Quat(cosf(a2), axis.x*s, axis.y*s, axis.z*s);
}
static Quat fromOmega(const Vec3& omegaRadPerSec, float dt){
float ang = omegaRadPerSec.norm() * dt;
if (ang < 1e-9f) return Quat::identity();
Vec3 ax = omegaRadPerSec.normalized();
return fromAxisAngle(ax, ang);
}
Quat operator*(const Quat& o) const {
return Quat(
w*o.w - x*o.x - y*o.y - z*o.z,
w*o.x + x*o.w + y*o.z - z*o.y,
w*o.y - x*o.z + y*o.w + z*o.x,
w*o.z + x*o.y - y*o.x + z*o.w
);
}
void normalize(){
float n = sqrtf(w*w + x*x + y*y + z*z);
if (n > 1e-9f){ w/=n; x/=n; y/=n; z/=n; }
}
Vec3 rotate(const Vec3& v) const { // q * v * q^-1
Vec3 qv(x,y,z);
Vec3 t = qv.cross(v) * 2.0f;
return v + t*w + qv.cross(t);
}
};


// Simple perspective project (camera looking -Z, y up, x right)
inline bool projectPerspective(const Vec3& p, float f, float zNear,
float& outX, float& outY, float& outDepth){
// reject behind near plane
if (p.z > -zNear) return false; // we expect ring in -Z
outX = (f * p.x) / -p.z; // divide by depth
outY = (f * p.y) / -p.z;
outDepth = -p.z;
return true;
}