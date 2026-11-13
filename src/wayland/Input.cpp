// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "wayland/Input.hpp"

#include "wayland/Logging.hpp"
#include "wayland/Server.hpp"
#include "wayland/Window.hpp"
#include "wayland/wlr.hpp"

namespace RiftHome::Wayland
{

struct Input::Keyboard {
    Keyboard(Input &input, wlr_input_device *device)
        : input(input)
        , keyboard(wlr_keyboard_from_input_device(device))
    {
        xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        xkb_keymap *keymap = xkb_keymap_new_from_names(context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
        wlr_keyboard_set_keymap(keyboard, keymap);
        xkb_keymap_unref(keymap);
        xkb_context_unref(context);
        wlr_keyboard_set_repeat_info(keyboard, 25, 600);

        wlr_seat *seat = input.m_server.seat;
        modifiers.connect(&keyboard->events.modifiers, [this, seat](void *) {
            wlr_seat_set_keyboard(seat, keyboard);
            wlr_seat_keyboard_notify_modifiers(seat, &keyboard->modifiers);
        });
        key.connect(&keyboard->events.key, [this, seat](void *data) {
            auto *event = static_cast<wlr_keyboard_key_event *>(data);
            if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED && this->input.m_server.isDrm()) {
                // Its own session: Ctrl+Alt+F<n> another console, Ctrl+Alt+Backspace leave (twice); not for the apps.
                const xkb_keysym_t *syms = nullptr;
                const int count = xkb_state_key_get_syms(keyboard->xkb_state, event->keycode + 8, &syms);
                const uint32_t mods = wlr_keyboard_get_modifiers(keyboard);
                for (int i = 0; i < count; ++i) {
                    if (syms[i] >= XKB_KEY_XF86Switch_VT_1 && syms[i] <= XKB_KEY_XF86Switch_VT_12) {
                        this->input.m_server.switchVt(unsigned(syms[i] - XKB_KEY_XF86Switch_VT_1 + 1));
                        return;
                    }
                    if ((mods & WLR_MODIFIER_CTRL) && (mods & WLR_MODIFIER_ALT)
                        && (syms[i] == XKB_KEY_BackSpace || syms[i] == XKB_KEY_Terminate_Server)) {
                        this->input.m_server.leaveKey();
                        return;
                    }
                }
            }
            wlr_seat_set_keyboard(seat, keyboard);
            wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
        });
        destroy.connect(&device->events.destroy, [this](void *) {
            this->input.m_keyboards.removeOne(this);
            this->input.updateCapabilities();
            delete this;
        });
        wlr_seat_set_keyboard(seat, keyboard);
    }

    Input &input;
    wlr_keyboard *keyboard;
    Listener modifiers;
    Listener key;
    Listener destroy;
};

Input::Input(Server &server)
    : m_server(server)
{
    wlr_cursor *cursor = server.cursor;
    m_motion.connect(&cursor->events.motion, [this, cursor](void *data) {
        auto *event = static_cast<wlr_pointer_motion_event *>(data);
        // Relative motion always (games read it); a locked pointer (mouse look) leaves the cursor where it is.
        m_server.sendRelativeMotion(uint64_t(event->time_msec) * 1000, event->delta_x, event->delta_y, event->unaccel_dx, event->unaccel_dy);
        if (m_server.pointerLocked()) {
            return;
        }
        wlr_cursor_move(cursor, &event->pointer->base, event->delta_x, event->delta_y);
        processMotion(event->time_msec);
    });
    m_motionAbsolute.connect(&cursor->events.motion_absolute, [this, cursor](void *data) {
        auto *event = static_cast<wlr_pointer_motion_absolute_event *>(data);
        wlr_cursor_warp_absolute(cursor, &event->pointer->base, event->x, event->y);
        processMotion(event->time_msec);
    });
    m_button.connect(&cursor->events.button, [this](void *data) {
        auto *event = static_cast<wlr_pointer_button_event *>(data);
        // Its own session: the monitor page's buttons first.
        if (event->state == WL_POINTER_BUTTON_STATE_PRESSED && m_server.monitorClick(m_server.cursor->x, m_server.cursor->y)) {
            return;
        }
        wlr_seat_pointer_notify_button(m_server.seat, event->time_msec, event->button, event->state);
        if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
            wlr_surface *surface = nullptr;
            double sx = 0, sy = 0;
            m_server.focusWindow(m_server.windowAt(m_server.cursor->x, m_server.cursor->y, &surface, &sx, &sy));
        }
    });
    m_axis.connect(&cursor->events.axis, [this](void *data) {
        auto *event = static_cast<wlr_pointer_axis_event *>(data);
        wlr_seat_pointer_notify_axis(m_server.seat, event->time_msec, event->orientation, event->delta,
                                     event->delta_discrete, event->source, event->relative_direction);
    });
    m_frame.connect(&cursor->events.frame, [this](void *) {
        wlr_seat_pointer_notify_frame(m_server.seat);
    });
}

Input::~Input()
{
    qDeleteAll(m_keyboards);
    if (m_virtualKeyboard) {
        wlr_keyboard_finish(m_virtualKeyboard);
        delete m_virtualKeyboard;
    }
}

void Input::enableVirtualKeyboard()
{
    if (m_virtualKeyboard) {
        return;
    }
    static const wlr_keyboard_impl impl = {"rift-home-virtual-keyboard", nullptr};
    m_virtualKeyboard = new wlr_keyboard{};
    wlr_keyboard_init(m_virtualKeyboard, &impl, "Rift Home virtual keyboard");
    // As a device: its keys and modifiers reach the seat the way a physical keyboard's do.
    m_keyboards.append(new Keyboard(*this, &m_virtualKeyboard->base));
    updateCapabilities();
}

void Input::typeKey(uint32_t code, bool pressed, uint32_t timeMs)
{
    if (!m_virtualKeyboard) {
        return;
    }
    wlr_keyboard_key_event event{};
    event.time_msec = timeMs;
    event.keycode = code;
    event.update_state = true; // xkb works out the modifiers from Shift/Ctrl/Alt keys
    event.state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED;
    wlr_keyboard_notify_key(m_virtualKeyboard, &event);
}

void Input::addDevice(wlr_input_device *device)
{
    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        m_keyboards.append(new Keyboard(*this, device));
        break;
    case WLR_INPUT_DEVICE_POINTER:
        wlr_cursor_attach_input_device(m_server.cursor, device);
        m_hasPointer = true;
        break;
    default:
        break;
    }
    updateCapabilities();
}

void Input::updateCapabilities()
{
    uint32_t capabilities = 0;
    if (m_hasPointer || m_virtualPointer) {
        capabilities |= WL_SEAT_CAPABILITY_POINTER;
    }
    if (!m_keyboards.isEmpty()) {
        capabilities |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(m_server.seat, capabilities);
}

void Input::processMotion(uint32_t timeMsec)
{
    wlr_surface *surface = nullptr;
    double sx = 0, sy = 0;
    m_server.windowAt(m_server.cursor->x, m_server.cursor->y, &surface, &sx, &sy);
    if (!surface) {
        wlr_cursor_set_xcursor(m_server.cursor, m_server.cursorManager, "default");
        wlr_seat_pointer_clear_focus(m_server.seat);
        return;
    }
    wlr_seat_pointer_notify_enter(m_server.seat, surface, sx, sy);
    m_server.updatePointerConstraint();
    wlr_seat_pointer_notify_motion(m_server.seat, timeMsec, sx, sy);
}

} // namespace RiftHome::Wayland
