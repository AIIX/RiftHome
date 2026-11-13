// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cmath>

namespace RiftHome::Spatial
{

//! OpenXR's conventions: metres, right-handed, +Y up, -Z forward.
struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vec3 operator+(Vec3 o) const
    {
        return {x + o.x, y + o.y, z + o.z};
    }
    Vec3 operator-(Vec3 o) const
    {
        return {x - o.x, y - o.y, z - o.z};
    }
    Vec3 operator*(float s) const
    {
        return {x * s, y * s, z * s};
    }
    float dot(Vec3 o) const
    {
        return x * o.x + y * o.y + z * o.z;
    }
    Vec3 cross(Vec3 o) const
    {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    float length() const
    {
        return std::sqrt(dot(*this));
    }
    Vec3 normalized() const
    {
        const float l = length();
        return l > 1e-6f ? *this * (1.0f / l) : Vec3{};
    }
};

struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;

    //! Rotation about +Y (counter-clockwise seen from above).
    static Quat fromYaw(float radians)
    {
        return {0.0f, std::sin(radians / 2.0f), 0.0f, std::cos(radians / 2.0f)};
    }
    //! Rotation about +X (positive turns -Z up, +Z down).
    static Quat fromPitch(float radians)
    {
        return {std::sin(radians / 2.0f), 0.0f, 0.0f, std::cos(radians / 2.0f)};
    }
    //! The rotation whose matrix has these columns (an orthonormal basis).
    static Quat fromBasis(Vec3 bx, Vec3 by, Vec3 bz)
    {
        const float tr = bx.x + by.y + bz.z;
        Quat q;
        if (tr > 0.0f) {
            const float s = std::sqrt(tr + 1.0f) * 2.0f;
            q = {(by.z - bz.y) / s, (bz.x - bx.z) / s, (bx.y - by.x) / s, 0.25f * s};
        } else if (bx.x > by.y && bx.x > bz.z) {
            const float s = std::sqrt(1.0f + bx.x - by.y - bz.z) * 2.0f;
            q = {0.25f * s, (by.x + bx.y) / s, (bz.x + bx.z) / s, (by.z - bz.y) / s};
        } else if (by.y > bz.z) {
            const float s = std::sqrt(1.0f + by.y - bx.x - bz.z) * 2.0f;
            q = {(by.x + bx.y) / s, 0.25f * s, (bz.y + by.z) / s, (bz.x - bx.z) / s};
        } else {
            const float s = std::sqrt(1.0f + bz.z - bx.x - by.y) * 2.0f;
            q = {(bz.x + bx.z) / s, (bz.y + by.z) / s, 0.25f * s, (bx.y - by.x) / s};
        }
        return q;
    }
    Quat operator*(const Quat &b) const
    {
        return {w * b.x + x * b.w + y * b.z - z * b.y, w * b.y - x * b.z + y * b.w + z * b.x,
                w * b.z + x * b.y - y * b.x + z * b.w, w * b.w - x * b.x - y * b.y - z * b.z};
    }
    Quat conjugate() const
    {
        return {-x, -y, -z, w};
    }
    Vec3 rotate(Vec3 v) const
    {
        const Vec3 u{x, y, z};
        return u * (2.0f * u.dot(v)) + v * (w * w - u.dot(u)) + u.cross(v) * (2.0f * w);
    }
};

struct Pose {
    Quat orientation;
    Vec3 position;

    Vec3 transform(Vec3 p) const
    {
        return position + orientation.rotate(p);
    }
    Pose operator*(const Pose &b) const
    {
        return {orientation * b.orientation, transform(b.position)};
    }
    Pose inverse() const
    {
        const Quat c = orientation.conjugate();
        return {c, c.rotate(position * -1.0f)};
    }
    Vec3 forward() const
    {
        return orientation.rotate({0.0f, 0.0f, -1.0f});
    }
};

constexpr float kPi = 3.14159265358979f;

inline float radians(float degrees)
{
    return degrees * kPi / 180.0f;
}

//! Azimuth of a direction: 0 straight ahead (-Z), positive to the right (+X).
inline float azimuthOf(Vec3 d)
{
    return std::atan2(d.x, -d.z);
}

//! The horizontal unit direction of an azimuth.
inline Vec3 directionOf(float azimuth)
{
    return {std::sin(azimuth), 0.0f, -std::cos(azimuth)};
}

//! Yaw of an upright window at @p position whose front (+Z) faces @p eye.
inline float facingYaw(Vec3 position, Vec3 eye)
{
    const Vec3 d = eye - position;
    return std::atan2(d.x, d.z);
}

//! Smallest signed difference of two angles.
inline float angleDifference(float a, float b)
{
    float d = std::fmod(a - b + kPi, 2.0f * kPi);
    if (d < 0.0f) {
        d += 2.0f * kPi;
    }
    return d - kPi;
}

//! The rotation a fraction @p t of the way from @p a to @p b, the shorter way round.
inline Quat slerp(Quat a, Quat b, float t)
{
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0.0f) {
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    float wa = 1.0f - t;
    float wb = t;
    if (d < 0.9995f) {
        const float theta = std::acos(d);
        const float sinTheta = std::sin(theta);
        wa = std::sin((1.0f - t) * theta) / sinTheta;
        wb = std::sin(t * theta) / sinTheta;
    }
    const Quat q{wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z, wa * a.w + wb * b.w};
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return n > 1e-6f ? Quat{q.x / n, q.y / n, q.z / n, q.w / n} : a;
}

} // namespace RiftHome::Spatial
