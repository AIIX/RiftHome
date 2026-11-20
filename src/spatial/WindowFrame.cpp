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
    // The padlock at the slider's left end, as far from it as the flat dot on the right.
    f.lock = {f.curve.x - f.curve.width / 2.0f - 0.02f * u - kLockDot * u / 2.0f, f.curve.y, kLockDot * u, kLockDot * u};
    // Cinema mode's shapes: a row under the grab bar, centred.
    const float shapeHeight = kAspectHeight * u;
    const float originalWidth = kAspectOriginalButton * u;
    const float shapeWidth = kAspectButton * u;
    f.aspectBar = {0.0f, f.grab.y - bar / 2.0f - 0.6f * gap - shapeHeight / 2.0f, originalWidth + (kAspects - 1) * shapeWidth, shapeHeight};
    float edge = f.aspectBar.x - f.aspectBar.width / 2.0f; // each button's left edge, left to right
    for (int i = 0; i < kAspects; ++i) {
        const float w = i == 0 ? originalWidth : shapeWidth;
        f.aspects[i] = {edge + w / 2.0f, f.aspectBar.y, w, shapeHeight};
        edge += w;
    }
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

void FrameLayout::showAspects()
{
    aspectsShown = true;
    // The frame shows near the row too (it sits below the usual margin).
    const float bottom = hover.y - hover.height / 2.0f;
    const float needed = aspectBar.y - aspectBar.height / 2.0f - 0.03f * unit;
    if (needed < bottom) {
        hover = {hover.x, (hover.y + hover.height / 2.0f + needed) / 2.0f, hover.width, hover.y + hover.height / 2.0f - needed};
    }
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
    if (aspectsShown && aspectBar.grown(0.0f, 0.004f * unit).contains(x, y)) { // before the grab bar's wide hit band
        for (int i = 0; i < kAspects; ++i) {
            if (aspects[i].grown(0.0f, 0.004f * unit).contains(x, y)) {
                return kAspectOrder[i];
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
    if (lock.grown(0.012f * unit, 0.024f * unit).contains(x, y)) {
        return FrameControl::Lock;
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

int aspectIndex(FrameControl control)
{
    for (int i = 0; i < kAspects; ++i) {
        if (kAspectOrder[i] == control) {
            return i;
        }
    }
    return -1;
}

std::pair<int, int> screenPixels(float aspect, int frameWidth, int frameHeight)
{
    if (aspect <= 0.0f) {
        return {frameWidth, frameHeight};
    }
    const auto even = [](float value) {
        return std::max(2, int(std::lround(value / 2.0f)) * 2);
    };
    if (aspect >= float(frameWidth) / float(frameHeight)) {
        return {frameWidth, even(float(frameWidth) / aspect)};
    }
    return {even(float(frameHeight) * aspect), frameHeight};
}

int shapeLit(int width, int height, int naturalWidth, int naturalHeight)
{
    if (width <= 0 || height <= 0) {
        return -1;
    }
    const float now = float(width) / float(height);
    const auto same = [now](float aspect) {
        return aspect > 0.0f && std::abs(now / aspect - 1.0f) < 0.01f;
    };
    if (naturalWidth > 0 && naturalHeight > 0 && same(float(naturalWidth) / float(naturalHeight))) {
        return 0;
    }
    for (int i = 1; i < kAspects; ++i) {
        if (same(kAspectValues[i])) {
            return i;
        }
    }
    return -1;
}

float shapeWidthInFrame(float aspect, float width, float height)
{
    if (aspect <= 0.0f || width <= 0.0f || height <= 0.0f) {
        return width;
    }
    constexpr float frame = 16.0f / 9.0f; // the 4K picture's shape
    const float frameWidth = width / height >= frame ? width : height * frame;
    return aspect >= frame ? frameWidth : frameWidth / frame * aspect;
}

} // namespace RiftHome::Spatial
