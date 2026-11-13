// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "color/ColorDescription.hpp"
#include "wayland/Listener.hpp"
#include "wayland/SurfaceProbe.hpp"

#include <QString>

#include <functional>

#include <memory>
#include <utility>
#include <vector>

struct wlr_scene_tree;
struct wlr_subsurface;
struct wlr_surface;
struct wlr_xwayland_surface;
struct wlr_xdg_popup;
struct wlr_xdg_toplevel;
struct wlr_xdg_toplevel_decoration_v1;

namespace RiftHome::Wayland
{

class Server;
class SurfaceNode;

//! Server-side decorations, once the toplevel can be configured.
void applyServerSideDecoration(wlr_xdg_toplevel_decoration_v1 *decoration);

/*!
 * An application window (xdg_toplevel). Deletes itself when the toplevel is destroyed. Logs its buffer, colour
 * description and the layer it will become whenever one of them changes.
 */
class Window
{
public:
    Window(Server &server, wlr_xdg_toplevel *toplevel, int id);
    //! An X11 window through XWayland: a window in the room like the others.
    Window(Server &server, wlr_xwayland_surface *xsurface, int id);
    ~Window();
    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;

    wlr_xdg_toplevel *toplevel() const //!< null for an X11 window
    {
        return m_toplevel;
    }
    wlr_xwayland_surface *xsurface() const //!< null for a Wayland window
    {
        return m_xsurface;
    }
    // What the server needs of a window, Wayland or X11 alike.
    //! Its main surface (null while an X11 window has none yet).
    wlr_surface *surface() const;
    //! The surface at a point (main surface coordinates): a popup or an X11 menu on top, else one of its own.
    wlr_surface *surfaceAt(double x, double y, double *sx, double *sy) const;
    struct Box {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
    };
    //! Its visible geometry in main surface coordinates (without client-side shadows).
    Box geometry() const;
    //! Bottom to top: its own surfaces (main, subsurfaces: @p own true), then popups and X11 menus, at main surface
    //! coordinates.
    void forEachSurface(const std::function<void(wlr_surface *, int, int, bool own)> &visit) const;
    void close();
    void setActivated(bool on);
    QString title() const;
    QString appId() const
    {
        return m_appId;
    }
    bool isFullscreen() const;
    bool isMaximized() const;
    //! An X11 menu or tooltip (override-redirect: placed by the app itself) shown with this window while mapped.
    void addX11Child(wlr_xwayland_surface *child);
    //! Shown on the monitor, not in the headset (a terminal from the monitor's page).
    bool onMonitor() const
    {
        return m_onMonitor;
    }
    void setOnMonitor(bool on)
    {
        m_onMonitor = on;
    }
    //! How often the app asked to be minimized (Rift Home hides it in the room).
    int minimizeRequests() const
    {
        return m_minimizeRequests;
    }
    wlr_scene_tree *sceneTree() const
    {
        return m_sceneTree;
    }
    //! Normal (the app's own size), maximized or fullscreen (both 1920x1080; fullscreen is a cinema screen in XR).
    enum class SizeMode {
        Normal,
        Maximized,
        Fullscreen,
    };
    void setSizeMode(SizeMode mode);
    //! A size of its own (the frame's resize handle): not maximized or fullscreen; within the app's limits.
    void resize(int width, int height);
    static constexpr int kLargeWidth = 1920;
    static constexpr int kLargeHeight = 1080;

    bool isMapped() const
    {
        return m_mapped;
    }
    int id() const
    {
        return m_id;
    }
    //! Place in front of the user while mapped (0 centre, then right, left, ...); -1 when unmapped.
    int slot() const
    {
        return m_slot;
    }
    void setSlot(int slot)
    {
        m_slot = slot;
    }
    //! "#id app_id", the prefix of every log line about this window.
    QString label() const;
    //! Commits since the last call: the main surface's and its subsurfaces'.
    std::pair<int, int> takeCommitCounts();
    //! Its subsurfaces now (nested ones too).
    int subsurfaceCount() const
    {
        return int(m_subsurfaces.size());
    }

    // Used by SurfaceNode.
    void addSubsurface(wlr_subsurface *subsurface);
    void removeSubsurface(SurfaceNode *node);
    void countSubsurfaceCommit()
    {
        ++m_subsurfaceCommits;
    }
    Server &server() const
    {
        return m_server;
    }

private:
    void onCommit();
    void attachX11Surface();
    struct X11Child;

    Server &m_server;
    wlr_xdg_toplevel *m_toplevel = nullptr;
    wlr_xwayland_surface *m_xsurface = nullptr;
    bool m_onMonitor = false;
    int m_normalWidth = 0; //!< an X11 window's own size, to go back to from maximized or fullscreen
    int m_normalHeight = 0;
    std::vector<std::unique_ptr<X11Child>> m_x11Children;
    wlr_scene_tree *m_sceneTree = nullptr;
    int m_id;
    QString m_appId; //!< kept: the toplevel's own is gone by the time it closes
    int m_minimizeRequests = 0;
    bool m_mapped = false;
    int m_slot = -1;
    int m_commits = 0;
    int m_subsurfaceCommits = 0;
    SurfaceWatch m_watch;
    std::vector<std::unique_ptr<SurfaceNode>> m_subsurfaces;
    int m_nextSubsurface = 1;

    Listener m_map;
    Listener m_unmap;
    Listener m_commit;
    Listener m_destroy;
    Listener m_requestMove;
    Listener m_requestResize;
    Listener m_requestMaximize;
    Listener m_requestFullscreen;
    Listener m_requestMinimize;
    Listener m_setTitle;
    Listener m_setAppId;
    Listener m_newSubsurface;
    Listener m_associate;
    Listener m_dissociate;
    Listener m_sceneTreeDestroy; //!< an X11 window's tree can go with its surface first (XWayland ending, a hidden window)
    Listener m_requestConfigure;
    Listener m_requestActivate;
    Listener m_setClass;
};

/*!
 * A subsurface of a window (nested ones too), with its own buffer and possibly its own colour description: browsers
 * draw page content and video into subsurfaces. Owned by its window.
 */
class SurfaceNode
{
public:
    SurfaceNode(Window &window, wlr_subsurface *subsurface, int number);
    ~SurfaceNode();
    SurfaceNode(const SurfaceNode &) = delete;
    SurfaceNode &operator=(const SurfaceNode &) = delete;

private:
    Window &m_window;
    int m_number;
    SurfaceWatch m_watch;
    Listener m_commit;
    Listener m_newSubsurface;
    Listener m_destroy;
};

//! A popup (menu, tooltip) drawn on its parent's scene tree. Deletes itself with the popup.
class Popup
{
public:
    static void create(wlr_xdg_popup *popup);

private:
    explicit Popup(wlr_xdg_popup *popup);

    wlr_xdg_popup *m_popup;
    Listener m_commit;
    Listener m_destroy;
};

} // namespace RiftHome::Wayland
