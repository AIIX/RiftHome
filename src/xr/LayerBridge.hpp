// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "color/ColorDescription.hpp"

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QStringList>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace RiftHome::Xr
{

struct DmabufPlane {
    int fd = -1; //!< owned (a duplicate of the client's)
    uint32_t offset = 0;
    uint32_t stride = 0;
    uint64_t modifier = 0; //!< its own (P010 from NVIDIA's VA-API: the planes' layouts can differ)
};

//! One buffer a surface committed, handed from the Wayland thread to the XR thread. Owns its file descriptors.
struct BufferHandoff {
    enum class Kind {
        Dmabuf,
        Shm,
    };
    Kind kind = Kind::Dmabuf;
    uint32_t drmFormat = 0;
    uint64_t modifier = 0;
    int width = 0;
    int height = 0;
    std::vector<DmabufPlane> planes; //!< dmabuf
    QByteArray pixels; //!< shm: a copy of the client's pixels
    int stride = 0; //!< shm
    int acquireFence = -1; //!< sync_file that signals when the client's GPU has written the buffer; -1: none. Owned
    Color::LayerPlan plan;
    bool alpha = false; //!< the format has alpha (premultiplied, Wayland's rule)
    uint64_t releaseToken = 0; //!< returned to the Wayland thread when the XR side is done; 0: nothing to release
    uint64_t bufferId = 0; //!< dmabuf: the client buffer's (the XR side keeps its import until it is destroyed); 0: none
    bool newBuffer = false; //!< the buffer's first handoff (its import is asked for at once: LayerBridge::setImportRequest)

    BufferHandoff() = default;
    ~BufferHandoff();
    BufferHandoff(const BufferHandoff &) = delete;
    BufferHandoff &operator=(const BufferHandoff &) = delete;
};

//! One surface of a window: where it sits in the window's visible geometry, in surface pixels (bottom to top).
struct SurfacePlacement {
    uint64_t surfaceId = 0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    //! Part of the window itself (main surface, subsurfaces): cut to the window's rounded geometry. Popups are not.
    bool clipped = true;
};

//! A mapped window and its surfaces, as the XR side places it.
struct WindowPlacement {
    int windowId = 0;
    std::string appId; //!< placements are remembered by it
    std::string title; //!< for Home's running apps
    int minimizeRequests = 0; //!< how often the app asked to be minimized (its own button): a change minimizes it
    int slot = 0; //!< place in front of the user: 0 centre, 1 right, 2 left, 3 further right, ...
    //! The window's visible geometry (xdg_surface.set_window_geometry: without client-side shadows), in window pixels:
    //! the app's own units times pixelScale (surfaces' places and sizes too; input comes back in them).
    int width = 0;
    int height = 0;
    float pixelScale = 1.0f; //!< window pixels a unit of the app's own: the apps' scale (HiDPI), 1 for X11 apps
    bool fullscreen = false; //!< fullscreen (a video): shown as a cinema screen
    bool maximized = false;
    bool x11 = false; //!< an X11 app (XWayland): its fullscreen is the X screen's size, not one Rift Home picks
    bool wantsKeyboard = false; //!< a text field is focused in it (text-input-v3)
    bool focused = false; //!< it has the keyboard focus: the virtual keyboard types into it
    std::vector<SurfacePlacement> surfaces;
};

//! A notification as the card and the dock's list show it (main thread -> XR thread).
struct Notification {
    uint32_t id = 0;
    QString app;
    QString summary;
    QString body;
    QImage icon; //!< null: none
    bool critical = false;
    QStringList actions; //!< its action buttons' labels, left to right
};

//! Pointer input from the controllers, in window pixels (the window's visible geometry), for the Wayland side's seat.
struct PointerEvent {
    enum class Type {
        Motion,
        Leave, //!< the ray left every window
        Button,
        Axis, //!< smooth scroll, surface pixels
        AxisStop,
        // Window commands (the frame's menu).
        Close, //!< ask the window to close
        Maximize,
        Fullscreen,
        Restore, //!< back to the app's own size
        Key, //!< the virtual keyboard: button = the key's linux evdev code (KEY_*), pressed = down/up
        Focus, //!< give the window the keyboard focus (the virtual keyboard opened for it)
        Launch, //!< start an app from the launcher: button = its index in XrCompositor::Options::apps (no window)
        VolumeDown, //!< the dock: the default output's volume (no window)
        VolumeUp,
        MuteToggle,
        OpenSettings, //!< the Settings app on page button (~0u: the page it shows), started if not running (no window)
        Resize, //!< a new window size (the frame's resize handle): x, y = width, height in window pixels
        NotificationActivate, //!< a notification tapped: button = its id (its default action, then closed; no window)
        NotificationDismiss, //!< a notification dismissed: button = its id, 0: all of them (no window)
        NotificationAction, //!< a notification's action button: button = its id, index = which (no window)
    };
    Type type = Type::Motion;
    int windowId = 0;
    double x = 0.0;
    double y = 0.0;
    uint32_t button = 0; //!< linux/input-event-codes.h BTN_*
    bool pressed = false;
    double axisX = 0.0;
    double axisY = 0.0;
    uint32_t timeMs = 0;
    int index = -1; //!< NotificationAction: which button
};

/*!
 * The only state the Wayland and XR threads share. The Wayland thread publishes buffers (newest wins)
 * and the window layout; the XR thread takes them each frame and gives every buffer back when done with it, copied or
 * dropped. A buffer whose fence has not signalled yet is put back: the XR thread never waits for a client.
 */
class LayerBridge
{
public:
    //! @p release runs on whichever thread is done with a buffer; it must hand the token to the Wayland thread.
    explicit LayerBridge(std::function<void(uint64_t token)> release);
    ~LayerBridge();

    // Wayland thread.
    void publishBuffer(uint64_t surfaceId, std::unique_ptr<BufferHandoff> buffer);
    void publishScene(std::vector<WindowPlacement> windows);
    void surfaceDestroyed(uint64_t surfaceId);
    //! A client buffer handed over before (BufferHandoff::bufferId) is gone: its import goes.
    void bufferDestroyed(uint64_t bufferId);
    /*!
     * The XR side's import of new client buffers, asked for as they are published (BufferHandoff::newBuffer) rather than
     * at the next frame: set while the XR side runs (any thread), empty before it ends. Runs on the Wayland thread under
     * the bridge's lock, so once cleared it runs no more.
     */
    void setImportRequest(std::function<void(const BufferHandoff &)> request)
    {
        std::lock_guard lock(m_mutex);
        m_importRequest = std::move(request);
    }

    // XR thread.
    struct Frame {
        std::vector<WindowPlacement> windows;
        std::map<uint64_t, std::unique_ptr<BufferHandoff>> buffers;
        std::vector<uint64_t> destroyed;
        std::vector<uint64_t> buffersGone;
        bool dropAllImports = false; //!< more buffers went than were listed (the XR side did not take frames for long)
    };
    Frame take();
    //! Not ready yet (its fence): pending again, unless a newer buffer came meanwhile.
    void putBack(uint64_t surfaceId, std::unique_ptr<BufferHandoff> buffer);
    //! Done with it: its token goes back to the Wayland thread.
    void release(std::unique_ptr<BufferHandoff> buffer);
    //! Pointer input for the Wayland thread (set once, before the XR thread starts).
    void setInputHandler(std::function<void(const PointerEvent &)> handler)
    {
        m_input = std::move(handler);
    }
    void sendInput(const PointerEvent &event)
    {
        if (m_input) {
            m_input(event);
        }
    }
    //! The default output's volume for the dock (any thread): percent (-1: unknown) and mute.
    void setVolume(int percent, bool muted)
    {
        m_volume = percent;
        m_muted = muted;
    }
    int volume() const
    {
        return m_volume;
    }
    bool muted() const
    {
        return m_muted;
    }
    //! A drag and drop in progress (Wayland thread): the pointer follows the ray across windows until it ends.
    void setDragging(bool dragging)
    {
        m_dragging = dragging;
    }
    bool dragging() const
    {
        return m_dragging;
    }
    //! The network for the dock (any thread), as NetworkManager says.
    enum class Network {
        Unknown,
        Disconnected,
        Wired,
        Wifi,
    };
    void setNetwork(Network network, int signal)
    {
        m_network = int(network);
        m_signal = signal;
    }
    Network network() const
    {
        return Network(m_network.load());
    }
    //! A VPN is up (WireGuard or a tunnel device connected): a lock on the dock's network icon.
    void setVpn(bool up)
    {
        m_vpn = up;
    }
    bool vpn() const
    {
        return m_vpn;
    }
    int wifiSignal() const //!< 0..100 while on Wi-Fi
    {
        return m_signal;
    }

    //! Notifications (main thread), newest first; @p card: the one whose card shows now (0: none).
    void publishNotifications(std::vector<Notification> list, uint32_t card)
    {
        std::lock_guard lock(m_notificationMutex);
        m_notifications = std::move(list);
        m_card = card;
        ++m_notificationRevision;
    }
    //! Changes with every publishNotifications (XR thread: take them again when it moved).
    uint64_t notificationRevision() const
    {
        return m_notificationRevision;
    }
    std::vector<Notification> notifications(uint32_t &card) const
    {
        std::lock_guard lock(m_notificationMutex);
        card = m_card;
        return m_notifications;
    }

private:
    std::function<void(uint64_t)> m_release;
    std::function<void(const PointerEvent &)> m_input;
    std::atomic<int> m_volume{-1};
    std::atomic<bool> m_muted{false};
    std::atomic<int> m_network{0};
    std::atomic<int> m_signal{0};
    std::atomic<bool> m_dragging{false};
    std::atomic<bool> m_vpn{false};
    mutable std::mutex m_notificationMutex;
    std::vector<Notification> m_notifications;
    uint32_t m_card = 0;
    std::atomic<uint64_t> m_notificationRevision{0};
    std::mutex m_mutex;
    std::vector<WindowPlacement> m_windows;
    std::map<uint64_t, std::unique_ptr<BufferHandoff>> m_pending;
    std::vector<uint64_t> m_destroyed;
    //! Destroyed surfaces the XR side has not been told of yet: a buffer of theirs it still holds is not put back. Told
    //! at take(), after which it holds none (Firefox makes a video surface every few seconds: this must not grow).
    std::set<uint64_t> m_dead;
    std::function<void(const BufferHandoff &)> m_importRequest;
    std::vector<uint64_t> m_buffersGone; //!< at most kMaxBuffersGone, then m_dropAllImports instead
    bool m_dropAllImports = false;
    static constexpr size_t kMaxBuffersGone = 4096;
};

} // namespace RiftHome::Xr
