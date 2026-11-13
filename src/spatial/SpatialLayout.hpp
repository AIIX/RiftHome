// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "spatial/Math.hpp"

#include <QJsonObject>

#include <algorithm>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace RiftHome::Spatial
{

//! A mapped window as the layout sees it.
struct WindowInfo {
    int windowId = 0;
    std::string appId;
    int widthPx = 0;
    int heightPx = 0;
    bool fullscreen = false;
    float pixelScale = 1.0f; //!< window pixels a unit of the app's own (the apps' scale; 1 for X11 apps)
};

//! Where a window is in the room and how it behaves. Its pose is upright (a yaw facing the user); its turn and tilt
//! turn its face left/right and up/down (surfacePose()), fullscreen screens too.
struct SpatialWindow {
    int windowId = 0;
    std::string appId;
    int widthPx = 0;
    int heightPx = 0;
    //! Window pixels a unit of the app's own (WindowInfo::pixelScale). Saved with its scale: a window placed when its app
    //! drew at another scale (or before HiDPI: 1) comes back at the same size in the room.
    float pixelScale = 1.0f;
    Pose pose;
    float scale = 1.0f;
    float tilt = 0.0f; //!< radians; positive: the face turns up (the top leans back)
    float turn = 0.0f; //!< radians, on top of facing the user; positive: the face turns to the user's right
    bool pinned = false; //!< stays where it is in the room when the user recentres
    bool follow = false;
    // Follow mode: where the window sits in the following group (relative to its yaw and centre).
    float followAzimuth = 0.0f;
    float followDistance = 1.6f;
    float followHeight = -0.1f;
    // Fullscreen (cinema): where it was before, to go back to.
    bool fullscreen = false;
    Pose windowedPose;
    float windowedScale = 1.0f;
    float windowedTilt = 0.0f; //!< its tilt and turn before; a fullscreen screen starts upright, facing the user
    float windowedTurn = 0.0f;
    //! Bent around a vertical axis in front of it (a small snap slider with reset in Settings):
    //! 0 flat, 1 bent through kMaxCurveAngle (the same shape at any size).
    float curvature = 0.0f;
    bool minimized = false; //!< hidden in the room (still running); Home's running apps bring it back
};

//! A window bent all the way (curvature 1) spans this angle around its axis.
constexpr float kMaxCurveAngle = 1.5708f; // 90 degrees
//! Less curvature than this is flat (the slider snaps to flat).
constexpr float kCurveSnap = 0.06f;

//! A ray on a window: window pixels (main surface coordinates) and the point in space.
struct Hit {
    int windowId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float distance = 0.0f;
    Vec3 point;
    //! Window-local metres along its surface (flat or curved): from its centre, x right, y up (as FrameLayout's).
    float localX = 0.0f;
    float localY = 0.0f;
};

struct LayoutOptions {
    float pixelsPerMeter = 1000.0f;
    float distance = 1.6f;
    float height = -0.1f;
    float gapDegrees = 2.5f;
    float cinemaWidth = 2.6f; //!< metres: a fullscreen window (about 60 degrees at 2.4 m)
    float cinemaDistance = 2.4f;
    float theaterWidth = 3.6f; //!< metres: cinema mode's screen (about 54 degrees at 3.5 m)
    float theaterDistance = 3.5f;
    float mainWidth = 1.92f; //!< metres: a window made the main one (about 60 degrees at 1.6 m)
};

/*!
 * A recentre's movement from the head poses around it (oldest first, one per frame, as located in LOCAL): LOCAL jumps,
 * so the located head jumps between two frames while the real head barely moves. Returns old LOCAL coordinates to new
 * ones (upright: yaw and offset), or nothing when no frame-to-frame jump stands out (nothing measurable moved).
 * RiftLinux's runtime moves LOCAL before it sends the event, without the pose, so the jump may lie before the event.
 */
std::optional<Pose> recentreFromHeads(const std::vector<Pose> &heads);

/*!
 * Rift Home's windows in the room: new windows open in front of the user next to the others, each facing
 * them (as visionOS does); grabbed windows follow the controller, nearer or farther, larger or smaller; follow-mode
 * windows stay in front, catching up slowly; poses are kept through recentering and remembered by app id. Pure: no
 * OpenXR, no Wayland. All poses are in one space (OpenXR's LOCAL).
 */
class SpatialLayout
{
public:
    explicit SpatialLayout(const LayoutOptions &options = {});

    //! The mapped windows now: new ones placed (remembered pose, else around the origin where @p head looks), gone
    //! ones remembered.
    void sync(const std::vector<WindowInfo> &windows, const Pose &head);
    const std::map<int, SpatialWindow> &windows() const
    {
        return m_windows;
    }
    float widthMeters(const SpatialWindow &window) const;
    float heightMeters(const SpatialWindow &window) const;
    float pixelsPerMeter() const
    {
        return m_options.pixelsPerMeter;
    }

    //! The window's plane in the room: its pose with its tilt. A curved window touches this plane at its centre line.
    Pose surfacePose(const SpatialWindow &window) const;
    //! A curved window's radius (metres; its axis is surface-local Y through (0, 0, radius)); 0 when flat.
    float curveRadius(const SpatialWindow &window) const;
    /*!
     * The point @p x, @p y window-local metres along the window's surface (flat or curved; beyond its edges the curve
     * goes on), @p lift metres in front of it, with the surface's orientation there (+Z its front).
     */
    Pose pointOn(const SpatialWindow &window, float x, float y, float lift = 0.0f) const;
    //! The nearest window the ray meets on its front (only @p onlyWindow when given: cinema mode).
    std::optional<Hit> hitTest(Vec3 origin, Vec3 direction, int onlyWindow = 0) const;
    //! Where the ray meets @p windowId's plane, inside the window or not (a drag that leaves the window).
    std::optional<Hit> projectOnto(int windowId, Vec3 origin, Vec3 direction) const;

    //! Grab @p windowId where the ray from @p controller meets it, @p hitDistance along the ray.
    bool beginGrab(int windowId, const Pose &controller, float hitDistance);
    /*!
     * Move the grabbed window with the controller, facing @p head. @p pushPull (-1..1, thumbstick forward = +1)
     * moves it farther or nearer, @p grow (-1..1) scales it; @p dt in seconds.
     */
    void updateGrab(const Pose &controller, const Pose &head, float pushPull, float grow, float dt);
    /*!
     * How much the arrangement curves around the user (Settings; with a small circle, moved windows turned toward the
     * user far too much). Windows open and are moved along an arc whose axis is behind the user, its
     * radius the distance divided by this: 1 a circle around the user, each window facing them (moving one swings it
     * around the hand, as before); towards 0 a flat wall, all facing the same way (moving one slides it along the wall).
     */
    void setArrangementCurve(float curve);
    float arrangementCurve() const
    {
        return m_curve;
    }
    void endGrab();
    int grabbedWindow() const
    {
        return m_grab ? m_grab->windowId : 0;
    }

    //! Scale @p windowId by dragging a point of its plane (its corner handle): the point under the ray stays under it.
    bool beginScale(int windowId, Vec3 origin, Vec3 direction);
    void updateScale(Vec3 origin, Vec3 direction);
    /*!
     * Tilt and turn @p windowId by dragging the point the ray is on (its tilt bar): up/down tilts, left/right turns,
     * however the hand gets the ray there (moving the hand without turning the wrist did nothing
     * when tilt followed the controller's rotation).
     */
    bool beginTilt(int windowId, Vec3 origin, Vec3 direction);
    void updateTilt(Vec3 origin, Vec3 direction);
    //! Back upright, facing the user (no tilt, no turn).
    void resetTilt(int windowId);
    /*!
     * Tilt and turn @p windowId by these radians (the thumbstick while the ray is on its tilt bar; tilting by the
     * controller's own angle was very difficult to control); kept within 60 degrees each way.
     */
    void adjustTilt(int windowId, float tilt, float turn);
    //! Ends a scale or tilt drag.
    void endDrag();

    //! Minimized (hidden, out of reach, still running) or back.
    void setMinimized(int windowId, bool minimized);

    //! The window's curvature (0..1; below kCurveSnap: flat).
    void setCurvature(int windowId, float curvature);
    //! More or less curved by @p delta (the thumbstick on the curve slider); passes over the snap band in one step.
    void adjustCurvature(int windowId, float delta);
    int draggedWindow() const
    {
        return m_drag ? m_drag->windowId : 0;
    }

    //! Make @p windowId the main window: straight in front of @p head, upright, about 60 degrees wide; windows in
    //! the way move aside.
    void makeMain(int windowId, const Pose &head);
    //! Pinned (anchored in the room) or not; returns the new state.
    bool togglePin(int windowId);
    /*!
     * Cinema mode: @p windowId alone as a large screen straight in front of @p head (the others are
     * hidden by the XR side, the room dimmed); leaveCinema() puts it back exactly where it was.
     */
    void enterCinema(int windowId, const Pose &head);
    void leaveCinema();
    int cinemaWindow() const
    {
        return m_cinema;
    }

    /*!
     * Follow mode on or off; returns the new state. Following windows move as one group (two windows
     * following on their own overlapped as they caught up at different moments): one yaw and centre for all, each
     * window where it was in the group, so the arrangement and what is in front stay as they were.
     */
    bool toggleFollow(int windowId, const Pose &head);
    //! Every window follows (the dock's Follow all), or none.
    void setFollowAll(bool on, const Pose &head);
    bool followAll() const;
    /*!
     * New windows (without a remembered place) open where the user looks, a little nearer than a window already there,
     * instead of in the nearest free place beside the others (dialogs opened far left and right). Off
     * for the windows a session starts with (they line up side by side).
     */
    void setOpenInFront(bool on)
    {
        m_openInFront = on;
    }
    //! The group catches up when the user has turned or walked away by more than a threshold.
    void updateFollow(const Pose &head, float dt);

    /*!
     * The user recentred (LOCAL moved to their head; RiftLinux's runtime does not say by how much): the whole
     * arrangement is re-expressed so its home is the new origin, facing LOCAL's forward, i.e. straight in front of the
     * user, every window where it was relative to the others (as visionOS's recentre). Pinned windows stay where they
     * are in the room when @p oldToNew (old LOCAL coordinates to new ones, from the head pose before and after) is
     * known.
     */
    void recentered(const std::optional<Pose> &oldToNew = std::nullopt);
    //! The layout's space moved by a known amount: @p newOriginInOldSpace from OpenXR's reference space change event.
    void spaceChanged(const Pose &newOriginInOldSpace);
    //! The arrangement's home: where the user was, facing where they looked, when the first window opened; the LOCAL
    //! origin after a recentre. Windows open around it; placements are remembered relative to it.
    const Pose &home() const
    {
        return m_home;
    }

    //! Remembered placements by app id, relative to the home (version 2), for the state file.
    QJsonObject save() const;
    void load(const QJsonObject &state);
    //! Something worth saving changed since the last call.
    bool takeDirty();

private:
    Pose placeNew(float width, const Pose &head, int exclude = 0) const;
    //! Windows in the way of @p windowId (overlapping it as seen from the eyes, nearer or about as far) move aside to the
    //! nearest free place: Make Main, and a window going fullscreen (a game started from Steam opened behind it).
    void moveAsideFrom(int windowId, const Pose &head);
    /*!
     * The windows @p windowId moved aside back where they were, now that it is gone (closed, minimized, out of
     * fullscreen): Settings opened in front pushed Chromium aside for good. Only those the user has not
     * moved since, and only where the place is free.
     */
    void restoreMovedAsideBy(int windowId);
    Pose placeInFront(float width, const Pose &head) const;
    //! The ray on @p window's surface (front only); @p withinWindow: only on the window itself, else anywhere on its
    //! plane or (curved) the half of its cylinder in front of the axis.
    std::optional<Hit> intersect(const SpatialWindow &window, Vec3 origin, Vec3 direction, bool withinWindow) const;
    void remember(const SpatialWindow &window);
    void ensureHome(const Pose &head);

    LayoutOptions m_options;
    std::map<int, SpatialWindow> m_windows;
    std::map<std::string, SpatialWindow> m_remembered; //!< poses relative to the home
    Pose m_home;
    bool m_homeSet = false;
    struct Grab {
        int windowId;
        float distance;
        Vec3 offsetLocal; //!< the grabbed point in the window's own coordinates, unscaled metres
        // Less than fully curved: the arc the grabbed point moves along (set at the first move).
        bool onArc = false;
        Vec3 centre;
        float radius = 0.0f;
    };
    //! The arrangement's arc through the point @p distance from @p eye at @p azimuth: its axis and radius.
    struct Arc {
        Vec3 centre;
        float radius;
    };
    Arc arcThrough(Vec3 eye, float azimuth, float distance) const;
    /*!
     * The axis a window at @p position turns to: on the arrangement's arc through it, the arcs' axes straight back from
     * the home's forward, behind @p eye (fully curved: @p eye itself). Depends on where the window is only, so a window
     * turns the same wherever and however often it is grabbed (the turn changed at the next grab and a
     * window brought back to the middle stayed turned, as the arc was taken from the window's direction when grabbed).
     */
    Vec3 arcAxisFor(Vec3 position, Vec3 eye) const;
    //! @p window turned where it stands as its place on the arc says (not pinned, following or a fullscreen screen).
    void followArc(SpatialWindow &window, Vec3 eye) const;
    //! The yaw of a window at @p position: facing its arc's axis (arcAxisFor()).
    float arcYaw(Vec3 position, Vec3 eye) const
    {
        return facingYaw(position, arcAxisFor(position, eye));
    }
    float m_curve = 1.0f; //!< as before until Settings say (their default: 0.5)
    std::optional<Grab> m_grab;
    struct Drag {
        enum class Kind {
            Scale,
            Tilt,
        };
        Kind kind;
        int windowId;
        float start; //!< scale: the dragged point's distance from the centre (unscaled metres); tilt: the ray's pitch
        float initial; //!< the window's scale or tilt then
        float initialTurn = 0.0f; //!< tilt: the window's turn then
        Pose plane = {}; //!< tilt: the window's plane when the drag began (the dragged point is measured on it)
        float startX = 0.0f; //!< tilt: where the ray met it, plane metres
        float startY = 0.0f;
    };
    std::optional<Drag> m_drag;
    //! A window moved aside (moveAsideFrom): by which window, where it was, where it went (oldest first).
    struct MovedAside {
        int windowId;
        int by;
        Pose before;
        Pose after;
    };
    std::vector<MovedAside> m_movedAside;
    int m_cinema = 0;
    bool m_openInFront = false;
    SpatialWindow m_beforeCinema; //!< the cinema window as it was
    //! Where the user put an app's cinema screen and how wide (lost when leaving and coming back).
    struct CinemaPlace {
        Pose pose; //!< relative to the home
        float width = 0.0f; //!< metres
    };
    std::map<std::string, CinemaPlace> m_cinemaPlaces;
    void rememberCinemaPlace();
    // The following group: the yaw and centre its windows are placed around (the head's when the group formed).
    float m_groupYaw = 0.0f;
    Vec3 m_groupCentre;
    bool m_groupSet = false;
    bool m_groupCatching = false;
    void joinGroup(SpatialWindow &window, const Pose &head);
    bool m_dirty = false;
};

} // namespace RiftHome::Spatial
