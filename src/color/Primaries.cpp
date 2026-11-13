// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "color/Primaries.hpp"

#include <wayland-protocols/color-management-v1-enum.h>

namespace RiftHome::Color
{

namespace
{

using M3 = std::array<double, 9>;

constexpr double kD65x = 0.3127, kD65y = 0.3290;
constexpr double kCx = 0.310, kCy = 0.316; // illuminant C

M3 mul(const M3 &a, const M3 &b)
{
    M3 r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            for (int k = 0; k < 3; ++k) {
                r[size_t(i * 3 + j)] += a[size_t(i * 3 + k)] * b[size_t(k * 3 + j)];
            }
        }
    }
    return r;
}

std::array<double, 3> apply(const M3 &m, const std::array<double, 3> &v)
{
    return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2], m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
}

M3 inverse(const M3 &m)
{
    const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    return {(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det,
            (f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det,
            (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det};
}

std::array<double, 3> whiteXyz(double x, double y)
{
    return {x / y, 1.0, (1.0 - x - y) / y};
}

M3 toXyz(const Chromaticities &c)
{
    // Columns: each primary's XYZ at Y = 1, scaled so the three add up to the white.
    const auto column = [](double x, double y) {
        return std::array<double, 3>{x / y, 1.0, (1.0 - x - y) / y};
    };
    const auto r = column(c.rx, c.ry), g = column(c.gx, c.gy), b = column(c.bx, c.by);
    const M3 p{r[0], g[0], b[0], r[1], g[1], b[1], r[2], g[2], b[2]};
    const auto s = apply(inverse(p), whiteXyz(c.wx, c.wy));
    return {p[0] * s[0], p[1] * s[1], p[2] * s[2], p[3] * s[0], p[4] * s[1], p[5] * s[2], p[6] * s[0], p[7] * s[1], p[8] * s[2]};
}

//! Bradford adaptation from one white to another (XYZ to XYZ).
M3 bradford(double fromX, double fromY, double toX, double toY)
{
    const M3 b{0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296};
    const auto from = apply(b, whiteXyz(fromX, fromY));
    const auto to = apply(b, whiteXyz(toX, toY));
    const M3 scale{to[0] / from[0], 0, 0, 0, to[1] / from[1], 0, 0, 0, to[2] / from[2]};
    return mul(inverse(b), mul(scale, b));
}

Matrix3 toFloat(const M3 &m)
{
    Matrix3 r{};
    for (size_t i = 0; i < 9; ++i) {
        r[i] = float(m[i]);
    }
    return r;
}

Chromaticities make(double rx, double ry, double gx, double gy, double bx, double by, double wx, double wy)
{
    return {float(rx), float(ry), float(gx), float(gy), float(bx), float(by), float(wx), float(wy)};
}

} // namespace

Chromaticities bt709()
{
    return make(0.64, 0.33, 0.30, 0.60, 0.15, 0.06, kD65x, kD65y);
}

Chromaticities bt2020()
{
    return make(0.708, 0.292, 0.170, 0.797, 0.131, 0.046, kD65x, kD65y);
}

std::optional<Chromaticities> namedPrimaries(uint32_t primariesNamed)
{
    switch (primariesNamed) {
    case WP_COLOR_MANAGER_V1_PRIMARIES_SRGB:
        return bt709();
    case WP_COLOR_MANAGER_V1_PRIMARIES_PAL_M:
        return make(0.67, 0.33, 0.21, 0.71, 0.14, 0.08, kCx, kCy);
    case WP_COLOR_MANAGER_V1_PRIMARIES_PAL:
        return make(0.64, 0.33, 0.29, 0.60, 0.15, 0.06, kD65x, kD65y);
    case WP_COLOR_MANAGER_V1_PRIMARIES_NTSC:
        return make(0.630, 0.340, 0.310, 0.595, 0.155, 0.070, kD65x, kD65y);
    case WP_COLOR_MANAGER_V1_PRIMARIES_GENERIC_FILM:
        return make(0.681, 0.319, 0.243, 0.692, 0.145, 0.049, kCx, kCy);
    case WP_COLOR_MANAGER_V1_PRIMARIES_BT2020:
        return bt2020();
    case WP_COLOR_MANAGER_V1_PRIMARIES_CIE1931_XYZ:
        return make(1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0);
    case WP_COLOR_MANAGER_V1_PRIMARIES_DCI_P3:
        return make(0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.314, 0.351);
    case WP_COLOR_MANAGER_V1_PRIMARIES_DISPLAY_P3:
        return make(0.680, 0.320, 0.265, 0.690, 0.150, 0.060, kD65x, kD65y);
    case WP_COLOR_MANAGER_V1_PRIMARIES_ADOBE_RGB:
        return make(0.64, 0.33, 0.21, 0.71, 0.15, 0.06, kD65x, kD65y);
    default:
        return std::nullopt;
    }
}

Matrix3 rgbToXyz(const Chromaticities &c)
{
    return toFloat(toXyz(c));
}

Matrix3 conversion(const Chromaticities &from, const Chromaticities &to)
{
    M3 m = toXyz(from);
    if (from.wx != to.wx || from.wy != to.wy) {
        m = mul(bradford(from.wx, from.wy, to.wx, to.wy), m);
    }
    return toFloat(mul(inverse(toXyz(to)), m));
}

std::array<float, 3> luminanceWeights(const Chromaticities &c)
{
    const M3 m = toXyz(c);
    return {float(m[3]), float(m[4]), float(m[5])};
}

} // namespace RiftHome::Color
