// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "wayland/Listener.hpp"

#include <QList>

#include <cstdint>

struct wlr_input_device;
struct wlr_keyboard;

namespace RiftHome::Wayland
{

class Server;

/*!
 * Keyboard and mouse from the backend (the nested window's in development) to the focused window. The
 * controllers (Touch) have their own pointer input.
 */
class Input
{
public:
    explicit Input(Server &server);
    ~Input();
    Input(const Input &) = delete;
    Input &operator=(const Input &) = delete;

    void addDevice(wlr_input_device *device);
    //! A pointer without a device (the controllers' rays): the seat offers wl_pointer.
    void setVirtualPointer(bool on)
    {
        m_virtualPointer = on;
        updateCapabilities();
    }
    /*!
     * A keyboard without a device (Rift Home's virtual keyboard): the seat offers wl_keyboard (US
     * layout) even with no physical keyboard, and typeKey() sends keys to the focused window.
     */
    void enableVirtualKeyboard();
    //! A key (linux evdev code) down or up on the virtual keyboard; modifiers follow from the keys (xkb state).
    void typeKey(uint32_t code, bool pressed, uint32_t timeMs);

private:
    struct Keyboard;

    void processMotion(uint32_t timeMsec);
    void updateCapabilities();

    Server &m_server;
    QList<Keyboard *> m_keyboards;
    wlr_keyboard *m_virtualKeyboard = nullptr; //!< owned (initialised by wlr_keyboard_init)
    bool m_hasPointer = false;
    bool m_virtualPointer = false;
    Listener m_motion;
    Listener m_motionAbsolute;
    Listener m_button;
    Listener m_axis;
    Listener m_frame;
};

} // namespace RiftHome::Wayland
