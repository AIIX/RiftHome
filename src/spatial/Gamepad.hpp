// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace RiftHome::Spatial
{

/*!
 * Game mode: the Touch controllers as an Xbox-style gamepad for flatscreen games, Rift Home's own
 * virtual pad (Linux joystick conventions: sticks -32768..32767 with y down, triggers 0..255). Pure.
 */
struct TouchInput {
    float stickX = 0.0f; //!< -1..1, right
    float stickY = 0.0f; //!< -1..1, up (OpenXR)
    float trigger = 0.0f; //!< 0..1
    float squeeze = 0.0f; //!< 0..1 (the grip)
    bool lower = false; //!< A (right) / X (left)
    bool upper = false; //!< B (right) / Y (left)
    bool stickClick = false;
};

struct GamepadState {
    int16_t lx = 0;
    int16_t ly = 0;
    int16_t rx = 0;
    int16_t ry = 0;
    uint8_t lt = 0;
    uint8_t rt = 0;
    bool a = false;
    bool b = false;
    bool x = false;
    bool y = false;
    bool lb = false;
    bool rb = false;
    bool start = false;
    bool thumbl = false;
    bool thumbr = false;
    bool operator==(const GamepadState &) const = default;
};

//! A stick's axis: a small dead zone around the middle, then the whole range.
inline int16_t gamepadAxis(float value)
{
    constexpr float deadzone = 0.08f;
    const float v = std::clamp(value, -1.0f, 1.0f);
    if (std::fabs(v) < deadzone) {
        return 0;
    }
    const float scaled = (std::fabs(v) - deadzone) / (1.0f - deadzone) * (v < 0.0f ? -1.0f : 1.0f);
    return int16_t(std::lround(std::clamp(scaled * 32767.0f, -32768.0f, 32767.0f)));
}

inline uint8_t gamepadTrigger(float value)
{
    return uint8_t(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

//! The pad's state from both controllers (Start is the caller's: the menu button's tap, its hold leaves game mode).
inline GamepadState gamepadFrom(const TouchInput &left, const TouchInput &right)
{
    GamepadState s;
    s.lx = gamepadAxis(left.stickX);
    s.ly = gamepadAxis(-left.stickY); // joysticks count down as positive
    s.rx = gamepadAxis(right.stickX);
    s.ry = gamepadAxis(-right.stickY);
    s.lt = gamepadTrigger(left.trigger);
    s.rt = gamepadTrigger(right.trigger);
    s.a = right.lower;
    s.b = right.upper;
    s.x = left.lower;
    s.y = left.upper;
    s.lb = left.squeeze > 0.5f;
    s.rb = right.squeeze > 0.5f;
    s.thumbl = left.stickClick;
    s.thumbr = right.stickClick;
    return s;
}

} // namespace RiftHome::Spatial
