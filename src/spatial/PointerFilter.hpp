// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "spatial/Math.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace RiftHome::Spatial
{

/*!
 * The 1 euro filter (Casiez, Roussel, Vogel, CHI 2012) for one value: strong smoothing while the value is still (hand
 * tremor), little lag while it moves fast. @p minCutoff in Hz, @p beta: how fast the cutoff rises with speed.
 */
class OneEuroFilter
{
public:
    OneEuroFilter(float minCutoff = 1.5f, float beta = 0.6f, float derivativeCutoff = 1.0f)
        : m_minCutoff(minCutoff)
        , m_beta(beta)
        , m_derivativeCutoff(derivativeCutoff)
    {
    }

    float filter(float value, float dt)
    {
        if (!m_started || dt <= 0.0f) {
            m_started = true;
            m_value = value;
            m_derivative = 0.0f;
            return value;
        }
        const float derivative = (value - m_value) / dt;
        m_derivative += alpha(m_derivativeCutoff, dt) * (derivative - m_derivative);
        const float cutoff = m_minCutoff + m_beta * std::fabs(m_derivative);
        m_value += alpha(cutoff, dt) * (value - m_value);
        return m_value;
    }
    void reset()
    {
        m_started = false;
    }

private:
    static float alpha(float cutoff, float dt)
    {
        const float tau = 1.0f / (2.0f * kPi * cutoff);
        return 1.0f / (1.0f + tau / dt);
    }

    float m_minCutoff;
    float m_beta;
    float m_derivativeCutoff;
    bool m_started = false;
    float m_value = 0.0f;
    float m_derivative = 0.0f;
};

//! The aim ray through 1 euro filters: its origin and its direction (renormalised).
class RayFilter
{
public:
    Pose filter(const Pose &aim, float dt)
    {
        const Vec3 o = aim.position;
        const Vec3 d = aim.forward();
        const Vec3 origin{m_o[0].filter(o.x, dt), m_o[1].filter(o.y, dt), m_o[2].filter(o.z, dt)};
        const Vec3 direction = Vec3{m_d[0].filter(d.x, dt), m_d[1].filter(d.y, dt), m_d[2].filter(d.z, dt)}.normalized();
        // Rebuild an orientation looking along the filtered direction, upright.
        const Vec3 z = direction * -1.0f;
        Vec3 x = Vec3{0.0f, 1.0f, 0.0f}.cross(z).normalized();
        if (x.length() < 0.5f) {
            x = {1.0f, 0.0f, 0.0f};
        }
        return {Quat::fromBasis(x, z.cross(x), z), origin};
    }
    void reset()
    {
        for (auto &f : m_o) {
            f.reset();
        }
        for (auto &f : m_d) {
            f.reset();
        }
    }

private:
    OneEuroFilter m_o[3]{{1.0f, 0.8f}, {1.0f, 0.8f}, {1.0f, 0.8f}};
    OneEuroFilter m_d[3]{{1.5f, 0.6f}, {1.5f, 0.6f}, {1.5f, 0.6f}};
};

/*!
 * Keeps a click where it was pressed: pulling a trigger turns the controller a little, and toolkits cancel a click
 * released off its button (Qt). The pointer stays on the press point until the ray has clearly turned away: in the
 * first moments of a press (a quick click) by more than @p quickDegrees, later (a slow drag) by more than
 * @p dragDegrees. Measured as the ray's angle, so a window's size and distance do not matter (quick
 * clicks of 130-210 ms turned the ray 2-7 degrees; a lock of 28 window pixels let 15 of 61 become drags).
 */
class ClickLock
{
public:
    explicit ClickLock(float quickDegrees = 6.0f, float dragDegrees = 1.5f, float quickSeconds = 0.35f)
        : m_quick(radians(quickDegrees))
        , m_drag(radians(dragDegrees))
        , m_quickSeconds(quickSeconds)
    {
    }
    void press(double x, double y, Vec3 direction)
    {
        m_locked = true;
        m_x = x;
        m_y = y;
        m_direction = direction.normalized();
        m_held = 0.0f;
    }
    void release()
    {
        m_locked = false;
    }
    bool locked() const
    {
        return m_locked;
    }
    //! The position to report for the ray's position (@p x, @p y) along @p direction, @p dt seconds after the last call.
    std::pair<double, double> filter(double x, double y, Vec3 direction, float dt)
    {
        if (m_locked) {
            m_held += dt;
            const float angle = std::acos(std::clamp(direction.normalized().dot(m_direction), -1.0f, 1.0f));
            if (angle <= (m_held < m_quickSeconds ? m_quick : m_drag)) {
                return {m_x, m_y};
            }
            m_locked = false; // a deliberate drag: follow the ray from now on
        }
        return {x, y};
    }

private:
    float m_quick;
    float m_drag;
    float m_quickSeconds;
    bool m_locked = false;
    double m_x = 0.0;
    double m_y = 0.0;
    Vec3 m_direction;
    float m_held = 0.0f;
};

} // namespace RiftHome::Spatial
