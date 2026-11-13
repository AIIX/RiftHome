// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "spatial/WindowFrame.hpp"

#include <algorithm>
#include <cmath>

namespace RiftHome::Spatial
{

FrameLayout FrameLayout::compute(float width, float height, float cornerRadius, float viewDistance)
{
    FrameLayout f;
    const float u = std::clamp(viewDistance / 1.6f, 0.6f, 2.5f);
    f.unit = u;
    f.windowWidth = width;
    f.windowHeight = height;
    f.cornerRadius = cornerRadius;
    const float gap = kGap * u;
    const float w = width / 2.0f;
    const float h = height / 2.0f;

    // The menu pill on the left, centred on the window's height.
    const float pillWidth = kButton * u;
    const float pillHeight = (kMenuButtons * kButton + 2.0f * kMenuPadding) * u;
    f.menu = {-w - gap - pillWidth / 2.0f, 0.0f, pillWidth, pillHeight};
    for (int i = 0; i < kMenuButtons; ++i) {
        const float top = pillHeight / 2.0f - kMenuPadding * u;
        f.buttons[i] = {f.menu.x, top - (i + 0.5f) * kButton * u, kButton * u, kButton * u};
    }
    // The tilt bar on the right (shorter), the grab bar underneath.
    const float bar = kBarThickness * u;
    f.tilt = {w + gap + bar / 2.0f, 0.0f, bar, std::clamp(0.3f * height, 0.08f * u, 0.2f * u)};
    f.resetTilt = {f.tilt.x, f.tilt.height / 2.0f + 0.014f * u + kDot * u / 2.0f, kDot * u, kDot * u};
    f.grab = {0.0f, -h - gap - bar / 2.0f, std::clamp(0.28f * width, 0.12f * u, 0.36f * u), bar};
    // The curvature slider above, its reset dot at the right end.
    f.curve = {0.0f, h + gap + bar / 2.0f, std::clamp(0.22f * width, 0.14f * u, 0.24f * u), bar};
    f.resetCurve = {f.curve.x + f.curve.width / 2.0f + 0.02f * u + kDot * u / 2.0f, f.curve.y, kDot * u, kDot * u};
    // The scale handle: an arc around the rounded corner, arms along both edges.
    const float cx = w - cornerRadius;
    const float cy = -h + cornerRadius;
    const float outer = cornerRadius + gap + bar;
    const float arm = kScaleArm * u;
    const float x0 = cx - arm;
    const float x1 = cx + outer;
    const float y0 = cy - outer;
    const float y1 = cy + arm;
    f.scale = {(x0 + x1) / 2.0f, (y0 + y1) / 2.0f, x1 - x0, y1 - y0};
    // The resize button just outside the top-right corner (at the bottom-left the menu pill ran over it
    // on shorter windows, and two L handles were confusing).
    const float dot = kResizeDot * u;
    f.resize = {w + 0.7f * gap, h + 0.7f * gap, dot, dot};
    // Near the window (and its controls): the frame shows.
    const float left = w + gap + pillWidth + 0.06f * u;
    const float right = w + gap + 0.08f * u;
    const float top = h + gap + 0.08f * u;
    const float bottom = h + gap + 0.08f * u;
    f.hover = {(right - left) / 2.0f, (top - bottom) / 2.0f, left + right, top + bottom};
    return f;
}

FrameControl FrameLayout::hit(float x, float y) const
{
    // The window itself is its content's (the scale handle's corner square reaches into it).
    if (std::fabs(x) <= windowWidth / 2.0f && std::fabs(y) <= windowHeight / 2.0f) {
        return FrameControl::None;
    }
    if (menu.grown(0.01f * unit, 0.0f).contains(x, y)) {
        for (int i = 0; i < kMenuButtons; ++i) {
            if (buttons[i].grown(0.01f * unit, 0.0f).contains(x, y)) {
                return kMenuOrder[i];
            }
        }
        return FrameControl::None;
    }
    const float grow = (kBarHitThickness - kBarThickness) / 2.0f * unit;
    if (scale.grown(0.02f * unit, 0.02f * unit).contains(x, y)) {
        return FrameControl::Scale;
    }
    if (resize.grown(0.014f * unit, 0.014f * unit).contains(x, y)) {
        return FrameControl::Resize;
    }
    if (resetTilt.grown(0.024f * unit, 0.016f * unit).contains(x, y)) { // small to see, easy to hit
        return FrameControl::ResetTilt;
    }
    if (resetCurve.grown(0.012f * unit, 0.024f * unit).contains(x, y)) {
        return FrameControl::ResetCurve;
    }
    if (curve.grown(0.008f * unit, grow).contains(x, y)) { // short of the reset dot
        return FrameControl::Curve;
    }
    if (tilt.grown(grow, 0.01f * unit).contains(x, y)) {
        return FrameControl::Tilt;
    }
    if (grab.grown(0.02f * unit, grow).contains(x, y)) {
        return FrameControl::Grab;
    }
    return FrameControl::None;
}

} // namespace RiftHome::Spatial
