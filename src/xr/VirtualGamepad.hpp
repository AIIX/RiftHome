// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "spatial/Gamepad.hpp"

namespace RiftHome::Xr
{

/*!
 * Rift Home's own gamepad for flatscreen games: a uinput device that looks like an Xbox 360 pad (games
 * and SDL know its layout by its ids), made when game mode first starts and kept until Rift Home quits: out of game
 * mode it rests (nothing pressed), so other programs never get presses meant for Rift Home's pointer. Removed and made
 * again, a game sees its pad go and another come, and often keeps waiting on the one that went (Road Redemption:
 * "controller connected", then no input). /dev/uinput is the user's (Steam's udev rules give it to the active session).
 * XR thread.
 */
class VirtualGamepad
{
public:
    VirtualGamepad() = default;
    ~VirtualGamepad();
    VirtualGamepad(const VirtualGamepad &) = delete;
    VirtualGamepad &operator=(const VirtualGamepad &) = delete;
    //! Moved: the device goes with it (the XR side's state is reset by assignment).
    VirtualGamepad(VirtualGamepad &&other) noexcept
        : m_fd(other.m_fd)
        , m_last(other.m_last)
    {
        other.m_fd = -1;
    }
    VirtualGamepad &operator=(VirtualGamepad &&other) noexcept
    {
        if (this != &other) {
            close();
            m_fd = other.m_fd;
            m_last = other.m_last;
            other.m_fd = -1;
        }
        return *this;
    }

    //! Makes the device (made already: kept); false (logged) when /dev/uinput cannot be used.
    bool open();
    //! Everything let go; the device stays (game mode off: the game keeps its pad for when it comes back).
    void rest();
    void close();
    bool isOpen() const
    {
        return m_fd >= 0;
    }
    //! What changed since the last state, then a report.
    void send(const Spatial::GamepadState &state);

private:
    void writeEvent(int type, int code, int value);
    int m_fd = -1;
    Spatial::GamepadState m_last;
};

} // namespace RiftHome::Xr
