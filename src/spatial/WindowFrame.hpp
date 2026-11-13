// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cmath>

namespace RiftHome::Spatial
{

/*!
 * The controls around a window: on its left a vertical menu pill (follow, make main,
 * pin, size, keyboard, close), on its right a thin tilt bar with a reset dot above it, under it a grab bar, at
 * its bottom-right corner an L-shaped scale handle following the rounded corner; above it the curvature slider with a
 * reset dot at its right end.
 */
enum class FrameControl {
    None,
    Follow,
    Main,
    Pin,
    Size,
    Keyboard, //!< the virtual keyboard (its icon; the button is in the dock)
    Cinema, //!< the window alone, large, the room dimmed
    Minimize, //!< hidden in the room until Home's running apps bring it back
    Close,
    Tilt,
    ResetTilt, //!< the dot above the tilt bar: the window back upright, facing the user
    Curve, //!< the slider above the window: how much it bends (left flat, right bent)
    ResetCurve, //!< the dot at the slider's right end: flat again
    Grab,
    Scale,
    Resize, //!< the bottom-left corner: the app gets a new pixel size (more content), not just bigger
};

constexpr int kMenuButtons = 7;
//! The menu's buttons, top to bottom (the keyboard is in the dock: one keyboard, for the focused window).
constexpr std::array<FrameControl, kMenuButtons> kMenuOrder{FrameControl::Follow, FrameControl::Main,     FrameControl::Pin,  FrameControl::Size,
                                                           FrameControl::Cinema, FrameControl::Minimize, FrameControl::Close};

//! A rectangle in the window's plane: its centre and size in metres, window-local (origin at the window's centre,
//! x right, y up).
struct FrameRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    bool contains(float px, float py) const
    {
        return px >= x - width / 2.0f && px <= x + width / 2.0f && py >= y - height / 2.0f && py <= y + height / 2.0f;
    }
    FrameRect grown(float dx, float dy) const
    {
        return {x, y, width + 2.0f * dx, height + 2.0f * dy};
    }
};

//! Where a window's controls are, for its size and how far away it is.
struct FrameLayout {
    //! The frame's size unit: 1 at 1.6 m, growing with distance so controls keep their size in view.
    float unit = 1.0f;
    float windowWidth = 0.0f;
    float windowHeight = 0.0f;
    float cornerRadius = 0.0f; //!< the window's rounded corners, metres
    FrameRect menu;
    std::array<FrameRect, kMenuButtons> buttons; //!< kMenuOrder
    FrameRect tilt;
    FrameRect resetTilt; //!< the dot above the tilt bar
    FrameRect grab;
    FrameRect scale; //!< the square the L-shaped handle is drawn in
    FrameRect resize; //!< the resize button: a round one just outside the top-right corner
    FrameRect curve; //!< the curvature slider's track, above the window
    FrameRect resetCurve; //!< the dot at its right end
    FrameRect hover; //!< the window, its controls and a margin around them

    /*!
     * @p width, @p height: the window in metres (scaled); @p cornerRadius in metres; @p viewDistance: from the head to
     * the window's centre.
     */
    static FrameLayout compute(float width, float height, float cornerRadius, float viewDistance);

    //! The control at a window-local point; None on the window itself (its content) and elsewhere.
    FrameControl hit(float x, float y) const;
    //! The curvature slider: where its knob is for a curvature (0..1), and the curvature at a point along it.
    float knobX(float curvature) const
    {
        return curve.x - curve.width / 2.0f + curvature * curve.width;
    }
    float curvatureAt(float x) const
    {
        return (x - (curve.x - curve.width / 2.0f)) / curve.width;
    }
    bool near(float x, float y) const
    {
        return hover.contains(x, y);
    }
    //! The ray near the window's edges (a band inside them, or outside the window among its controls): the frame
    //! shows (not over the whole window: it kills the scene).
    bool nearEdge(float x, float y) const
    {
        const float band = kEdgeBand * unit;
        const bool deepInside = std::fabs(x) < windowWidth / 2.0f - band && std::fabs(y) < windowHeight / 2.0f - band;
        return near(x, y) && !deepInside;
    }
    //! Design values, in units (multiply by unit for metres).
    static constexpr float kGap = 0.034f; //!< between the window and its controls (more space looks better)
    static constexpr float kButton = 0.084f; //!< the menu's button pitch and width
    static constexpr float kMenuPadding = 0.012f;
    static constexpr float kBarThickness = 0.011f;
    static constexpr float kBarHitThickness = 0.06f;
    static constexpr float kScaleArm = 0.05f;
    static constexpr float kEdgeBand = 0.05f; //!< how far inside the window's edges the frame still shows
    static constexpr float kDot = 0.028f; //!< the reset dot's diameter
    static constexpr float kKnob = 0.03f; //!< the curvature slider's knob
    static constexpr float kResizeDot = 0.044f; //!< the resize button's diameter
};

} // namespace RiftHome::Spatial
