// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "wayland/Window.hpp"

#include <algorithm>

#include "wayland/Logging.hpp"
#include "wayland/Server.hpp"
#include "wayland/wlr.hpp"

#include <cmath>

namespace RiftHome::Wayland
{

void applyServerSideDecoration(wlr_xdg_toplevel_decoration_v1 *decoration)
{
    if (decoration->toplevel && decoration->toplevel->base->initialized) {
        wlr_xdg_toplevel_decoration_v1_set_mode(decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
}

Window::Window(Server &server, wlr_xdg_toplevel *toplevel, int id)
    : m_server(server)
    , m_toplevel(toplevel)
    , m_id(id)
{
    m_sceneTree = wlr_scene_xdg_surface_create(server.roomTree(), toplevel->base);
    m_sceneTree->node.data = this;
    toplevel->base->data = m_sceneTree; // popups find their parent's tree here

    wlr_surface *surface = toplevel->base->surface;
    m_map.connect(&surface->events.map, [this](void *) {
        m_mapped = true;
        qCInfo(lcSurface).noquote() << label() << "mapped:" << QString::fromUtf8(m_toplevel->title ? m_toplevel->title : "");
        m_server.windowMapped(this);
    });
    m_unmap.connect(&surface->events.unmap, [this](void *) {
        m_mapped = false;
        qCInfo(lcSurface).noquote() << label() << "unmapped";
        m_server.windowUnmapped(this);
    });
    m_commit.connect(&surface->events.commit, [this](void *) {
        onCommit();
    });
    m_destroy.connect(&toplevel->events.destroy, [this](void *) {
        qCInfo(lcSurface).noquote() << label() << "closed";
        m_server.windowDestroyed(this);
        delete this;
    });
    // No move/resize grabs from the client (the controllers do that); said in the log.
    m_requestMove.connect(&toplevel->events.request_move, [this](void *) {
        qCDebug(lcSurface).noquote() << label() << "asks to be moved (the controllers move windows)";
    });
    m_requestResize.connect(&toplevel->events.request_resize, [this](void *) {
        qCDebug(lcSurface).noquote() << label() << "asks to be resized (the controllers resize windows)";
    });
    // Maximize and fullscreen need a configure in reply, even when nothing changes: both granted.
    m_requestMaximize.connect(&toplevel->events.request_maximize, [this](void *) {
        const bool on = m_toplevel->requested.maximized;
        qCInfo(lcSurface).noquote() << label() << (on ? "asks to be maximized" : "asks to be unmaximized");
        setSizeMode(on ? SizeMode::Maximized : SizeMode::Normal);
    });
    m_requestFullscreen.connect(&toplevel->events.request_fullscreen, [this](void *) {
        // A 1920x1080 picture, which the XR side shows as a cinema screen.
        const bool on = m_toplevel->requested.fullscreen;
        qCInfo(lcSurface).noquote() << label() << (on ? "goes fullscreen" : "leaves fullscreen");
        setSizeMode(on ? SizeMode::Fullscreen : (m_toplevel->current.maximized ? SizeMode::Maximized : SizeMode::Normal));
    });
    m_requestMinimize.connect(&toplevel->events.request_minimize, [this](void *) {
        ++m_minimizeRequests; // the XR side hides it (Home's running apps bring it back)
        qCInfo(lcSurface).noquote() << label() << "asks to be minimized";
        // Told now: an app that has asked draws nothing new, so no frame would tell it (Chromium's own
        // minimize button did nothing otherwise).
        m_server.publishScene();
    });
    m_newSubsurface.connect(&surface->events.new_subsurface, [this](void *data) {
        addSubsurface(static_cast<wlr_subsurface *>(data));
    });
    m_setAppId.connect(&toplevel->events.set_app_id, [this](void *) {
        if (m_toplevel->app_id && *m_toplevel->app_id) {
            m_appId = QString::fromUtf8(m_toplevel->app_id);
        }
    });
    m_setTitle.connect(&toplevel->events.set_title, [this](void *) {
        qCDebug(lcSurface).noquote() << label() << "title:" << QString::fromUtf8(m_toplevel->title ? m_toplevel->title : "");
    });
}

//! An X11 menu or tooltip shown with its window: its own scene node on the window's tree, until it unmaps. The node
//! may go first (with the window's tree, or its surface): followed, never destroyed twice (a menu unmapping
//! after its window's surface had gone crashed Rift Home).
struct Window::X11Child {
    wlr_xwayland_surface *xsurface = nullptr;
    wlr_scene_tree *tree = nullptr;
    Listener unmap;
    Listener destroy;
    Listener treeDestroy;
    ~X11Child()
    {
        treeDestroy.disconnect();
        if (tree) {
            wlr_scene_node_destroy(&tree->node);
        }
    }
};

Window::Window(Server &server, wlr_xwayland_surface *xsurface, int id)
    : m_server(server)
    , m_xsurface(xsurface)
    , m_id(id)
{
    if (xsurface->class_ && *xsurface->class_) {
        m_appId = QString::fromUtf8(xsurface->class_);
    }
    m_destroy.connect(&xsurface->events.destroy, [this](void *) {
        qCInfo(lcSurface).noquote() << label() << "closed (X11)";
        m_x11Children.clear();
        m_server.windowDestroyed(this);
        delete this;
    });
    m_associate.connect(&xsurface->events.associate, [this](void *) {
        attachX11Surface();
    });
    m_dissociate.connect(&xsurface->events.dissociate, [this](void *) {
        m_map.disconnect();
        m_unmap.disconnect();
        m_commit.disconnect();
        m_x11Children.clear(); // their nodes are on this window's tree: before it goes
        if (m_mapped) {
            m_mapped = false;
            m_server.windowUnmapped(this);
        }
        if (m_sceneTree) {
            m_sceneTreeDestroy.disconnect();
            wlr_scene_node_destroy(&m_sceneTree->node);
            m_sceneTree = nullptr;
        }
    });
    // Its own size, at 0,0: X11 windows are placed in the room, not on an X screen (their menus come at their own
    // position relative to it).
    m_requestConfigure.connect(&xsurface->events.request_configure, [this](void *data) {
        const auto *event = static_cast<wlr_xwayland_surface_configure_event *>(data);
        // Kept to the X screen: what lies outside it never gets the pointer (Server::kX11ScreenWidth). Fullscreen and
        // maximized windows keep the X screen's size, as on a desktop.
        if (m_xsurface->fullscreen || (m_xsurface->maximized_horz && m_xsurface->maximized_vert)) {
            wlr_xwayland_surface_configure(m_xsurface, 0, 0, Server::kX11ScreenWidth, Server::kX11ScreenHeight);
            return;
        }
        const uint16_t width = std::min<uint16_t>(event->width, Server::kX11ScreenWidth);
        const uint16_t height = std::min<uint16_t>(event->height, Server::kX11ScreenHeight);
        m_normalWidth = width;
        m_normalHeight = height;
        wlr_xwayland_surface_configure(m_xsurface, 0, 0, width, height);
    });
    m_requestFullscreen.connect(&xsurface->events.request_fullscreen, [this](void *) {
        const bool on = m_xsurface->fullscreen;
        qCInfo(lcSurface).noquote() << label() << (on ? "goes fullscreen" : "leaves fullscreen");
        setSizeMode(on ? SizeMode::Fullscreen : SizeMode::Normal);
    });
    m_requestMaximize.connect(&xsurface->events.request_maximize, [this](void *) {
        const bool on = m_xsurface->maximized_horz || m_xsurface->maximized_vert;
        qCInfo(lcSurface).noquote() << label() << (on ? "asks to be maximized" : "asks to be unmaximized");
        setSizeMode(on ? SizeMode::Maximized : SizeMode::Normal);
    });
    m_requestMinimize.connect(&xsurface->events.request_minimize, [this](void *data) {
        if (static_cast<wlr_xwayland_minimize_event *>(data)->minimize) {
            ++m_minimizeRequests; // the XR side hides it (Home's running apps bring it back)
            qCInfo(lcSurface).noquote() << label() << "asks to be minimized";
            m_server.publishScene(); // told now (see the Wayland window's)
        }
    });
    m_requestActivate.connect(&xsurface->events.request_activate, [this](void *) {
        m_server.focusWindow(this);
    });
    m_setClass.connect(&xsurface->events.set_class, [this](void *) {
        if (m_xsurface->class_ && *m_xsurface->class_) {
            m_appId = QString::fromUtf8(m_xsurface->class_);
        }
    });
    m_setTitle.connect(&xsurface->events.set_title, [this](void *) {
        qCDebug(lcSurface).noquote() << label() << "title:" << title();
    });
    if (xsurface->surface) {
        attachX11Surface();
    }
}

void Window::attachX11Surface()
{
    wlr_surface *surface = m_xsurface->surface;
    m_sceneTree = wlr_scene_subsurface_tree_create(m_server.roomTree(), surface);
    m_sceneTree->node.data = this;
    // wlroots destroys this tree with the surface, which can come before dissociate (XWayland's clients going at its
    // end; that crashed Rift Home): followed, never destroyed twice.
    m_sceneTreeDestroy.connect(&m_sceneTree->node.events.destroy, [this](void *) {
        m_sceneTree = nullptr;
        m_sceneTreeDestroy.disconnect();
    });
    m_map.connect(&surface->events.map, [this](void *) {
        m_mapped = true;
        if (m_appId.isEmpty()) {
            // No WM_CLASS (glxgears has none): its instance name, else its title, names it (places are kept by it).
            const char *name = m_xsurface->instance && *m_xsurface->instance ? m_xsurface->instance : m_xsurface->title;
            m_appId = QString::fromUtf8(name ? name : "x11");
        }
        // At the X screen's corner and within it (it may map without asking to be configured): all of it under the pointer.
        // Mapped fullscreen or maximized (games set it before mapping): the X screen, as a window manager gives it.
        const bool large = m_xsurface->fullscreen || (m_xsurface->maximized_horz && m_xsurface->maximized_vert);
        const int width = large ? Server::kX11ScreenWidth : std::min<int>(m_xsurface->width, Server::kX11ScreenWidth);
        const int height = large ? Server::kX11ScreenHeight : std::min<int>(m_xsurface->height, Server::kX11ScreenHeight);
        if (m_xsurface->x != 0 || m_xsurface->y != 0 || width != m_xsurface->width || height != m_xsurface->height) {
            wlr_xwayland_surface_configure(m_xsurface, 0, 0, uint16_t(width), uint16_t(height));
        }
        m_normalWidth = m_normalWidth > 0 ? m_normalWidth : m_xsurface->width;
        m_normalHeight = m_normalHeight > 0 ? m_normalHeight : m_xsurface->height;
        qCInfo(lcSurface).noquote() << label() << "mapped (X11):" << title()
                                    << QStringLiteral("| %1x%2%3%4").arg(m_xsurface->width).arg(m_xsurface->height)
                                           .arg(m_xsurface->fullscreen ? QStringLiteral(" fullscreen") : QString())
                                           .arg(m_xsurface->maximized_horz && m_xsurface->maximized_vert ? QStringLiteral(" maximized") : QString());
        m_server.windowMapped(this);
    });
    m_unmap.connect(&surface->events.unmap, [this](void *) {
        m_mapped = false;
        qCInfo(lcSurface).noquote() << label() << "unmapped";
        m_server.windowUnmapped(this);
    });
    m_commit.connect(&surface->events.commit, [this](void *) {
        onCommit();
    });
}

Window::~Window() = default;

wlr_surface *Window::surface() const
{
    return m_toplevel ? m_toplevel->base->surface : m_xsurface->surface;
}

wlr_surface *Window::surfaceAt(double x, double y, double *sx, double *sy) const
{
    if (m_toplevel) {
        return wlr_xdg_surface_surface_at(m_toplevel->base, x, y, sx, sy);
    }
    // An X11 window's menus on top (the newest last), then the window itself.
    for (auto it = m_x11Children.rbegin(); it != m_x11Children.rend(); ++it) {
        wlr_xwayland_surface *child = (*it)->xsurface;
        if (!child->surface) {
            continue;
        }
        const double cx = x - (child->x - m_xsurface->x);
        const double cy = y - (child->y - m_xsurface->y);
        if (wlr_surface *hit = wlr_surface_surface_at(child->surface, cx, cy, sx, sy)) {
            return hit;
        }
    }
    return m_xsurface->surface ? wlr_surface_surface_at(m_xsurface->surface, x, y, sx, sy) : nullptr;
}

Window::Box Window::geometry() const
{
    if (m_toplevel) {
        // wlroots' effective geometry: the client's window geometry, or the surface tree's extent when none was set.
        const wlr_box g = m_toplevel->base->geometry;
        if (g.width > 0 && g.height > 0) {
            return {g.x, g.y, g.width, g.height};
        }
        const wlr_surface *main = m_toplevel->base->surface;
        return {0, 0, main->current.width, main->current.height};
    }
    if (m_xsurface->surface && m_xsurface->surface->current.width > 0) {
        return {0, 0, m_xsurface->surface->current.width, m_xsurface->surface->current.height};
    }
    return {0, 0, m_xsurface->width, m_xsurface->height};
}

void Window::forEachSurface(const std::function<void(wlr_surface *, int, int, bool)> &visit) const
{
    struct Context {
        const std::function<void(wlr_surface *, int, int, bool)> *visit;
        bool own;
        int dx;
        int dy;
    };
    const auto call = [](wlr_surface *surface, int sx, int sy, void *data) {
        auto *c = static_cast<Context *>(data);
        (*c->visit)(surface, sx + c->dx, sy + c->dy, c->own);
    };
    Context context{&visit, true, 0, 0};
    wlr_surface *main = surface();
    if (!main) {
        return;
    }
    wlr_surface_for_each_surface(main, call, &context);
    context.own = false;
    if (m_toplevel) {
        wlr_xdg_surface_for_each_popup_surface(m_toplevel->base, call, &context);
        return;
    }
    for (const auto &child : m_x11Children) {
        if (child->xsurface->surface && child->xsurface->surface->mapped) {
            context.dx = child->xsurface->x - m_xsurface->x;
            context.dy = child->xsurface->y - m_xsurface->y;
            wlr_surface_for_each_surface(child->xsurface->surface, call, &context);
        }
    }
}

void Window::close()
{
    if (m_toplevel) {
        wlr_xdg_toplevel_send_close(m_toplevel);
    } else {
        wlr_xwayland_surface_close(m_xsurface);
    }
}

void Window::setActivated(bool on)
{
    if (m_toplevel) {
        wlr_xdg_toplevel_set_activated(m_toplevel, on);
        return;
    }
    wlr_xwayland_surface_activate(m_xsurface, on);
    if (on) {
        wlr_xwayland_surface_restack(m_xsurface, nullptr, XCB_STACK_MODE_ABOVE);
    }
}

QString Window::title() const
{
    const char *t = m_toplevel ? m_toplevel->title : m_xsurface->title;
    return QString::fromUtf8(t ? t : "");
}

bool Window::isFullscreen() const
{
    return m_toplevel ? m_toplevel->current.fullscreen : m_xsurface->fullscreen;
}

bool Window::isMaximized() const
{
    return m_toplevel ? m_toplevel->current.maximized : (m_xsurface->maximized_horz && m_xsurface->maximized_vert);
}

void Window::addX11Child(wlr_xwayland_surface *child)
{
    if (!child->surface || !m_sceneTree) {
        return;
    }
    auto entry = std::make_unique<X11Child>();
    X11Child *c = entry.get();
    c->xsurface = child;
    c->tree = wlr_scene_subsurface_tree_create(m_sceneTree, child->surface);
    if (!c->tree) {
        return;
    }
    c->treeDestroy.connect(&c->tree->node.events.destroy, [c](void *) {
        c->tree = nullptr;
        c->treeDestroy.disconnect();
    });
    wlr_scene_node_set_position(&c->tree->node, child->x - m_xsurface->x, child->y - m_xsurface->y);
    const auto remove = [this, c](void *) {
        std::erase_if(m_x11Children, [c](const std::unique_ptr<X11Child> &e) {
            return e.get() == c;
        });
    };
    // Gone when it unmaps or loses its surface (dissociate comes before the surface is destroyed: the node goes first).
    c->unmap.connect(&child->surface->events.unmap, remove);
    c->destroy.connect(&child->events.dissociate, remove);
    m_x11Children.push_back(std::move(entry));
    qCInfo(lcSurface).noquote() << label() << "X11 menu shown at" << (child->x - m_xsurface->x) << (child->y - m_xsurface->y);
}

void Window::setSizeMode(SizeMode mode)
{
    if (m_xsurface) {
        wlr_xwayland_surface_set_fullscreen(m_xsurface, mode == SizeMode::Fullscreen);
        wlr_xwayland_surface_set_maximized(m_xsurface, mode == SizeMode::Maximized, mode == SizeMode::Maximized);
        if (mode == SizeMode::Normal) {
            if (m_normalWidth > 0 && m_normalHeight > 0) {
                wlr_xwayland_surface_configure(m_xsurface, 0, 0, uint16_t(m_normalWidth), uint16_t(m_normalHeight));
            }
        } else {
            wlr_xwayland_surface_configure(m_xsurface, 0, 0, Server::kX11ScreenWidth, Server::kX11ScreenHeight); // the X screen, as games expect
        }
        return;
    }
    if (!m_toplevel->base->initialized) {
        return;
    }
    wlr_xdg_toplevel_set_fullscreen(m_toplevel, mode == SizeMode::Fullscreen);
    wlr_xdg_toplevel_set_maximized(m_toplevel, mode == SizeMode::Maximized);
    if (mode == SizeMode::Normal) {
        wlr_xdg_toplevel_set_size(m_toplevel, 0, 0); // the app goes back to its own size
    } else {
        // The same pixels at any apps' scale (a 1920x1080 picture): fewer of the app's own units, its text bigger.
        const double scale = m_server.pixelScale(this);
        wlr_xdg_toplevel_set_size(m_toplevel, int(std::lround(kLargeWidth / scale)), int(std::lround(kLargeHeight / scale)));
    }
}

void Window::resize(int width, int height)
{
    if (m_xsurface) {
        const int w = std::clamp(width, 320, Server::kX11ScreenWidth);
        const int h = std::clamp(height, 200, Server::kX11ScreenHeight);
        m_normalWidth = w;
        m_normalHeight = h;
        wlr_xwayland_surface_set_fullscreen(m_xsurface, false);
        wlr_xwayland_surface_set_maximized(m_xsurface, false, false);
        wlr_xwayland_surface_configure(m_xsurface, 0, 0, uint16_t(w), uint16_t(h));
        return;
    }
    if (!m_toplevel->base->initialized) {
        return;
    }
    // The app's own limits (0: none), and a sensible range around them.
    const auto limit = [](int value, int low, int high) {
        return std::clamp(value, low, high);
    };
    const int minW = std::max(m_toplevel->current.min_width, 320);
    const int minH = std::max(m_toplevel->current.min_height, 200);
    const int maxW = m_toplevel->current.max_width > 0 ? m_toplevel->current.max_width : 3840;
    const int maxH = m_toplevel->current.max_height > 0 ? m_toplevel->current.max_height : 2160;
    wlr_xdg_toplevel_set_fullscreen(m_toplevel, false);
    wlr_xdg_toplevel_set_maximized(m_toplevel, false);
    wlr_xdg_toplevel_set_size(m_toplevel, limit(width, minW, std::max(minW, maxW)), limit(height, minH, std::max(minH, maxH)));
}

QString Window::label() const
{
    return QStringLiteral("#%1 %2").arg(m_id).arg(m_appId.isEmpty() ? QStringLiteral("(no app id)") : m_appId);
}

std::pair<int, int> Window::takeCommitCounts()
{
    const std::pair counts{m_commits, m_subsurfaceCommits};
    m_commits = 0;
    m_subsurfaceCommits = 0;
    return counts;
}

void Window::addSubsurface(wlr_subsurface *subsurface)
{
    m_subsurfaces.push_back(std::make_unique<SurfaceNode>(*this, subsurface, m_nextSubsurface++));
}

void Window::removeSubsurface(SurfaceNode *node)
{
    std::erase_if(m_subsurfaces, [node](const std::unique_ptr<SurfaceNode> &n) {
        return n.get() == node;
    });
}

SurfaceNode::SurfaceNode(Window &window, wlr_subsurface *subsurface, int number)
    : m_window(window)
    , m_number(number)
{
    wlr_surface *surface = subsurface->surface;
    m_commit.connect(&surface->events.commit, [this, surface](void *) {
        m_window.countSubsurfaceCommit();
        if (const auto line = m_watch.update(surface, m_window.server().explicitSync)) {
            qCInfo(lcSurface).noquote() << m_window.label() << QStringLiteral("subsurface %1").arg(m_number) << *line;
        }
    });
    m_newSubsurface.connect(&surface->events.new_subsurface, [this](void *data) {
        m_window.addSubsurface(static_cast<wlr_subsurface *>(data));
    });
    // Deletes this node: nothing may touch it after removeSubsurface().
    m_destroy.connect(&subsurface->events.destroy, [this](void *) {
        m_window.removeSubsurface(this);
    });
}

SurfaceNode::~SurfaceNode() = default;

void Window::onCommit()
{
    if (m_xsurface) {
        ++m_commits;
        if (const auto line = m_watch.update(m_xsurface->surface, m_server.explicitSync)) {
            qCInfo(lcSurface).noquote() << label() << *line;
        }
        return;
    }
    if (m_toplevel->base->initial_commit) {
        // The client picks its own size (0x0) for now.
        wlr_xdg_toplevel_set_size(m_toplevel, 0, 0);
        if (wlr_xdg_toplevel_decoration_v1 *decoration = m_server.decorationFor(m_toplevel)) {
            applyServerSideDecoration(decoration);
        }
        return;
    }
    ++m_commits;
    if (const auto line = m_watch.update(m_toplevel->base->surface, m_server.explicitSync)) {
        qCInfo(lcSurface).noquote() << label() << *line;
    }
}

void Popup::create(wlr_xdg_popup *popup)
{
    wlr_xdg_surface *parent = popup->parent ? wlr_xdg_surface_try_from_wlr_surface(popup->parent) : nullptr;
    if (!parent || !parent->data) {
        qCWarning(lcSurface) << "popup without a known parent: not shown";
        return;
    }
    popup->base->data = wlr_scene_xdg_surface_create(static_cast<wlr_scene_tree *>(parent->data), popup->base);
    qCInfo(lcSurface) << "popup opened (a menu, tooltip or dropdown)";
    new Popup(popup);
}

Popup::Popup(wlr_xdg_popup *popup)
    : m_popup(popup)
{
    m_commit.connect(&popup->base->surface->events.commit, [this](void *) {
        if (m_popup->base->initial_commit) {
            wlr_xdg_surface_schedule_configure(m_popup->base);
        }
    });
    m_destroy.connect(&popup->events.destroy, [this](void *) {
        delete this;
    });
}

} // namespace RiftHome::Wayland
