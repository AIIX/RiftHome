// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cmath>

namespace RiftHome::Spatial
{

/*!
 * Sharp windows at a distance. The runtime samples a layer bilinearly from its full-size image (no mip
 * levels), so a window shown with fewer pixels than it has skips texels and its text shimmers as the head moves. Such a
 * window gets an area-filtered smaller copy instead, at about the density the runtime's samplers keep sharp.
 *
 * Measured offline (scratchpad p2sim: text through the runtime's layer squasher and lens pass against a perfect
 * display, many sub-pixel phases): copies at 1.95x the runtime's 1:1 density at the lens centre (1.5x its scratch
 * images') were never clearly worse and far better from 2 m on; scaling by less than 0.9 makes it softer, so not done.
 *
 * Levels: level n shows 2^(-n/6) of the window's pixels a side (0: all of them). A window changes level only when it
 * wants one at least 0.75 of a step away, so standing still or small movements never flip between two.
 *
 * Shown bigger than the runtime's 1:1 density (a window brought near or made big) the runtime's bilinear enlarging
 * blurs it: below 0.80 of that density the window gets a copy twice its pixels a side, enlarged with a sharp
 * (Lanczos-2) filter (kUpLevel); it goes back above 0.90. Measured the same way: at 0.4-0.6 m (1000 px a metre) 40-45 %
 * less error than the runtime's enlarging alone; at 0.8 m (about 1:1) worse, so not there. Sharpening instead (an
 * unsharp mask) gained only about 10 % near and lost from 1.3 m on.
 */
struct Sharpness {
    static constexpr int kStepsAHalving = 6;
    static constexpr int kMaxLevel = 18; //!< an eighth a side
    static constexpr float kFrom = 0.9f; //!< no copy for a scale above this
    static constexpr float kHysteresis = 0.75f;
    static constexpr float kDensityFactor = 1.95f; //!< target density: this times the runtime's 1:1 one
    static constexpr float kNearest = 0.25f; //!< metres: nearer counts as this near
    static constexpr int kUpLevel = -kStepsAHalving; //!< the 2x copy
    static constexpr float kUpEnter = 0.80f; //!< of the runtime's 1:1 density: below, the 2x copy
    static constexpr float kUpLeave = 0.90f; //!< above, back to the window's own pixels

    //! The level a window wants (continuous, 0 when no copy): @p pixelsPerMetre on it, seen from @p distance metres
    //! where it is nearest the eyes, against @p targetDensity pixels a radian (0: off).
    static float wantedLevel(float pixelsPerMetre, float distance, float targetDensity)
    {
        if (targetDensity <= 0.0f || pixelsPerMetre <= 0.0f) {
            return 0.0f;
        }
        const float seen = pixelsPerMetre * std::max(distance, kNearest); // pixels a radian
        const float native = targetDensity / kDensityFactor;
        if (seen < native * kUpEnter) {
            return float(kUpLevel);
        }
        if (seen < native * kUpLeave) {
            return float(kUpLevel) / 2.0f; // between: whichever it shows
        }
        const float scale = targetDensity / seen;
        if (scale >= kFrom) {
            return 0.0f;
        }
        return std::clamp(-float(kStepsAHalving) * std::log2(scale), 0.0f, float(kMaxLevel));
    }

    //! The level to show next, from the one shown and the one wanted.
    static int nextLevel(int level, float wanted)
    {
        if (wanted <= float(kUpLevel) + kHysteresis) {
            return kUpLevel;
        }
        if (wanted < 0.0f) {
            return level == kUpLevel ? kUpLevel : 0; // the band between enlarging and not: no change
        }
        if (level != kUpLevel && std::abs(wanted - float(level)) <= kHysteresis) {
            return level;
        }
        return std::clamp(int(std::lround(wanted)), 0, kMaxLevel);
    }

    static float scaleOf(int level)
    {
        return level == kUpLevel ? 2.0f : level <= 0 ? 1.0f : std::exp2(-float(level) / float(kStepsAHalving));
    }

    //! The copy's pixels along a side of @p pixels at @p level.
    static int shownSize(int pixels, int level)
    {
        return level == kUpLevel ? 2 * pixels : level <= 0 ? pixels : std::max(1, int(std::ceil(float(pixels) * scaleOf(level))));
    }
};

} // namespace RiftHome::Spatial
