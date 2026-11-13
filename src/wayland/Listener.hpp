// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <wayland-server-core.h>

#include <functional>
#include <utility>

namespace RiftHome::Wayland
{

/*!
 * A wl_listener that calls a C++ function and disconnects itself when destroyed. Never copied or moved: its address
 * is linked into the signal's list. The handler may delete the listener's owner (destroy signals do).
 */
class Listener
{
public:
    Listener()
    {
        m_raw.owner = this;
        m_raw.listener.notify = &Listener::dispatch;
        wl_list_init(&m_raw.listener.link);
    }
    ~Listener()
    {
        disconnect();
    }
    Listener(const Listener &) = delete;
    Listener &operator=(const Listener &) = delete;

    void connect(wl_signal *signal, std::function<void(void *)> handler)
    {
        disconnect();
        m_handler = std::move(handler);
        wl_signal_add(signal, &m_raw.listener);
    }

    void disconnect()
    {
        wl_list_remove(&m_raw.listener.link);
        wl_list_init(&m_raw.listener.link);
    }

private:
    // Standard layout, the wl_listener first: a pointer to it is a pointer to Raw.
    struct Raw {
        wl_listener listener;
        Listener *owner;
    };

    static void dispatch(wl_listener *listener, void *data)
    {
        // A copy, so the handler may destroy this Listener while it runs.
        const auto handler = reinterpret_cast<Raw *>(listener)->owner->m_handler;
        handler(data);
    }

    Raw m_raw{};
    std::function<void(void *)> m_handler;
};

} // namespace RiftHome::Wayland
