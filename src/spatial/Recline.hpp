// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "spatial/Math.hpp"

#include <algorithm>
#include <cmath>

namespace RiftHome::Spatial
{

/*!
 * Recline: the whole arrangement (windows, Home, the dock, the keyboard) turned to where the user looks, for lying back
 * in bed, on a sofa or on one side. Rift Home keeps laying everything out upright as always; recline is one transform
 * from that layout into the room (OpenXR's LOCAL space), applied to what is shown, with its inverse applied to the head
 * and the controllers' rays. The room's own picture and the controllers' bodies stay where they are. Pure.
 */

//! Seconds the arrangement takes to turn to the view or back upright.
constexpr float kReclineSeconds = 0.5f;
//! Head roll from which the arrangement turns with the head (lying on one side); below it, a slight lean does not skew
//! the windows: their up stays the one nearest the room's.
constexpr float kReclineRollFollowDegrees = 20.0f;

//! How far the head is turned about its view (signed, radians): 0 with its up as near the room's up as the view allows.
inline float headRoll(const Pose &head)
{
    const Vec3 f = head.forward().normalized();
    const Vec3 worldUp{0.0f, 1.0f, 0.0f};
    const Vec3 level = (worldUp - f * f.dot(worldUp)).normalized();
    if (level.length() < 0.5f) {
        return 0.0f; // looking straight up or down: no level to roll from
    }
    const Vec3 up = head.orientation.rotate(worldUp);
    return std::atan2(up.cross(level).dot(f), up.dot(level));
}

/*!
 * The head's view without its roll: at the head, its -Z where the head looks, its up the one nearest the room's (the
 * head's own up when looking straight up or down, and with @p followRoll).
 */
inline Pose levelView(const Pose &head, bool followRoll = false)
{
    const Vec3 forward = head.forward().normalized();
    const Vec3 worldUp{0.0f, 1.0f, 0.0f};
    Vec3 up = (worldUp - forward * forward.dot(worldUp)).normalized();
    if (up.length() < 0.5f || followRoll) {
        up = head.orientation.rotate(worldUp);
    }
    const Vec3 right = forward.cross(up).normalized();
    up = right.cross(forward);
    return {Quat::fromBasis(right, up, forward * -1.0f), head.position};
}

/*!
 * The transform (layout to room) that puts the arrangement's home in front of @p head: @p home's eye moves to the head,
 * its forward to where the head looks, and its up to the head's up when the head rolls by kReclineRollFollowDegrees or
 * more, else to the up nearest the room's. @p home is upright (SpatialLayout::home()).
 */
inline Pose reclineTo(const Pose &home, const Pose &head)
{
    return levelView(head, std::abs(headRoll(head)) >= radians(kReclineRollFollowDegrees)) * home.inverse();
}

/*!
 * A fraction @p t (0..1) of the way from recline @p from to @p to, turning about @p pivot (the arrangement's eye, in the
 * layout): the pivot moves straight from where @p from shows it to where @p to does, everything else turns around it.
 */
inline Pose reclineBlend(const Pose &from, const Pose &to, float t, Vec3 pivot)
{
    t = std::clamp(t, 0.0f, 1.0f);
    const Quat turn = slerp(from.orientation, to.orientation, t);
    const Vec3 a = from.transform(pivot);
    const Vec3 b = to.transform(pivot);
    const Vec3 at = a + (b - a) * t;
    return {turn, at - turn.rotate(pivot)};
}

//! Eased progress for the turn: slow at both ends.
inline float reclineEase(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace RiftHome::Spatial
