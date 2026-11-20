// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "spatial/SpatialLayout.hpp"

#include "spatial/WindowFrame.hpp"

#include <QJsonArray>

#include <algorithm>
#include <cmath>

namespace RiftHome::Spatial
{

namespace
{

constexpr float kMinDistance = 0.4f;
//! The flattest arrangement: an arc of radius about 7 times the distance (windows beside the main one turn a few degrees).
//! A truly flat wall sent a window moved far to the side 8 m away and brought it back much too near.
constexpr float kFlattestCurve = 0.15f;
constexpr float kMaxDistance = 8.0f;
constexpr float kMinScale = 0.3f;
constexpr float kMaxScale = 4.0f;
constexpr float kMaxMainScale = 2.5f; //!< a small window made main is not blown up further
constexpr float kMaxTilt = 1.05f; //!< 60 degrees
constexpr float kTiltPerMetre = 1.6f; //!< window tilt or turn (radians) per metre the dragged point moves (about 9 degrees per 10 cm)
constexpr float kMaxTurn = 1.05f; //!< 60 degrees
//! How far above or below the eyes a grabbed window's middle goes, by angle: the same near or far, wherever it is held.
constexpr float kMaxGrabElevation = 0.785f; //!< 45 degrees
//! A new window wider than this many pixels opens as wide as this (2 m at 1.6 m, about 64 degrees): X11 apps at the
//! X screen's 2560 pixels (Steam) would otherwise open wider than the headset's view.
constexpr float kLargestNewWidthPx = 2000.0f;
constexpr float kTiltDeadzone = 0.015f; //!< metres of the other axis' wobble ignored
//! Follow mode: catch up once the user has turned this far from the window's place, stop when this close.
constexpr float kCatchUpStartDegrees = 25.0f;
constexpr float kCatchUpStopDegrees = 1.5f;
constexpr float kCatchUpStartMetres = 0.4f;
constexpr float kCatchUpSeconds = 0.35f; //!< time constant of the catch-up

float yawOf(const Quat &q)
{
    return 2.0f * std::atan2(q.y, q.w);
}

float horizontalDistance(Vec3 v)
{
    return std::sqrt(v.x * v.x + v.z * v.z);
}

//! Upright, facing @p eye, @p distance away (horizontally) in the direction from @p eye through @p through, at the same
//! angle above or below the eyes: a window grown or shrunk where it is (fullscreen and back moved it).
Pose alongView(Vec3 eye, Vec3 through, float distance)
{
    Vec3 relative = through - eye;
    if (horizontalDistance(relative) < 0.05f) {
        relative = {0.0f, 0.0f, -1.0f};
    }
    const float k = distance / horizontalDistance(relative);
    const Vec3 position = eye + Vec3{relative.x * k, relative.y * k, relative.z * k};
    return {Quat::fromYaw(facingYaw(position, eye)), position};
}

float pastDeadzone(float v)
{
    return std::fabs(v) <= kTiltDeadzone ? 0.0f : v - std::copysign(kTiltDeadzone, v);
}

//! A window's turn and tilt on top of its upright pose (fullscreen screens too).
Quat faceRotation(const SpatialWindow &window)
{
    return Quat::fromYaw(window.turn) * Quat::fromPitch(-window.tilt);
}

QJsonArray toJson(Vec3 v)
{
    return QJsonArray{v.x, v.y, v.z};
}

Vec3 vecFromJson(const QJsonValue &value)
{
    const QJsonArray a = value.toArray();
    return a.size() == 3 ? Vec3{float(a[0].toDouble()), float(a[1].toDouble()), float(a[2].toDouble())} : Vec3{};
}

} // namespace

std::optional<Pose> recentreFromHeads(const std::vector<Pose> &heads)
{
    // The biggest frame-to-frame jump (1 degree of turn counted as 1 cm), if it is far above head motion in 11 ms.
    size_t best = 0;
    float bestJump = 0.0f;
    for (size_t i = 1; i < heads.size(); ++i) {
        const float moved = (heads[i].position - heads[i - 1].position).length();
        const float turned = std::fabs(angleDifference(azimuthOf(heads[i].forward()), azimuthOf(heads[i - 1].forward())));
        const float jump = moved + turned * (0.01f / radians(1.0f));
        if (jump > bestJump) {
            bestJump = jump;
            best = i;
        }
    }
    if (best == 0 || bestJump < 0.05f) {
        return std::nullopt;
    }
    const Pose &before = heads[best - 1];
    const Pose &after = heads[best];
    const Quat turn = Quat::fromYaw(azimuthOf(before.forward()) - azimuthOf(after.forward()));
    return Pose{turn, after.position - turn.rotate(before.position)};
}

SpatialLayout::SpatialLayout(const LayoutOptions &options)
    : m_options(options)
{
}

float SpatialLayout::widthMeters(const SpatialWindow &window) const
{
    return window.widthPx / m_options.pixelsPerMeter * window.scale;
}

float SpatialLayout::heightMeters(const SpatialWindow &window) const
{
    return window.heightPx / m_options.pixelsPerMeter * window.scale;
}

void SpatialLayout::sync(const std::vector<WindowInfo> &windows, const Pose &head)
{
    for (auto it = m_windows.begin(); it != m_windows.end();) {
        const bool present = std::any_of(windows.begin(), windows.end(), [&](const WindowInfo &w) {
            return w.windowId == it->first;
        });
        if (present) {
            ++it;
            continue;
        }
        remember(it->second);
        if (m_grab && m_grab->windowId == it->first) {
            m_grab.reset();
        }
        if (m_drag && m_drag->windowId == it->first) {
            m_drag.reset();
        }
        if (m_cinema == it->first) {
            m_cinema = 0;
        }
        const int gone = it->first;
        it = m_windows.erase(it);
        restoreMovedAsideBy(gone);
        m_dirty = true;
    }
    for (const WindowInfo &info : windows) {
        if (!m_windows.contains(info.windowId)) {
            ensureHome(head);
        }
        if (auto it = m_windows.find(info.windowId); it != m_windows.end()) {
            SpatialWindow &window = it->second;
            const float widthBefore = widthMeters(window); // metres, at the old pixel size
            const int pixelsBefore = window.widthPx;
            const int heightBefore = window.heightPx;
            window.widthPx = info.widthPx;
            window.heightPx = info.heightPx;
            window.appId = info.appId;
            window.pixelScale = info.pixelScale;
            window.x11 = info.x11;
            if (!info.fullscreen && !info.maximized && info.widthPx > 0 && info.heightPx > 0 && !hasAskedSize(window)) {
                window.naturalWidthPx = info.widthPx; // the app's own choice (a video's or game's shape)
                window.naturalHeightPx = info.heightPx;
            }
            if (window.windowId == m_cinema) {
                // Cinema mode: the theater screen keeps its width (the user's, if they scaled it) whatever size the app
                // gives its window (it goes fullscreen). Pixels the user asked for (the resize button) keep its scale: a
                // larger screen; a shape asked (setCinemaShape) is as wide as asked once the app has its pixels.
                window.fullscreen = info.fullscreen;
                const bool changed = pixelsBefore != window.widthPx || heightBefore != window.heightPx;
                const auto near = [](int a, int b) {
                    return std::abs(a - b) <= 4;
                };
                // Only while fullscreen: an app that leaves fullscreen by itself (mpv's own key) is not a size of the screen.
                if (changed && window.fullscreen && m_cinemaRestore && window.widthPx > 0 && near(window.widthPx, m_cinemaRestore->widthPx)
                    && near(window.heightPx, m_cinemaRestore->heightPx)) {
                    window.scale = m_cinemaRestore->width / (window.widthPx / m_options.pixelsPerMeter);
                    m_cinemaRestore.reset();
                } else if (changed && window.fullscreen && hasAskedSize(window)) {
                    // The user's own size (the resize button): its scale stays, more pixels a larger screen.
                } else if (window.widthPx > 0 && pixelsBefore != window.widthPx && widthBefore > 0.0f) {
                    window.scale = widthBefore / (window.widthPx / m_options.pixelsPerMeter);
                }
                continue;
            }
            if (info.fullscreen != window.fullscreen) {
                // Fullscreen: a large screen where the window is (its direction from the eyes), further away; back to
                // its own size and distance where the screen is then (both moved it elsewhere: the
                // screen went in front of the home, the window back to where it had been).
                window.fullscreen = info.fullscreen;
                if (window.fullscreen) {
                    window.windowedPose = window.pose;
                    window.windowedScale = window.scale;
                    window.windowedTilt = window.tilt;
                    window.windowedTurn = window.turn;
                    window.pose = alongView(head.position, window.pose.position, m_options.cinemaDistance);
                    window.tilt = 0.0f;
                    window.turn = 0.0f;
                    if (window.widthPx > 0) {
                        window.scale = m_options.cinemaWidth / (window.widthPx / m_options.pixelsPerMeter);
                    }
                    moveAsideFrom(window.windowId, head); // nothing nearer hides it (a game started from Steam)
                } else {
                    const float distance = std::max(horizontalDistance(window.windowedPose.position - head.position), kMinDistance);
                    window.pose = alongView(head.position, window.pose.position, distance);
                    window.scale = window.windowedScale;
                    window.tilt = window.windowedTilt;
                    window.turn = window.windowedTurn;
                    restoreMovedAsideBy(window.windowId);
                }
                m_dirty = true;
            } else if (window.fullscreen && window.widthPx > 0 && pixelsBefore != window.widthPx && widthBefore > 0.0f) {
                // Fullscreen: the screen keeps its width (the user may have scaled it; it snapped back every
                // frame) when the app changes its pixel size.
                window.scale = widthBefore / (window.widthPx / m_options.pixelsPerMeter);
            }
            if (!window.fullscreen && info.maximized != window.maximized) {
                // Maximized: twice the pixels, as large in the room (its scale halves); back, its own scale again. Made
                // smaller with the resize button instead, the size asked keeps its scale.
                window.maximized = info.maximized;
                if (!hasAskedSize(window)) {
                    const float density = m_options.maximizedDensity;
                    window.scale = window.maximized ? window.scale / density : window.scale * density;
                }
                m_dirty = true;
            }
            continue;
        }
        SpatialWindow window;
        window.windowId = info.windowId;
        window.appId = info.appId;
        window.widthPx = info.widthPx;
        window.heightPx = info.heightPx;
        window.pixelScale = info.pixelScale;
        window.x11 = info.x11;
        window.maximized = info.maximized && !info.fullscreen;
        if (!info.fullscreen && !info.maximized) {
            window.naturalWidthPx = info.widthPx;
            window.naturalHeightPx = info.heightPx;
        }
        const float density = window.maximized ? m_options.maximizedDensity : 1.0f; // remembered at its normal scale
        const auto remembered = info.appId.empty() ? m_remembered.end() : m_remembered.find(info.appId);
        const bool appShown = std::any_of(m_windows.begin(), m_windows.end(), [&](const auto &entry) {
            return entry.second.appId == info.appId;
        });
        if (remembered != m_remembered.end() && !appShown) {
            window.pose = m_home * remembered->second.pose;
            window.pinned = remembered->second.pinned;
            window.locked = remembered->second.locked;
            window.follow = remembered->second.follow;
            if (m_curve < 0.999f) {
                followArc(window, m_home.position); // remembered from another curve: turned to this one where it stands
            }
            // Its size in the room as it was: its pixels are more or fewer if its app drew at another scale then.
            window.scale = std::clamp(remembered->second.scale * remembered->second.pixelScale / std::max(info.pixelScale, 0.1f), kMinScale, kMaxScale)
                / density;
            window.followAzimuth = remembered->second.followAzimuth;
            window.followDistance = remembered->second.followDistance;
            window.followHeight = remembered->second.followHeight;
            window.tilt = remembered->second.tilt;
            window.turn = remembered->second.turn;
            window.curvature = remembered->second.curvature;
        } else {
            window.scale = 1.0f;
            if (float(info.widthPx) / density > kLargestNewWidthPx) {
                window.scale = kLargestNewWidthPx / (float(info.widthPx) / density);
            }
            window.scale /= density;
            window.pose = m_openInFront ? placeInFront(widthMeters(window), head) : placeNew(widthMeters(window), head);
            window.followDistance = m_options.distance;
            window.followHeight = m_options.height;
        }
        m_windows.emplace(info.windowId, window);
    }
}

SpatialLayout::Arc SpatialLayout::arcThrough(Vec3 eye, float azimuth, float distance) const
{
    const float radius = distance / std::max(m_curve, kFlattestCurve);
    return {eye - directionOf(azimuth) * (radius - distance), radius};
}

Vec3 SpatialLayout::arcAxisFor(Vec3 position, Vec3 eye) const
{
    if (m_curve >= 0.999f) {
        return eye;
    }
    // The arc of curve c through the position: its axis k = R (1 - c) behind the eye along the home's forward, its
    // radius R. With x across and z along that forward (from the eye): x^2 + (z + R (1 - c))^2 = R^2, i.e.
    // c (2 - c) R^2 - 2 z (1 - c) R - (x^2 + z^2) = 0; its one positive root.
    const float c = std::max(m_curve, kFlattestCurve);
    const Vec3 forward = directionOf(azimuthOf(m_home.forward()));
    const Vec3 right{-forward.z, 0.0f, forward.x};
    const Vec3 relative = position - eye;
    const float x = relative.x * right.x + relative.z * right.z;
    const float z = relative.x * forward.x + relative.z * forward.z;
    const float a = c * (2.0f - c);
    const float b = 2.0f * z * (1.0f - c);
    const float radius = (b + std::sqrt(b * b + 4.0f * a * (x * x + z * z))) / (2.0f * a);
    return eye - forward * (radius * (1.0f - c));
}

void SpatialLayout::setArrangementCurve(float curve)
{
    curve = std::clamp(curve, 0.0f, 1.0f);
    if (curve == m_curve) {
        return;
    }
    m_curve = curve;
    // The windows turn where they stand to the new curve at once (no window turns differently at its next grab).
    for (auto &[id, window] : m_windows) {
        followArc(window, m_home.position);
    }
    m_dirty = true;
}

void SpatialLayout::followArc(SpatialWindow &window, Vec3 eye) const
{
    if (!m_homeSet || window.pinned || window.follow || window.fullscreen || window.windowId == m_cinema) {
        return;
    }
    window.pose.orientation = Quat::fromYaw(arcYaw(window.pose.position, eye));
}

Pose SpatialLayout::placeNew(float width, const Pose &head, int exclude) const
{
    // Around the home, in the direction the user looks now: along the arrangement's arc through the place straight
    // ahead (fully curved: around the home itself), each window turned to the arc's axis.
    const Vec3 eye = m_home.position;
    const float headAzimuth = azimuthOf(head.forward());
    const float home = azimuthOf(m_home.forward());
    const Arc arc = arcThrough(eye, std::fabs(angleDifference(headAzimuth, home)) < radians(60.0f) ? home : headAzimuth, m_options.distance);
    const float distance = arc.radius;
    // Where the gaze meets the arc, as seen from its axis (fully curved: the gaze itself).
    float gazeOnArc = headAzimuth;
    {
        const Vec3 u = directionOf(headAzimuth);
        const float ox = eye.x - arc.centre.x;
        const float oz = eye.z - arc.centre.z;
        const float b = 2.0f * (ox * u.x + oz * u.z);
        const float c = ox * ox + oz * oz - distance * distance;
        const float t = (-b + std::sqrt(std::max(b * b - 4.0f * c, 0.0f))) / 2.0f;
        gazeOnArc = azimuthOf(eye + u * t - arc.centre);
    }
    const float half = std::atan((width / 2.0f) / distance);
    const float gap = radians(m_options.gapDegrees) * m_options.distance / distance; // the same gap in metres

    struct Interval {
        float azimuth;
        float half;
    };
    std::vector<Interval> taken;
    for (const auto &[id, window] : m_windows) {
        if (id == exclude) {
            continue;
        }
        const Vec3 relative = window.pose.position - arc.centre;
        const float d = horizontalDistance(relative);
        if (d > 0.1f) {
            taken.push_back({azimuthOf(relative), std::atan((widthMeters(window) / 2.0f) / d)});
        }
    }
    // In front of the user, else right next to a window already there, the nearest free place to the gaze first.
    std::vector<float> candidates{gazeOnArc};
    for (const Interval &t : taken) {
        candidates.push_back(t.azimuth + t.half + gap + half);
        candidates.push_back(t.azimuth - t.half - gap - half);
    }
    float best = gazeOnArc;
    float bestOff = 1e9f;
    for (float candidate : candidates) {
        const bool free = std::all_of(taken.begin(), taken.end(), [&](const Interval &t) {
            return std::fabs(angleDifference(candidate, t.azimuth)) >= t.half + half + gap * 0.5f;
        });
        const float off = std::fabs(angleDifference(candidate, gazeOnArc));
        if (free && off < bestOff) {
            best = candidate;
            bestOff = off;
        }
    }
    const Vec3 position = arc.centre + directionOf(best) * distance + Vec3{0.0f, m_options.height, 0.0f};
    return {Quat::fromYaw(arcYaw(position, eye)), position}; // turned as it will be when moved (arcAxisFor())
}

Pose SpatialLayout::placeInFront(float width, const Pose &head) const
{
    const float azimuth = azimuthOf(head.forward());
    float distance = m_options.distance;
    for (const auto &[id, other] : m_windows) {
        if (other.minimized || other.fullscreen) {
            continue;
        }
        const Vec3 relative = other.pose.position - head.position;
        const float d = horizontalDistance(relative);
        if (d < 0.1f || d > distance + 0.3f) {
            continue;
        }
        const float half = std::atan(width / 2.0f / d);
        const float otherHalf = std::atan(widthMeters(other) / 2.0f / d);
        if (std::fabs(angleDifference(azimuthOf(relative), azimuth)) < half + otherHalf) {
            distance = std::min(distance, d - 0.25f); // in front of it
        }
    }
    distance = std::max(distance, 0.7f);
    // As high as the main place, by angle (nearer: less far below the eyes).
    const Vec3 position = head.position + directionOf(azimuth) * distance + Vec3{0.0f, m_options.height * distance / m_options.distance, 0.0f};
    return {Quat::fromYaw(arcYaw(position, head.position)), position};
}

Pose SpatialLayout::surfacePose(const SpatialWindow &window) const
{
    return {window.pose.orientation * faceRotation(window), window.pose.position};
}

float SpatialLayout::curveRadius(const SpatialWindow &window) const
{
    const float width = widthMeters(window);
    if (window.curvature < kCurveSnap || width <= 0.0f) {
        return 0.0f;
    }
    return width / (window.curvature * kMaxCurveAngle);
}

Pose SpatialLayout::pointOn(const SpatialWindow &window, float x, float y, float lift) const
{
    const Pose surface = surfacePose(window);
    const float r = curveRadius(window);
    if (r <= 0.0f) {
        return {surface.orientation, surface.transform({x, y, lift})};
    }
    // Along the arc: x metres of arc from the centre line, turned to face the axis there.
    const float phi = x / r;
    const float d = r - lift;
    return {surface.orientation * Quat::fromYaw(-phi), surface.transform({d * std::sin(phi), y, r - d * std::cos(phi)})};
}

std::optional<Hit> SpatialLayout::intersect(const SpatialWindow &window, Vec3 origin, Vec3 direction, bool withinWindow) const
{
    const float width = widthMeters(window);
    const float height = heightMeters(window);
    if (width <= 0.0f || height <= 0.0f) {
        return std::nullopt;
    }
    const Pose toLocal = surfacePose(window).inverse();
    const Vec3 o = toLocal.transform(origin);
    const Vec3 d = toLocal.orientation.rotate(direction);
    const float r = curveRadius(window);
    float t = 0.0f;
    float lx = 0.0f;
    float ly = 0.0f;
    if (r <= 0.0f) {
        if (d.z > -1e-4f) {
            return std::nullopt; // from behind, or along the window
        }
        t = -o.z / d.z;
        if (t <= 0.0f) {
            return std::nullopt;
        }
        lx = o.x + d.x * t;
        ly = o.y + d.y * t;
    } else {
        // The cylinder x^2 + (z - r)^2 = r^2 (its axis along y through (0, 0, r)); its front faces the axis.
        const float oz = o.z - r;
        const float a = d.x * d.x + d.z * d.z;
        if (a < 1e-8f) {
            return std::nullopt; // along the axis
        }
        const float b = 2.0f * (o.x * d.x + oz * d.z);
        const float c = o.x * o.x + oz * oz - r * r;
        const float discriminant = b * b - 4.0f * a * c;
        if (discriminant < 0.0f) {
            return std::nullopt;
        }
        const float root = std::sqrt(discriminant);
        const float limit = withinWindow ? width / 2.0f / r : kPi / 2.0f;
        bool found = false;
        for (const float candidate : {(-b - root) / (2.0f * a), (-b + root) / (2.0f * a)}) {
            if (candidate <= 0.0f) {
                continue;
            }
            const Vec3 p = o + d * candidate;
            if (d.x * -p.x + d.z * (r - p.z) >= 0.0f) {
                continue; // its back: the ray goes toward the axis there
            }
            const float phi = std::atan2(p.x, r - p.z);
            if (std::fabs(phi) > limit) {
                continue;
            }
            t = candidate;
            lx = phi * r;
            ly = p.y;
            found = true;
            break;
        }
        if (!found) {
            return std::nullopt;
        }
    }
    if (withinWindow && (std::fabs(lx) > width / 2.0f || std::fabs(ly) > height / 2.0f)) {
        return std::nullopt;
    }
    Hit hit;
    hit.windowId = window.windowId;
    hit.x = (lx / width + 0.5f) * window.widthPx;
    hit.y = (0.5f - ly / height) * window.heightPx;
    hit.distance = t;
    hit.point = origin + direction * t;
    hit.localX = lx;
    hit.localY = ly;
    return hit;
}

std::optional<Hit> SpatialLayout::hitTest(Vec3 origin, Vec3 direction, int onlyWindow) const
{
    std::optional<Hit> best;
    for (const auto &[id, window] : m_windows) {
        if ((onlyWindow && id != onlyWindow) || window.minimized) {
            continue;
        }
        const auto hit = intersect(window, origin, direction, true);
        if (hit && (!best || hit->distance < best->distance)) {
            best = hit;
        }
    }
    return best;
}

std::optional<Hit> SpatialLayout::projectOnto(int windowId, Vec3 origin, Vec3 direction) const
{
    const auto it = m_windows.find(windowId);
    if (it == m_windows.end()) {
        return std::nullopt;
    }
    return intersect(it->second, origin, direction, false);
}

bool SpatialLayout::beginGrab(int windowId, const Pose &controller, float hitDistance)
{
    const auto it = m_windows.find(windowId);
    if (it == m_windows.end()) {
        return false;
    }
    const Vec3 point = controller.position + controller.forward() * hitDistance;
    const Vec3 local = surfacePose(it->second).inverse().transform(point);
    m_grab = Grab{windowId, hitDistance, local * (1.0f / it->second.scale), false, {}, 0.0f};
    return true;
}

void SpatialLayout::updateGrab(const Pose &controller, const Pose &head, float pushPull, float grow, float dt)
{
    if (!m_grab) {
        return;
    }
    const auto it = m_windows.find(m_grab->windowId);
    if (it == m_windows.end()) {
        m_grab.reset();
        return;
    }
    SpatialWindow &window = it->second;
    if (window.locked) {
        // Its place is locked: only larger or smaller (about its centre); the hand, the push and the pull move nothing.
        const float scale = std::clamp(window.scale * std::exp(grow * 0.8f * dt), kMinScale, kMaxScale);
        if (scale != window.scale) {
            window.scale = scale;
            keepInRoom(window, Inside::Smaller); // no larger than the room has room for
            m_dirty = true;
        }
        return;
    }
    const float pushed = std::exp(pushPull * 1.2f * dt);
    m_grab->distance = std::clamp(m_grab->distance * pushed, kMinDistance, kMaxDistance);
    window.scale = std::clamp(window.scale * std::exp(grow * 0.8f * dt), kMinScale, kMaxScale);
    const Vec3 offset = m_grab->offsetLocal * window.scale;
    const Quat tilt = faceRotation(window);

    Vec3 target;
    Vec3 axis = head.position; // what the window turns to
    if (m_curve >= 0.999f) {
        // Fully curved: the grabbed point stays on the ray, the window upright (with its tilt), facing the user.
        target = controller.position + controller.forward() * m_grab->distance;
    } else {
        // Along the arrangement's arc (its axis behind the user, straight back from the home's forward, as placeNew()'s),
        // through the window: the grabbed point where the ray meets it, the window turned to its axis, so straight in
        // front it faces the user squarely again.
        if (!m_grab->onArc) {
            const Vec3 grabbed = surfacePose(window).transform(offset);
            m_grab->onArc = true;
            m_grab->centre = arcAxisFor(grabbed, head.position);
            m_grab->radius = horizontalDistance(grabbed - m_grab->centre); // through the grabbed point: no jump
        }
        // Pushed or pulled: the arc nearer or farther by as much as the window would move.
        const float nearness = horizontalDistance(window.pose.position - head.position);
        const float wanted = std::clamp(nearness * pushed, kMinDistance, kMaxDistance);
        m_grab->radius = std::max(m_grab->radius + (wanted - nearness), 0.1f);
        const Vec3 o = controller.position;
        const Vec3 v = controller.forward();
        const float ox = o.x - m_grab->centre.x;
        const float oz = o.z - m_grab->centre.z;
        const float a = v.x * v.x + v.z * v.z;
        const float b = 2.0f * (ox * v.x + oz * v.z);
        const float c = ox * ox + oz * oz - m_grab->radius * m_grab->radius;
        const float disc = b * b - 4.0f * a * c;
        const float t = a > 1e-6f && disc >= 0.0f ? (-b + std::sqrt(disc)) / (2.0f * a) : -1.0f;
        if (t <= 0.0f) {
            return; // straight up or down, or the arc behind: the window stays where it is
        }
        target = o + v * t;
        const float heldAbove = tilt.rotate(offset).y; //!< the held point above the window's middle
        const float reach = horizontalDistance(target - head.position) * std::tan(kMaxGrabElevation);
        target.y = std::clamp(target.y - heldAbove, head.position.y - reach, head.position.y + reach) + heldAbove;
    }
    // Turned by where the window's centre ends up (arcYaw(): the same at every grab), fully curved: to the user.
    const auto yawAt = [&](Vec3 centre) {
        return m_curve >= 0.999f ? facingYaw(centre, axis) : arcYaw(centre, head.position);
    };
    float yaw = yawAt(target);
    for (int i = 0; i < 3; ++i) {
        yaw = yawAt(target - (Quat::fromYaw(yaw) * tilt).rotate(offset));
    }
    const Quat orientation = Quat::fromYaw(yaw);
    window.pose = {orientation, target - (orientation * tilt).rotate(offset)};
    if (roomLimit() > 0.0f && farthestFromRoomCentre(window) > roomLimit()) {
        // Pushed (or held) past the room's surface: it stops inside it, nearer the room's centre at its size, and the
        // grab takes it from there, so pushing on does not build up beyond the room.
        keepInRoom(window, Inside::Nearer);
        const Vec3 grabbed = surfacePose(window).transform(m_grab->offsetLocal * window.scale);
        m_grab->distance = std::max((grabbed - controller.position).length(), kMinDistance);
        if (m_grab->onArc) {
            m_grab->radius = std::max(horizontalDistance(grabbed - m_grab->centre), 0.1f);
        }
    }
    if (window.follow && m_groupSet) {
        const Vec3 relative = window.pose.position - m_groupCentre;
        window.followAzimuth = angleDifference(azimuthOf(relative), m_groupYaw);
        window.followDistance = horizontalDistance(relative);
        window.followHeight = relative.y;
    }
    m_dirty = true;
}

void SpatialLayout::endGrab()
{
    if (m_grab) {
        if (const auto it = m_windows.find(m_grab->windowId); it != m_windows.end()) {
            remember(it->second);
        }
        m_dirty = true;
    }
    m_grab.reset();
}

bool SpatialLayout::beginScale(int windowId, Vec3 origin, Vec3 direction)
{
    const auto hit = projectOnto(windowId, origin, direction);
    if (!hit) {
        return false;
    }
    const SpatialWindow &window = m_windows.at(windowId);
    const float d = std::hypot(hit->localX, hit->localY);
    if (d < 0.05f) {
        return false;
    }
    m_drag = Drag{Drag::Kind::Scale, windowId, d / window.scale, window.scale};
    return true;
}

void SpatialLayout::updateScale(Vec3 origin, Vec3 direction)
{
    if (!m_drag || m_drag->kind != Drag::Kind::Scale) {
        return;
    }
    const auto it = m_windows.find(m_drag->windowId);
    const auto hit = projectOnto(m_drag->windowId, origin, direction);
    if (it == m_windows.end() || !hit) {
        return;
    }
    // Scaled about the centre: the dragged point (the corner handle) stays under the ray.
    const float d = std::hypot(hit->localX, hit->localY);
    it->second.scale = std::clamp(d / m_drag->start, kMinScale, kMaxScale);
    keepInRoom(it->second, Inside::Smaller); // no larger than the room has room for
    m_dirty = true;
}

bool SpatialLayout::beginTilt(int windowId, Vec3 origin, Vec3 direction)
{
    const auto it = m_windows.find(windowId);
    const auto hit = projectOnto(windowId, origin, direction);
    if (it == m_windows.end() || !hit) {
        return false;
    }
    Drag drag{Drag::Kind::Tilt, windowId, 0.0f, it->second.tilt};
    drag.initialTurn = it->second.turn;
    drag.plane = surfacePose(it->second);
    const Vec3 local = drag.plane.inverse().transform(hit->point);
    drag.startX = local.x;
    drag.startY = local.y;
    m_drag = drag;
    return true;
}

void SpatialLayout::updateTilt(Vec3 origin, Vec3 direction)
{
    if (!m_drag || m_drag->kind != Drag::Kind::Tilt) {
        return;
    }
    const auto it = m_windows.find(m_drag->windowId);
    if (it == m_windows.end()) {
        return;
    }
    // Where the ray meets the plane the drag began on: dragged up turns the face up, right turns it right.
    const Vec3 normal = m_drag->plane.orientation.rotate({0.0f, 0.0f, 1.0f});
    const float denom = direction.dot(normal);
    if (denom > -1e-4f) {
        return;
    }
    const float t = (m_drag->plane.position - origin).dot(normal) / denom;
    if (t <= 0.0f) {
        return;
    }
    const Vec3 local = m_drag->plane.inverse().transform(origin + direction * t);
    const float up = pastDeadzone(local.y - m_drag->startY);
    const float right = pastDeadzone(local.x - m_drag->startX);
    it->second.tilt = std::clamp(m_drag->initial + up * kTiltPerMetre, -kMaxTilt, kMaxTilt);
    it->second.turn = std::clamp(m_drag->initialTurn + right * kTiltPerMetre, -kMaxTurn, kMaxTurn);
    m_dirty = true;
}

void SpatialLayout::resetTilt(int windowId)
{
    const auto it = m_windows.find(windowId);
    if (it == m_windows.end()) {
        return;
    }
    it->second.tilt = 0.0f;
    it->second.turn = 0.0f;
    remember(it->second);
    m_dirty = true;
}

void SpatialLayout::adjustTilt(int windowId, float tilt, float turn)
{
    const auto it = m_windows.find(windowId);
    if (it == m_windows.end() || (tilt == 0.0f && turn == 0.0f)) {
        return;
    }
    it->second.tilt = std::clamp(it->second.tilt + tilt, -kMaxTilt, kMaxTilt);
    it->second.turn = std::clamp(it->second.turn + turn, -kMaxTurn, kMaxTurn);
    remember(it->second);
    m_dirty = true;
}

void SpatialLayout::endDrag()
{
    if (m_drag) {
        if (const auto it = m_windows.find(m_drag->windowId); it != m_windows.end()) {
            remember(it->second);
        }
        m_dirty = true;
    }
    m_drag.reset();
}

void SpatialLayout::setMinimized(int windowId, bool minimized)
{
    const auto it = m_windows.find(windowId);
    if (it == m_windows.end() || it->second.minimized == minimized) {
        return;
    }
    it->second.minimized = minimized;
    if (m_grab && m_grab->windowId == windowId) {
        m_grab.reset();
    }
    if (m_drag && m_drag->windowId == windowId) {
        m_drag.reset();
    }
    if (minimized && m_cinema == windowId) {
        leaveCinema();
    }
    if (minimized) {
        restoreMovedAsideBy(windowId);
    }
    m_dirty = true;
}

void SpatialLayout::setCurvature(int windowId, float curvature)
{
    const auto it = m_windows.find(windowId);
    if (it == m_windows.end()) {
        return;
    }
    const float c = std::clamp(curvature, 0.0f, 1.0f);
    const float snapped = c < kCurveSnap ? 0.0f : c;
    if (snapped == it->second.curvature) {
        return;
    }
    it->second.curvature = snapped;
    remember(it->second);
    m_dirty = true;
}

void SpatialLayout::adjustCurvature(int windowId, float delta)
{
    const auto it = m_windows.find(windowId);
    if (it == m_windows.end() || delta == 0.0f) {
        return;
    }
    float c = it->second.curvature + delta;
    if (delta > 0.0f && c < kCurveSnap) {
        c = kCurveSnap; // out of flat in one step (below the snap it would stay flat)
    }
    setCurvature(windowId, c);
}

void SpatialLayout::makeMain(int windowId, const Pose &head)
{
    const auto it = m_windows.find(windowId);
    if (it == m_windows.end()) {
        return;
    }
    SpatialWindow &window = it->second;
    const float azimuth = azimuthOf(head.forward());
    float distance = m_options.distance;
    if (window.fullscreen) {
        // A fullscreen screen (a film): in front of the user at its distance and size, upright (the
        // button did nothing for it).
        distance = std::max(horizontalDistance(window.pose.position - head.position), kMinDistance);
        const Vec3 position = head.position + directionOf(azimuth) * distance + Vec3{0.0f, m_options.height * distance / m_options.distance, 0.0f};
        window.pose = {Quat::fromYaw(facingYaw(position, head.position)), position};
    } else {
        const Vec3 position = head.position + directionOf(azimuth) * distance + Vec3{0.0f, m_options.height, 0.0f};
        window.pose = {Quat::fromYaw(arcYaw(position, head.position)), position};
        if (window.widthPx > 0) {
            window.scale = std::clamp(m_options.mainWidth / (window.widthPx / m_options.pixelsPerMeter), kMinScale, kMaxMainScale);
        }
    }
    window.tilt = 0.0f;
    window.turn = 0.0f;
    window.follow = false;
    // Windows in the way move aside (mpv nearer than the fullscreen Firefox stayed in front of it).
    moveAsideFrom(windowId, head);
    if (!window.fullscreen) {
        remember(window); // a fullscreen screen's place is not the app's window place
    }
    m_dirty = true;
}

void SpatialLayout::moveAsideFrom(int windowId, const Pose &head)
{
    const auto it = m_windows.find(windowId);
    if (it == m_windows.end()) {
        return;
    }
    const SpatialWindow &window = it->second;
    const Vec3 toWindow = window.pose.position - head.position;
    const float distance = std::max(horizontalDistance(toWindow), kMinDistance);
    const float azimuth = azimuthOf(toWindow);
    const float half = std::atan(widthMeters(window) / 2.0f / distance);
    for (auto &[id, other] : m_windows) {
        if (id == windowId || other.pinned || other.follow || other.fullscreen || other.minimized) {
            continue;
        }
        const Vec3 relative = other.pose.position - head.position;
        const float d = horizontalDistance(relative);
        if (d < 0.1f || d > distance + 0.6f) {
            continue;
        }
        const float otherHalf = std::atan(widthMeters(other) / 2.0f / d);
        if (std::fabs(angleDifference(azimuthOf(relative), azimuth)) < half + otherHalf) {
            const Pose placed = placeNew(widthMeters(other), head, id);
            const Pose before = other.pose;
            other.pose = {placed.orientation, placed.position + Vec3{0.0f, other.pose.position.y - placed.position.y, 0.0f}};
            m_movedAside.push_back({id, windowId, before, other.pose});
            remember(other);
        }
    }
}

void SpatialLayout::restoreMovedAsideBy(int windowId)
{
    const auto samePose = [](const Pose &a, const Pose &b) {
        const Vec3 d = a.position - b.position;
        const float dot = a.orientation.x * b.orientation.x + a.orientation.y * b.orientation.y + a.orientation.z * b.orientation.z
            + a.orientation.w * b.orientation.w;
        return d.length() < 0.001f && std::fabs(std::fabs(dot) - 1.0f) < 1e-5f;
    };
    // Newest first: a window pushed twice goes back step by step.
    for (auto record = m_movedAside.rbegin(); record != m_movedAside.rend(); ++record) {
        if (record->by != windowId) {
            continue;
        }
        const auto it = m_windows.find(record->windowId);
        if (it == m_windows.end() || !samePose(it->second.pose, record->after)) {
            continue; // closed, or the user has put it somewhere since
        }
        SpatialWindow &window = it->second;
        // Free there? Another window may have taken the place meanwhile (as seen from the home).
        const Vec3 eye = m_home.position;
        const Vec3 relative = record->before.position - eye;
        const float d = std::max(horizontalDistance(relative), kMinDistance);
        const float half = std::atan(widthMeters(window) / 2.0f / d);
        const bool free = std::none_of(m_windows.begin(), m_windows.end(), [&](const auto &entry) {
            const SpatialWindow &other = entry.second;
            if (entry.first == window.windowId || entry.first == windowId || other.minimized) {
                return false;
            }
            const Vec3 r = other.pose.position - eye;
            const float od = horizontalDistance(r);
            return od > 0.1f && od < d + 0.6f
                && std::fabs(angleDifference(azimuthOf(r), azimuthOf(relative))) < half + std::atan(widthMeters(other) / 2.0f / od);
        });
        if (free) {
            window.pose = record->before;
            followArc(window, m_home.position);
            remember(window);
            m_dirty = true;
        }
    }
    m_movedAside.erase(std::remove_if(m_movedAside.begin(), m_movedAside.end(),
                                      [&](const MovedAside &record) {
                                          return record.by == windowId || record.windowId == windowId || !m_windows.contains(record.windowId);
                                      }),
                       m_movedAside.end());
}

void SpatialLayout::enterCinema(int windowId, const Pose &head)
{
    const auto it = m_windows.find(windowId);
    if (it == m_windows.end()) {
        return;
    }
    leaveCinema();
    SpatialWindow &window = it->second;
    m_beforeCinema = window;
    m_cinema = windowId;
    m_cinemaRestore.reset();
    m_askedSizes.erase(windowId); // sizes asked of the window before are not the screen's (it was not fullscreen then)
    // Where the user left this app's cinema screen last time, else straight ahead.
    const auto place = window.appId.empty() ? m_cinemaPlaces.end() : m_cinemaPlaces.find(window.appId);
    float width = m_options.theaterWidth;
    if (place != m_cinemaPlaces.end()) {
        window.pose = m_home * place->second.pose;
        width = place->second.width;
    } else {
        const Vec3 position = head.position + directionOf(azimuthOf(head.forward())) * m_options.theaterDistance + Vec3{0.0f, 0.1f, 0.0f};
        window.pose = {Quat::fromYaw(facingYaw(position, head.position)), position};
    }
    window.tilt = 0.0f;
    window.turn = 0.0f;
    window.follow = false;
    if (window.widthPx > 0) {
        window.scale = width / (window.widthPx / m_options.pixelsPerMeter);
    }
    m_dirty = true;
}

void SpatialLayout::rememberCinemaPlace()
{
    const auto it = m_windows.find(m_cinema);
    if (it != m_windows.end() && !it->second.appId.empty()) {
        m_cinemaPlaces[it->second.appId] = {m_home.inverse() * it->second.pose, widthMeters(it->second)};
    }
}

void SpatialLayout::leaveCinema()
{
    rememberCinemaPlace();
    const auto it = m_windows.find(m_cinema);
    m_askedSizes.erase(m_cinema);
    m_cinemaRestore.reset();
    m_cinema = 0;
    if (it == m_windows.end()) {
        return;
    }
    SpatialWindow &window = it->second;
    window.pose = m_beforeCinema.pose;
    window.scale = m_beforeCinema.scale;
    window.tilt = m_beforeCinema.tilt;
    window.turn = m_beforeCinema.turn;
    window.follow = m_beforeCinema.follow;
    window.pinned = m_beforeCinema.pinned;
    // Back to what it was: a fullscreen window stays the screen the user made it, with where it was before that (session
    // 11: taken for "just went fullscreen", it came back at the default size, its windowed place lost). A window that
    // was not fullscreen goes back when the app leaves fullscreen (the next syncs say).
    window.fullscreen = m_beforeCinema.fullscreen;
    if (window.fullscreen && window.widthPx > 0 && m_beforeCinema.widthPx > 0) {
        window.scale = widthMeters(m_beforeCinema) / (window.widthPx / m_options.pixelsPerMeter); // its width, at today's pixels
    }
    window.windowedPose = m_beforeCinema.windowedPose;
    window.windowedScale = m_beforeCinema.windowedScale;
    window.windowedTilt = m_beforeCinema.windowedTilt;
    window.windowedTurn = m_beforeCinema.windowedTurn;
    m_dirty = true;
}

void SpatialLayout::setCinemaShape(int windowId, int widthPx, int heightPx, float widthMeters)
{
    if (windowId != m_cinema || widthPx <= 0 || heightPx <= 0 || widthMeters <= 0.0f) {
        return;
    }
    m_cinemaRestore = CinemaRestore{widthPx, heightPx, widthMeters}; // as wide as that once the app has the pixels
}

void SpatialLayout::askedSize(int windowId, int widthPx, int heightPx)
{
    std::vector<std::pair<int, int>> &asked = m_askedSizes[windowId];
    asked.emplace_back(widthPx, heightPx);
    if (asked.size() > 64) {
        asked.erase(asked.begin()); // a long drag: the sizes still to come are the latest ones
    }
}

bool SpatialLayout::hasAskedSize(const SpatialWindow &window) const
{
    const auto it = m_askedSizes.find(window.windowId);
    if (it == m_askedSizes.end()) {
        return false;
    }
    // The app's size in its own units, times the apps' scale: a few pixels off what was asked.
    return std::any_of(it->second.begin(), it->second.end(), [&](const std::pair<int, int> &size) {
        return std::abs(window.widthPx - size.first) <= 4 && std::abs(window.heightPx - size.second) <= 4;
    });
}

bool SpatialLayout::toggleLock(int windowId)
{
    const auto it = m_windows.find(windowId);
    if (it == m_windows.end()) {
        return false;
    }
    it->second.locked = !it->second.locked;
    remember(it->second);
    m_dirty = true;
    return it->second.locked;
}

bool SpatialLayout::togglePin(int windowId)
{
    const auto it = m_windows.find(windowId);
    if (it == m_windows.end()) {
        return false;
    }
    it->second.pinned = !it->second.pinned;
    if (it->second.pinned) {
        it->second.follow = false;
    }
    remember(it->second);
    m_dirty = true;
    return it->second.pinned;
}

void SpatialLayout::joinGroup(SpatialWindow &window, const Pose &head)
{
    const bool anyone = std::any_of(m_windows.begin(), m_windows.end(), [&](const auto &e) {
        return e.second.follow && &e.second != &window;
    });
    if (!anyone || !m_groupSet) {
        m_groupYaw = azimuthOf(head.forward());
        m_groupCentre = head.position;
        m_groupSet = true;
        m_groupCatching = false;
    }
    const Vec3 relative = window.pose.position - m_groupCentre;
    window.followAzimuth = angleDifference(azimuthOf(relative), m_groupYaw);
    window.followDistance = std::max(horizontalDistance(relative), kMinDistance);
    window.followHeight = relative.y;
    window.follow = true;
    window.pinned = false;
}

bool SpatialLayout::toggleFollow(int windowId, const Pose &head)
{
    const auto it = m_windows.find(windowId);
    if (it == m_windows.end()) {
        return false;
    }
    SpatialWindow &window = it->second;
    if (window.follow) {
        window.follow = false;
    } else {
        joinGroup(window, head);
    }
    remember(window);
    m_dirty = true;
    return window.follow;
}

void SpatialLayout::setFollowAll(bool on, const Pose &head)
{
    for (auto &[id, window] : m_windows) {
        if (on && !window.follow) {
            joinGroup(window, head);
        } else if (!on) {
            window.follow = false;
        }
        remember(window);
    }
    m_dirty = true;
}

bool SpatialLayout::followAll() const
{
    bool any = false;
    for (const auto &[id, window] : m_windows) {
        if (!window.follow) {
            return false;
        }
        any = true;
    }
    return any;
}

void SpatialLayout::updateFollow(const Pose &head, float dt)
{
    const bool anyone = std::any_of(m_windows.begin(), m_windows.end(), [](const auto &e) {
        return e.second.follow;
    });
    if (!anyone) {
        return;
    }
    if (!m_groupSet) {
        // Remembered followers (a restart): the group forms around where the user is now.
        m_groupYaw = azimuthOf(head.forward());
        m_groupCentre = head.position;
        m_groupSet = true;
    }
    // One decision for the whole group: catch up once the user has turned or walked away far enough.
    const float headAzimuth = azimuthOf(head.forward());
    const float angleError = std::fabs(angleDifference(headAzimuth, m_groupYaw));
    const float distanceError = horizontalDistance(head.position - m_groupCentre);
    if (!m_groupCatching && (angleError > radians(kCatchUpStartDegrees) || distanceError > kCatchUpStartMetres)) {
        m_groupCatching = true;
    }
    if (m_groupCatching && angleError < radians(kCatchUpStopDegrees) && distanceError < 0.05f) {
        m_groupCatching = false;
    }
    if (m_groupCatching) {
        const float k = 1.0f - std::exp(-dt / kCatchUpSeconds);
        m_groupYaw += angleDifference(headAzimuth, m_groupYaw) * k;
        m_groupCentre = m_groupCentre + (head.position - m_groupCentre) * k;
    }
    for (auto &[id, window] : m_windows) {
        if (!window.follow || (m_grab && m_grab->windowId == id)) {
            continue; // the cinema screen follows too
        }
        const Vec3 position = m_groupCentre + directionOf(m_groupYaw + window.followAzimuth) * window.followDistance
            + Vec3{0.0f, window.followHeight, 0.0f};
        window.pose = {Quat::fromYaw(facingYaw(position, m_groupCentre)), position};
    }
}

void SpatialLayout::setRoomBound(Vec3 centre, float radius)
{
    m_roomCentre = centre;
    m_roomRadius = std::max(radius, 0.0f);
}

float SpatialLayout::farthestFromRoomCentre(const SpatialWindow &window) const
{
    // Its corners with its frame's controls (the menu pill, the bars, a margin) at the size they have at its distance;
    // a curved window bends towards the user, so its flat corners are as far as it reaches.
    const FrameLayout frame = FrameLayout::compute(widthMeters(window), heightMeters(window), 0.02f, (window.pose.position - m_roomCentre).length());
    const FrameRect &r = frame.hover;
    const Pose surface = surfacePose(window);
    float farthest = 0.0f;
    for (const float sx : {-0.5f, 0.5f}) {
        for (const float sy : {-0.5f, 0.5f}) {
            farthest = std::max(farthest, (surface.transform({r.x + sx * r.width, r.y + sy * r.height, 0.0f}) - m_roomCentre).length());
        }
    }
    return farthest;
}

void SpatialLayout::keepInRoom(SpatialWindow &window, Inside how)
{
    const float limit = roomLimit();
    if (limit <= 0.0f || farthestFromRoomCentre(window) <= limit) {
        return;
    }
    const Vec3 from = window.pose.position - m_roomCentre;
    const float startScale = window.scale;
    const auto apply = [&](float k) {
        if (how != Inside::Smaller) {
            window.pose.position = m_roomCentre + from * k;
        }
        if (how != Inside::Nearer) {
            window.scale = startScale * k;
        }
    };
    // Nearer: no nearer the centre than a metre (else smaller as well); the largest factor that fits, by halving.
    float low = how == Inside::Nearer ? std::min(1.0f, 1.0f / std::max(from.length(), 1e-3f)) : 0.0f;
    float high = 1.0f;
    apply(low);
    if (farthestFromRoomCentre(window) <= limit) {
        for (int i = 0; i < 24; ++i) {
            const float middle = (low + high) / 2.0f;
            apply(middle);
            (farthestFromRoomCentre(window) <= limit ? low : high) = middle;
        }
        apply(low);
    } else if (how == Inside::Nearer) {
        keepInRoom(window, Inside::Smaller); // too large to fit at any distance: smaller too
    } else if (how == Inside::Smaller) {
        window.scale = startScale; // its middle itself beyond the room: no size fits there, so nearer and smaller
        keepInRoom(window, Inside::Both);
    }
    m_dirty = true;
}

void SpatialLayout::keepWindowsInRoom()
{
    if (roomLimit() <= 0.0f) {
        return;
    }
    for (auto &[id, window] : m_windows) {
        if (window.minimized || (m_grab && m_grab->windowId == id) || (m_drag && m_drag->windowId == id)) {
            continue; // hidden; or held: its grab or drag keeps it inside
        }
        if (window.follow && m_groupSet) {
            // Following: placed again every frame from the group's distances, so those come in too (else it would come
            // in smaller every frame).
            keepInRoom(window, Inside::Nearer);
            const Vec3 relative = window.pose.position - m_groupCentre;
            window.followAzimuth = angleDifference(azimuthOf(relative), m_groupYaw);
            window.followDistance = horizontalDistance(relative);
            window.followHeight = relative.y;
            continue;
        }
        keepInRoom(window, Inside::Both);
    }
}

void SpatialLayout::ensureHome(const Pose &head)
{
    if (m_homeSet) {
        return;
    }
    // Upright, facing where the user looks: forward (-Z) along the head's azimuth.
    m_home = {Quat::fromYaw(-azimuthOf(head.forward())), head.position};
    m_homeSet = true;
}

void SpatialLayout::recentered(const std::optional<Pose> &oldToNew)
{
    const Pose toNew = m_home.inverse();
    for (auto &[id, window] : m_windows) {
        window.pose = (window.pinned && oldToNew ? *oldToNew : toNew) * window.pose;
    }
    m_home = Pose{};
    m_homeSet = true;
    m_grab.reset();
    m_drag.reset();
    m_groupSet = false; // the group forms again around the user, wherever the recentre put them
    m_dirty = true;
}

void SpatialLayout::spaceChanged(const Pose &newOriginInOldSpace)
{
    const Pose toNew = newOriginInOldSpace.inverse();
    for (auto &[id, window] : m_windows) {
        window.pose = toNew * window.pose;
    }
    m_home = toNew * m_home;
    m_dirty = true;
}

void SpatialLayout::remember(const SpatialWindow &window)
{
    if (!window.appId.empty()) {
        SpatialWindow relative = window;
        relative.pose = m_home.inverse() * window.pose;
        if (relative.maximized) {
            relative.scale *= m_options.maximizedDensity; // its normal scale
            relative.maximized = false;
        }
        m_remembered[window.appId] = relative;
    }
}

QJsonObject SpatialLayout::save() const
{
    std::map<std::string, SpatialWindow> all = m_remembered;
    for (const auto &[id, window] : m_windows) {
        if (!window.appId.empty()) {
            SpatialWindow relative = window;
            relative.pose = m_home.inverse() * window.pose;
            if (relative.maximized) {
                relative.scale *= m_options.maximizedDensity; // its normal scale
            }
            all[window.appId] = relative;
        }
    }
    QJsonObject windows;
    for (const auto &[appId, window] : all) {
        windows.insert(QString::fromStdString(appId),
                       QJsonObject{{QStringLiteral("position"), toJson(window.pose.position)},
                                   {QStringLiteral("yaw"), yawOf(window.pose.orientation)},
                                   {QStringLiteral("scale"), window.scale},
                                   {QStringLiteral("pixelScale"), window.pixelScale},
                                   {QStringLiteral("tilt"), window.tilt},
                                   {QStringLiteral("turn"), window.turn},
                                   {QStringLiteral("curvature"), window.curvature},
                                   {QStringLiteral("pinned"), window.pinned},
                                   {QStringLiteral("locked"), window.locked},
                                   {QStringLiteral("follow"), window.follow},
                                   {QStringLiteral("followAzimuth"), window.followAzimuth},
                                   {QStringLiteral("followDistance"), window.followDistance},
                                   {QStringLiteral("followHeight"), window.followHeight}});
    }
    // Version 2: poses relative to the home (version 1 had LOCAL poses, wrong after a recentre: not read).
    return QJsonObject{{QStringLiteral("version"), 2}, {QStringLiteral("windows"), windows}};
}

void SpatialLayout::load(const QJsonObject &state)
{
    if (state.value(QStringLiteral("version")).toInt() != 2) {
        return;
    }
    const QJsonObject windows = state.value(QStringLiteral("windows")).toObject();
    for (auto it = windows.begin(); it != windows.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        SpatialWindow window;
        window.appId = it.key().toStdString();
        window.pose = {Quat::fromYaw(float(o.value(QStringLiteral("yaw")).toDouble())), vecFromJson(o.value(QStringLiteral("position")))};
        window.scale = std::clamp(float(o.value(QStringLiteral("scale")).toDouble(1.0)), kMinScale, kMaxScale);
        window.pixelScale = std::clamp(float(o.value(QStringLiteral("pixelScale")).toDouble(1.0)), 0.5f, 4.0f); // none: before HiDPI
        window.tilt = std::clamp(float(o.value(QStringLiteral("tilt")).toDouble()), -kMaxTilt, kMaxTilt);
        window.turn = std::clamp(float(o.value(QStringLiteral("turn")).toDouble()), -kMaxTurn, kMaxTurn);
        window.curvature = std::clamp(float(o.value(QStringLiteral("curvature")).toDouble()), 0.0f, 1.0f);
        window.pinned = o.value(QStringLiteral("pinned")).toBool();
        window.locked = o.value(QStringLiteral("locked")).toBool(); // none (before the padlock): not locked
        window.follow = o.value(QStringLiteral("follow")).toBool();
        window.followAzimuth = float(o.value(QStringLiteral("followAzimuth")).toDouble());
        window.followDistance = float(o.value(QStringLiteral("followDistance")).toDouble(m_options.distance));
        window.followHeight = float(o.value(QStringLiteral("followHeight")).toDouble(m_options.height));
        m_remembered[window.appId] = window;
    }
}

bool SpatialLayout::takeDirty()
{
    const bool dirty = m_dirty;
    m_dirty = false;
    return dirty;
}

} // namespace RiftHome::Spatial
