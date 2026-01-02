#pragma once
#include "Math3D.h"
#include <stdint.h>


class AttitudeFilter {
public:
void reset(){ q_ = Quat::identity(); gLP_ = Vec3(0,0,1); bias_ = Vec3(0,0,0); lastUs_ = 0; }


// dt is computed internally from nowUs if nowUs!=0, otherwise you may pass dt>0 and nowUs=0
void update(float ax, float ay, float az, float gx_dps, float gy_dps, float gz_dps,
uint32_t nowUs = 0, float explicitDt = 0.0f){
// 1) time step
float dt;
if (nowUs){ if (!lastUs_) lastUs_ = nowUs; dt = (nowUs - lastUs_) * 1e-6f; lastUs_ = nowUs; }
else { dt = explicitDt; }
if (dt <= 0.0f) return;


// 2) gravity low-pass when |a|≈1g
const float G = 9.80665f; // raw ax..az expected in g? If already g-units, set G=1.0f
float axg = ax, ayg = ay, azg = az; // assume caller feeds in units of 1g
float amag = sqrtf(axg*axg + ayg*ayg + azg*azg);
const float TOL = 0.20f; // 1g ±20%
if (fabsf(amag - 1.0f) < TOL) {
const float alpha = 0.02f;
gLP_.x += alpha*(axg - gLP_.x);
gLP_.y += alpha*(ayg - gLP_.y);
gLP_.z += alpha*(azg - gLP_.z);
float n = gLP_.norm(); if (n>1e-3f){ gLP_ = gLP_*(1.0f/n); }
}


// 3) gyro minus bias (convert dps→rad/s)
const float D2R = 0.017453292519943295f;
Vec3 gyro((gx_dps - bias_.x)*D2R, (gy_dps - bias_.y)*D2R, (gz_dps - bias_.z)*D2R);


// 4) integrate small rotation
Quat dq = Quat::fromOmega(gyro, dt);
q_ = q_ * dq; q_.normalize();


// 5) complementary correction for roll/pitch using gravity
// Find gravity predicted by quaternion (body up vector is q^-1 * (0,0,1) * q → same as rotate)
Vec3 upPred = q_.rotate(Vec3(0,0,1));
// Error axis to rotate upPred toward measured gLP_: e = upPred × gLP_
Vec3 e = upPred.cross(gLP_);
// Apply a tiny correction proportional to error (like Mahony without magnetometer)
const float KP = 2.0f; // P gain (tune)
Vec3 corr = e * KP;
Quat dqCorr = Quat::fromOmega(corr, dt);
q_ = dqCorr * q_; q_.normalize();


// 6) bias trim when still-ish
if (fabsf(gx_dps)+fabsf(gy_dps)+fabsf(gz_dps) < 1.0f && fabsf(amag-1.0f) < TOL){
const float KBIAS = 0.0015f; // tune
bias_.x += (gx_dps - bias_.x) * KBIAS;
bias_.y += (gy_dps - bias_.y) * KBIAS;
bias_.z += (gz_dps - bias_.z) * KBIAS;
}


// 7) preserve yaw as rotation about gravity: project gyro onto ĝ and accumulate external yaw if caller needs it
// (Optional: expose yawRateAboutG if desired)
}


Quat quat() const { return q_; }
Vec3 gravityBody() const { return gLP_; }
Vec3 biasDps() const { return bias_; }


private:
Quat q_;
Vec3 gLP_; // measured gravity direction in body frame
Vec3 bias_; // gyro bias in dps
uint32_t lastUs_ = 0;
};