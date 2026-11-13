// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "xr/XrInput.hpp"

#include <QLoggingCategory>

#include <cstring>
#include <string>
#include <vector>

Q_DECLARE_LOGGING_CATEGORY(lcXr)

namespace RiftHome::Xr
{

namespace
{

XrPath path(XrInstance instance, const char *text)
{
    XrPath p = XR_NULL_PATH;
    xrStringToPath(instance, text, &p);
    return p;
}

XrAction createAction(XrActionSet set, const char *name, const char *localized, XrActionType type, const XrPath *hands)
{
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(info.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    info.actionType = type;
    info.countSubactionPaths = 2;
    info.subactionPaths = hands;
    XrAction action = XR_NULL_HANDLE;
    if (XR_FAILED(xrCreateAction(set, &info, &action))) {
        qCWarning(lcXr) << "action" << name << "not created";
    }
    return action;
}

} // namespace

bool XrInput::init(XrInstance instance, XrSession session)
{
    m_instance = instance;
    m_session = session;
    m_hands[0] = path(instance, "/user/hand/left");
    m_hands[1] = path(instance, "/user/hand/right");

    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(setInfo.actionSetName, "rifthome", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    std::strncpy(setInfo.localizedActionSetName, "Rift Home", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    if (XR_FAILED(xrCreateActionSet(instance, &setInfo, &m_set))) {
        qCWarning(lcXr) << "no action set";
        return false;
    }
    m_aim = createAction(m_set, "aim", "Point", XR_ACTION_TYPE_POSE_INPUT, m_hands);
    m_grip = createAction(m_set, "grip", "Hold", XR_ACTION_TYPE_POSE_INPUT, m_hands);
    m_trigger = createAction(m_set, "select", "Click", XR_ACTION_TYPE_FLOAT_INPUT, m_hands);
    m_squeeze = createAction(m_set, "grab", "Grab a window", XR_ACTION_TYPE_FLOAT_INPUT, m_hands);
    m_primary = createAction(m_set, "secondary_click", "Right click", XR_ACTION_TYPE_BOOLEAN_INPUT, m_hands);
    m_secondary = createAction(m_set, "gamepad_b", "Gamepad B (game mode)", XR_ACTION_TYPE_BOOLEAN_INPUT, m_hands);
    m_dock = createAction(m_set, "dock", "Dock (X)", XR_ACTION_TYPE_BOOLEAN_INPUT, m_hands);
    m_stick = createAction(m_set, "scroll", "Scroll; nearer/farther and size while grabbing", XR_ACTION_TYPE_VECTOR2F_INPUT, m_hands);
    m_stickClick = createAction(m_set, "right_click_stick", "Right click (thumbstick)", XR_ACTION_TYPE_BOOLEAN_INPUT, m_hands);
    m_haptic = createAction(m_set, "haptic", "Haptics", XR_ACTION_TYPE_VIBRATION_OUTPUT, m_hands);
    m_home = createAction(m_set, "home", "Home (Y)", XR_ACTION_TYPE_BOOLEAN_INPUT, m_hands);
    m_menu = createAction(m_set, "home_menu", "Home (menu button)", XR_ACTION_TYPE_BOOLEAN_INPUT, m_hands);

    std::vector<XrActionSuggestedBinding> bindings;
    const auto bind = [&](XrAction action, const char *suffix) {
        for (const char *hand : {"/user/hand/left", "/user/hand/right"}) {
            bindings.push_back({action, path(instance, (std::string(hand) + suffix).c_str())});
        }
    };
    bind(m_aim, "/input/aim/pose");
    bind(m_grip, "/input/grip/pose");
    bind(m_trigger, "/input/trigger/value");
    bind(m_squeeze, "/input/squeeze/value");
    bind(m_stick, "/input/thumbstick");
    bind(m_stickClick, "/input/thumbstick/click");
    bind(m_haptic, "/output/haptic");
    bindings.push_back({m_primary, path(instance, "/user/hand/right/input/a/click")});
    bindings.push_back({m_secondary, path(instance, "/user/hand/right/input/b/click")});
    bindings.push_back({m_dock, path(instance, "/user/hand/left/input/x/click")});
    bindings.push_back({m_home, path(instance, "/user/hand/left/input/y/click")});
    bindings.push_back({m_menu, path(instance, "/user/hand/left/input/menu/click")});
    XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile = path(instance, "/interaction_profiles/oculus/touch_controller");
    suggested.countSuggestedBindings = uint32_t(bindings.size());
    suggested.suggestedBindings = bindings.data();
    if (XR_FAILED(xrSuggestInteractionProfileBindings(instance, &suggested))) {
        qCWarning(lcXr) << "the runtime refused the Touch controller bindings";
        return false;
    }

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &m_set;
    if (XR_FAILED(xrAttachSessionActionSets(session, &attach))) {
        qCWarning(lcXr) << "action set not attached";
        return false;
    }
    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        spaceInfo.action = m_aim;
        spaceInfo.subactionPath = m_hands[h];
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        xrCreateActionSpace(session, &spaceInfo, &m_aimSpaces[h]);
        spaceInfo.action = m_grip;
        xrCreateActionSpace(session, &spaceInfo, &m_gripSpaces[h]);
    }
    qCInfo(lcXr) << "Touch controllers: actions bound (point, click, grab, right click on A and the thumbstick, scroll, dock on X, home on Y and the menu button, haptics)";
    return true;
}

void XrInput::destroy()
{
    for (XrSpace *spaces : {m_aimSpaces, m_gripSpaces}) {
        for (int h = 0; h < 2; ++h) {
            if (spaces[h] != XR_NULL_HANDLE) {
                xrDestroySpace(spaces[h]);
                spaces[h] = XR_NULL_HANDLE;
            }
        }
    }
    if (m_set != XR_NULL_HANDLE) {
        xrDestroyActionSet(m_set); // its actions with it
        m_set = XR_NULL_HANDLE;
    }
}

bool XrInput::sync(XrSpace base, XrTime time, Hand hands[2])
{
    for (int h = 0; h < 2; ++h) {
        hands[h] = Hand{};
    }
    if (m_set == XR_NULL_HANDLE) {
        return false;
    }
    XrActiveActionSet active{m_set, XR_NULL_PATH};
    XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &active;
    const XrResult result = xrSyncActions(m_session, &syncInfo);
    if (result != XR_SUCCESS) {
        return false; // XR_SESSION_NOT_FOCUSED: the Rift panel or another app has the input
    }
    for (int h = 0; h < 2; ++h) {
        Hand &hand = hands[h];
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.subactionPath = m_hands[h];

        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(m_aimSpaces[h], base, time, &location))) {
            const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
            hand.tracked = (location.locationFlags & needed) == needed;
            const XrPosef &p = location.pose;
            hand.aim = {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w}, {p.position.x, p.position.y, p.position.z}};
        }
        location = {XR_TYPE_SPACE_LOCATION};
        hand.gripTracked = false;
        if (m_gripSpaces[h] != XR_NULL_HANDLE && XR_SUCCEEDED(xrLocateSpace(m_gripSpaces[h], base, time, &location))) {
            const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
            hand.gripTracked = (location.locationFlags & needed) == needed;
            const XrPosef &p = location.pose;
            hand.grip = {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w}, {p.position.x, p.position.y, p.position.z}};
        }
        XrActionStateFloat f{XR_TYPE_ACTION_STATE_FLOAT};
        get.action = m_trigger;
        if (XR_SUCCEEDED(xrGetActionStateFloat(m_session, &get, &f)) && f.isActive) {
            hand.trigger = f.currentState;
        }
        get.action = m_squeeze;
        f = {XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_SUCCEEDED(xrGetActionStateFloat(m_session, &get, &f)) && f.isActive) {
            hand.squeeze = f.currentState;
        }
        XrActionStateBoolean b{XR_TYPE_ACTION_STATE_BOOLEAN};
        get.action = m_primary;
        if (XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &get, &b)) && b.isActive) {
            hand.primary = b.currentState;
        }
        b = {XR_TYPE_ACTION_STATE_BOOLEAN};
        get.action = m_secondary;
        if (XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &get, &b)) && b.isActive) {
            hand.secondary = b.currentState;
        }
        b = {XR_TYPE_ACTION_STATE_BOOLEAN};
        get.action = m_dock;
        if (XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &get, &b)) && b.isActive) {
            hand.dock = b.currentState;
        }
        b = {XR_TYPE_ACTION_STATE_BOOLEAN};
        get.action = m_home;
        if (XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &get, &b)) && b.isActive) {
            hand.home = b.currentState;
        }
        b = {XR_TYPE_ACTION_STATE_BOOLEAN};
        get.action = m_menu;
        if (XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &get, &b)) && b.isActive) {
            hand.menu = b.currentState;
            hand.menuChanged = b.lastChangeTime;
        }
        b = {XR_TYPE_ACTION_STATE_BOOLEAN};
        get.action = m_stickClick;
        if (XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &get, &b)) && b.isActive) {
            hand.stickClick = b.currentState;
        }
        XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
        get.action = m_stick;
        if (XR_SUCCEEDED(xrGetActionStateVector2f(m_session, &get, &v)) && v.isActive) {
            hand.stickX = v.currentState.x;
            hand.stickY = v.currentState.y;
        }
    }
    return true;
}

void XrInput::haptic(int hand, float amplitude, int64_t durationNs)
{
    if (m_haptic == XR_NULL_HANDLE) {
        return;
    }
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = amplitude;
    vibration.duration = durationNs;
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
    info.action = m_haptic;
    info.subactionPath = m_hands[hand];
    xrApplyHapticFeedback(m_session, &info, reinterpret_cast<const XrHapticBaseHeader *>(&vibration));
}

} // namespace RiftHome::Xr
