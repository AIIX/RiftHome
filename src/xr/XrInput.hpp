// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "spatial/Math.hpp"

#include <openxr/openxr.h>

namespace RiftHome::Xr
{

/*!
 * The Touch controllers through OpenXR actions (interaction profile /interaction_profiles/oculus/touch_controller):
 * aim pose, trigger, grip, A (right click), X (the dock), the left menu button (home, as Meta's universal menu) and Y
 * (home too), thumbstick and its click, haptics. B is free. Focus losses were the right Oculus button (the
 * runtime's Rift panel holds the focus while open), not the menu button (ten menu
 * presses made no focus change). Menu presses stay logged with the session's state changes.
 */
class XrInput
{
public:
    struct Hand {
        bool tracked = false; //!< aim pose valid this frame
        Spatial::Pose aim;
        bool gripTracked = false; //!< grip pose valid this frame: the controller is drawn there
        Spatial::Pose grip; //!< the controller's body (OpenXR's grip pose: -Z up the handle, -Y ahead, +X right)
        float trigger = 0.0f;
        float squeeze = 0.0f;
        bool primary = false; //!< A (right controller): right click
        bool secondary = false; //!< B (right controller): the gamepad's B in game mode
        bool dock = false; //!< X (left controller): the dock
        bool stickClick = false;
        bool home = false; //!< Y (left controller): home
        bool menu = false; //!< the left menu button: home
        XrTime menuChanged = 0; //!< the runtime's time of the menu button's last change (logged)
        float stickX = 0.0f;
        float stickY = 0.0f;
    };

    //! Before the session's first xrSyncActions; false (logged) when the runtime takes none of it.
    bool init(XrInstance instance, XrSession session);
    void destroy();
    //! The hands' state at @p time in @p base; false when the session has no input focus.
    bool sync(XrSpace base, XrTime time, Hand hands[2]);
    void haptic(int hand, float amplitude, int64_t durationNs);

private:
    XrInstance m_instance = XR_NULL_HANDLE;
    XrSession m_session = XR_NULL_HANDLE;
    XrActionSet m_set = XR_NULL_HANDLE;
    XrAction m_aim = XR_NULL_HANDLE;
    XrAction m_grip = XR_NULL_HANDLE;
    XrAction m_trigger = XR_NULL_HANDLE;
    XrAction m_squeeze = XR_NULL_HANDLE;
    XrAction m_primary = XR_NULL_HANDLE;
    XrAction m_secondary = XR_NULL_HANDLE;
    XrAction m_dock = XR_NULL_HANDLE;
    XrAction m_stick = XR_NULL_HANDLE;
    XrAction m_stickClick = XR_NULL_HANDLE;
    XrAction m_haptic = XR_NULL_HANDLE;
    XrAction m_home = XR_NULL_HANDLE;
    XrAction m_menu = XR_NULL_HANDLE;
    XrPath m_hands[2] = {XR_NULL_PATH, XR_NULL_PATH};
    XrSpace m_aimSpaces[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrSpace m_gripSpaces[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
};

} // namespace RiftHome::Xr
