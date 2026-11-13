// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "wayland/Server.hpp"

#include "wayland/ColorManagement.hpp"

#include "color/ColorDescription.hpp"
#include "wayland/Input.hpp"
#include "wayland/LinuxDmabuf.hpp"
#include "wayland/Logging.hpp"
#include "wayland/Output.hpp"
#include "wayland/SurfaceExport.hpp"
#include "wayland/Window.hpp"
#include "wayland/wlr.hpp"
#include "xr/LayerBridge.hpp"

#include <QAbstractEventDispatcher>
#include <QDir>
#include <QFile>
#include <QSocketNotifier>
#include <QStringList>
#include <QTime>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <drm_fourcc.h>
#include <iterator>
#include <malloc.h>

Q_LOGGING_CATEGORY(lcWayland, "rifthome.wayland")
Q_LOGGING_CATEGORY(lcSurface, "rifthome.surface")

namespace RiftHome::Wayland
{

struct Server::TextInput {
    wlr_text_input_v3 *input = nullptr;
    Listener enable;
    Listener disable;
    Listener destroy;
};

namespace
{

//! A toplevel's xdg-decoration: server side is asked for (Rift Home draws its own window bars).
class Decoration
{
public:
    Decoration(QHash<wlr_xdg_toplevel *, wlr_xdg_toplevel_decoration_v1 *> &map, wlr_xdg_toplevel_decoration_v1 *decoration)
        : m_map(map)
        , m_decoration(decoration)
        , m_toplevel(decoration->toplevel)
    {
        m_map.insert(m_toplevel, decoration);
        m_requestMode.connect(&decoration->events.request_mode, [this](void *) {
            // Which frame the app asks for (Chromium draws its own unless "Use system title bar and borders": Chromium).
            const auto mode = m_decoration->requested_mode;
            qCInfo(lcWayland).noquote() << (m_toplevel->app_id ? m_toplevel->app_id : "(no app id)") << "asks for"
                                        << (mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE   ? "its own frame (client side)"
                                                : mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE ? "Rift Home's frame (server side)"
                                                                                                          : "no preference");
            applyServerSideDecoration(m_decoration);
        });
        m_destroy.connect(&decoration->events.destroy, [this](void *) {
            m_map.remove(m_toplevel);
            delete this;
        });
        applyServerSideDecoration(decoration);
    }

private:
    QHash<wlr_xdg_toplevel *, wlr_xdg_toplevel_decoration_v1 *> &m_map;
    wlr_xdg_toplevel_decoration_v1 *m_decoration;
    wlr_xdg_toplevel *m_toplevel;
    Listener m_requestMode;
    Listener m_destroy;
};

int onSignal(int signalNumber, void *data)
{
    qCInfo(lcWayland) << "signal" << signalNumber << "- stopping";
    Q_EMIT static_cast<Server *>(data)->quitRequested();
    return 0;
}

} // namespace

Server::Server(const Options &options, QObject *parent)
    : QObject(parent)
    , m_options(options)
{
}

Server::~Server()
{
    if (!display) {
        return;
    }
    delete m_statsTimer;
    delete m_notifier;
    m_bridge = nullptr;
    for (wlr_buffer *buffer : std::as_const(m_lockedBuffers)) {
        wlr_buffer_unlock(buffer);
    }
    m_lockedBuffers.clear();
    m_bufferIds.clear(); // their destroy listeners off: buffers still alive go with their clients below

    // XWayland first (its windows go with it), then windows, popups and decorations delete themselves as their clients go.
    m_newX11Surface.disconnect();
    m_x11Ready.disconnect();
    m_x11OutputDestroy.disconnect();
    if (m_xwayland) {
        wlr_xwayland_destroy(m_xwayland);
        m_xwayland = nullptr;
    }
    wl_display_destroy_clients(display);
    qDeleteAll(m_windows);
    m_windows.clear();

    // Our listeners leave the wlroots objects' signals before those objects are freed.
    for (Listener *listener : {&m_newOutput, &m_newInput, &m_newSurface, &m_newToplevel, &m_newPopup, &m_newDecoration,
                               &m_requestSetCursor, &m_requestSetSelection, &m_requestSetPrimarySelection,
                               &m_requestStartDrag, &m_requestSetShape, &m_newConstraint, &m_constraintDestroy}) {
        listener->disconnect();
    }
    m_textInputs.clear();
    m_newTextInput.disconnect();
    m_input.reset();

    // Each only if made: a start that failed half-way (no session for the monitors) stops here too.
    if (scene) {
        wlr_scene_node_destroy(&scene->tree.node);
    }
    if (cursorManager) {
        wlr_xcursor_manager_destroy(cursorManager);
    }
    if (cursor) {
        wlr_cursor_destroy(cursor);
        cursor = nullptr; // the outputs going with the backend below confine no cursor
    }
    if (allocator) {
        wlr_allocator_destroy(allocator);
    }
    if (renderer) {
        wlr_renderer_destroy(renderer);
    }
    m_leaseRequest.disconnect();
    m_leaseEnded.disconnect();
    if (backend) {
        wlr_backend_destroy(backend); // outputs delete themselves here
    }
    if (m_session) {
        wlr_session_destroy(m_session);
    }
    wl_display_destroy(display);
}

namespace
{

/*!
 * wlroots' messages, a message that comes every frame written once a minute with how often it came (a buffer its
 * renderer is not given, HDR video, which the XR side shows: two errors a video frame; EGL's at teardown). wlroots
 * logs from the Wayland thread only.
 */
void logWlroots(wlr_log_importance importance, const char *fmt, va_list args)
{
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point start = Clock::now();
    static std::unordered_map<const char *, std::pair<Clock::time_point, int>> seen; // by call site
    const Clock::time_point now = Clock::now();
    auto &[last, skipped] = seen[fmt];
    if (last != Clock::time_point() && now - last < std::chrono::minutes(1)) {
        ++skipped;
        return;
    }
    char text[2048];
    std::vsnprintf(text, sizeof(text), fmt, args);
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
    static const char *const kLevels[] = {"", "ERROR", "INFO", "DEBUG"};
    std::fprintf(stderr, "%02lld:%02lld:%02lld.%03lld [%s] %s%s\n", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000,
                 kLevels[std::clamp(int(importance), 0, 3)], text,
                 skipped > 0 ? qPrintable(QStringLiteral(" (and %1 more in the minute before)").arg(skipped)) : "");
    last = now;
    skipped = 0;
}

} // namespace

bool Server::start()
{
    wlr_log_init(m_options.verbose ? WLR_DEBUG : WLR_ERROR, m_options.verbose ? nullptr : logWlroots);

    display = wl_display_create();
    m_loop = wl_display_get_event_loop(display);
    wl_display_set_global_filter(
        display,
        [](const wl_client *client, const wl_global *global, void *data) {
            return static_cast<const Server *>(data)->clientSees(client, global);
        },
        this);

    if (m_options.backend == Backend::Drm) {
        // Its own session: the monitors and input through the login session (logind), never a desktop's
        // window, so no desktop display may be seen.
        qunsetenv("WAYLAND_DISPLAY");
        qunsetenv("WAYLAND_SOCKET");
        qunsetenv("DISPLAY");
        backend = wlr_backend_autocreate(m_loop, &m_session);
    } else {
        backend = m_options.backend == Backend::Headless ? wlr_headless_backend_create(m_loop) : wlr_backend_autocreate(m_loop, nullptr);
    }
    if (!backend) {
        qCCritical(lcWayland) << (m_options.backend == Backend::Drm ? "no session for the monitors and input (start it from a console login or the login screen)"
                                                                     : "no wlroots backend (nested needs WAYLAND_DISPLAY of the desktop)");
        return false;
    }
    renderer = wlr_renderer_autocreate(backend);
    if (!renderer) {
        qCCritical(lcWayland) << "no wlroots renderer";
        return false;
    }
    wlr_renderer_init_wl_shm(renderer, display);
    if (wlr_renderer_get_texture_formats(renderer, WLR_BUFFER_CAP_DMABUF)) {
        if (!LinuxDmabuf::create(display, renderer)) { // Rift Home's own (P010, a modifier for each plane)
            wlr_linux_dmabuf_v1_create_with_renderer(display, 5, renderer);
        }
    } else {
        qCWarning(lcWayland) << "renderer takes no dmabufs: shared memory buffers only";
    }
    allocator = wlr_allocator_autocreate(backend, renderer);
    if (!allocator) {
        qCCritical(lcWayland) << "no wlroots allocator";
        return false;
    }

    wlr_compositor *compositor = wlr_compositor_create(display, 6, renderer);
    wlr_subcompositor_create(display);
    wlr_data_device_manager_create(display);
    wlr_primary_selection_v1_device_manager_create(display);
    // Text fields tell the compositor when they are focused (text-input-v3): the virtual keyboard can show then.
    wlr_text_input_manager_v3 *textInputs = wlr_text_input_manager_v3_create(display);
    m_newTextInput.connect(&textInputs->events.new_text_input, [this](void *data) {
        auto *ti = static_cast<wlr_text_input_v3 *>(data);
        auto entry = std::make_unique<TextInput>();
        TextInput *t = entry.get();
        t->input = ti;
        const auto changed = [this, t](bool on) {
            if (Window *window = windowOf(t->input->focused_surface)) {
                qCInfo(lcSurface).noquote() << window->label() << (on ? "text field focused (wants the keyboard)" : "text field left");
            }
        };
        t->enable.connect(&ti->events.enable, [changed](void *) {
            changed(true);
        });
        t->disable.connect(&ti->events.disable, [changed](void *) {
            changed(false);
        });
        t->destroy.connect(&ti->events.destroy, [this, t](void *) {
            std::erase_if(m_textInputs, [t](const std::unique_ptr<TextInput> &e) {
                return e.get() == t;
            });
        });
        m_textInputs.push_back(std::move(entry));
        updateTextInputFocus();
    });
    wlr_viewporter_create(display);
    wlr_fractional_scale_manager_v1_create(display, 1);
    wlr_presentation_create(display, backend, 2);
    wlr_single_pixel_buffer_manager_v1_create(display);

    // NVIDIA has no implicit sync: clients need linux-drm-syncobj.
    const int drmFd = wlr_renderer_get_drm_fd(renderer);
    if (drmFd >= 0 && renderer->features.timeline) {
        wlr_linux_drm_syncobj_manager_v1_create(display, 1, drmFd);
        explicitSync = true;
    }
    qCInfo(lcWayland).nospace() << "explicit sync (linux-drm-syncobj-v1): " << (explicitSync ? "on" : "OFF")
                                << " (renderer timeline " << renderer->features.timeline << ", backend timeline "
                                << backend->features.timeline << ")";

    createColorManager();
    m_newSurface.connect(&compositor->events.new_surface, [this](void *data) {
        auto *surface = static_cast<wlr_surface *>(data);
        if (m_bridge) {
            new SurfaceExport(*this, surface, m_nextSurfaceId++); // deletes itself with the surface
        }
    });

    outputLayout = wlr_output_layout_create(display);
    wlr_xdg_output_manager_v1_create(display, outputLayout);
    scene = wlr_scene_create();
    sceneLayout = wlr_scene_attach_output_layout(scene, outputLayout);
    // The headset's windows below, the monitor's page and the windows started from it above.
    m_roomTree = wlr_scene_tree_create(&scene->tree);
    m_monitorTree = wlr_scene_tree_create(&scene->tree);
    if (m_options.backend == Backend::Drm) {
        // The headset (a non-desktop output) is lent to the runtime that asks (RiftLinux's, in Wayland direct mode).
        m_leaseManager = wlr_drm_lease_v1_manager_create(display, backend);
        if (m_leaseManager) {
            m_leaseRequest.connect(&m_leaseManager->events.request, [this](void *data) {
                auto *request = static_cast<wlr_drm_lease_request_v1 *>(data);
                wlr_drm_lease_v1 *lease = wlr_drm_lease_request_v1_grant(request);
                if (!lease) {
                    wlr_drm_lease_request_v1_reject(request);
                    qCWarning(lcWayland) << "headset: a lease request could not be granted";
                    setHeadsetState(QStringLiteral("Lease refused"), false);
                    return;
                }
                qCInfo(lcWayland) << "headset: lent to the runtime";
                setHeadsetState(QStringLiteral("Lent to the runtime"), true);
                m_leaseEnded.connect(&lease->drm_lease->events.destroy, [this](void *) {
                    m_leaseEnded.disconnect();
                    qCInfo(lcWayland) << "headset: lease ended (offered again when it reconnects)";
                    setHeadsetState(QStringLiteral("Back from the runtime"), false);
                });
            });
        } else {
            qCWarning(lcWayland) << "headset: no lease manager (the runtime cannot get the headset)";
        }
    }

    m_xdgShell = wlr_xdg_shell_create(display, 5);
    m_newToplevel.connect(&m_xdgShell->events.new_toplevel, [this](void *data) {
        m_windows.append(new Window(*this, static_cast<wlr_xdg_toplevel *>(data), m_nextWindowId++));
    });
    m_newPopup.connect(&m_xdgShell->events.new_popup, [](void *data) {
        Popup::create(static_cast<wlr_xdg_popup *>(data));
    });
    // X11 apps through XWayland, started when the first one connects: windows in the room as the
    // others; their menus and tooltips (override-redirect: placed by the app) shown with the window they belong to.
    m_xwayland = wlr_xwayland_create(display, compositor, true);
    if (m_xwayland) {
        wlr_xwayland_set_seat(m_xwayland, seat);
        m_x11Ready.connect(&m_xwayland->events.ready, [this](void *) {
            Q_EMIT x11Ready();
        });
        m_newX11Surface.connect(&m_xwayland->events.new_surface, [this](void *data) {
            auto *xsurface = static_cast<wlr_xwayland_surface *>(data);
            if (xsurface->override_redirect) {
                trackX11Menu(xsurface);
            } else {
                m_windows.append(new Window(*this, xsurface, m_nextWindowId++));
            }
        });
        qCInfo(lcWayland).noquote() << "XWayland on" << m_xwayland->display_name << "(starts when an X11 app connects)";
    } else {
        qCWarning(lcWayland) << "no XWayland: X11 apps cannot run";
    }
    wlr_xdg_decoration_manager_v1 *decorations = wlr_xdg_decoration_manager_v1_create(display);
    m_newDecoration.connect(&decorations->events.new_toplevel_decoration, [this](void *data) {
        new Decoration(m_decorations, static_cast<wlr_xdg_toplevel_decoration_v1 *>(data));
    });

    seat = wlr_seat_create(display, "seat0");
    // Games' mouse look: pointer lock / confinement and relative motion; XWayland uses them for X11
    // games' grabs too.
    m_relativePointer = wlr_relative_pointer_manager_v1_create(display);
    m_constraints = wlr_pointer_constraints_v1_create(display);
    m_newConstraint.connect(&m_constraints->events.new_constraint, [this](void *) {
        updatePointerConstraint(); // on at once if its surface has the pointer
    });
    m_requestSetSelection.connect(&seat->events.request_set_selection, [this](void *data) {
        auto *event = static_cast<wlr_seat_request_set_selection_event *>(data);
        wlr_seat_set_selection(seat, event->source, event->serial);
    });
    m_requestSetPrimarySelection.connect(&seat->events.request_set_primary_selection, [this](void *data) {
        auto *event = static_cast<wlr_seat_request_set_primary_selection_event *>(data);
        wlr_seat_set_primary_selection(seat, event->source, event->serial);
    });
    // Drag and drop: granted for a button held on the surface (as sway does). Unanswered, a toolkit stays in its drag
    // and ignores clicks (quick clicks that moved became drags in Dolphin).
    m_requestStartDrag.connect(&seat->events.request_start_drag, [this](void *data) {
        auto *event = static_cast<wlr_seat_request_start_drag_event *>(data);
        if (wlr_seat_validate_pointer_grab_serial(seat, event->origin, event->serial)) {
            qCInfo(lcSurface) << "drag and drop started";
            // While it lasts the controller's pointer goes to whatever window the ray is on, not the one pressed, so the
            // drop lands there (else a file from Dolphin could not be dropped on mpv).
            m_dragDestroy.connect(&event->drag->events.destroy, [this](void *) {
                m_dragDestroy.disconnect();
                if (m_bridge) {
                    m_bridge->setDragging(false);
                }
                qCInfo(lcSurface) << "drag and drop ended";
            });
            if (m_bridge) {
                m_bridge->setDragging(true);
            }
            wlr_seat_start_pointer_drag(seat, event->drag, event->serial);
            return;
        }
        qCInfo(lcSurface) << "drag and drop refused (no button held for it)";
        if (event->drag->source) {
            wlr_data_source_destroy(event->drag->source);
        }
    });
    cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(cursor, outputLayout);
    cursorManager = wlr_xcursor_manager_create(nullptr, 24);
    m_requestSetCursor.connect(&seat->events.request_set_cursor, [this](void *data) {
        auto *event = static_cast<wlr_seat_pointer_request_set_cursor_event *>(data);
        if (event->seat_client == seat->pointer_state.focused_client) {
            wlr_cursor_set_surface(cursor, event->surface, event->hotspot_x, event->hotspot_y);
        }
    });
    wlr_cursor_shape_manager_v1 *shapes = wlr_cursor_shape_manager_v1_create(display, 1);
    m_requestSetShape.connect(&shapes->events.request_set_shape, [this](void *data) {
        auto *event = static_cast<wlr_cursor_shape_manager_v1_request_set_shape_event *>(data);
        if (event->seat_client == seat->pointer_state.focused_client) {
            wlr_cursor_set_xcursor(cursor, cursorManager, wlr_cursor_shape_v1_name(event->shape));
        }
    });
    m_input = std::make_unique<Input>(*this);
    if (m_bridge || m_options.virtualPointer) {
        m_input->setVirtualPointer(true); // the controllers' rays
    }
    if (m_bridge || m_options.virtualKeyboard) {
        m_input->enableVirtualKeyboard(); // the virtual keyboard's keys
    }

    m_newOutput.connect(&backend->events.new_output, [this](void *data) {
        auto *output = static_cast<wlr_output *>(data);
        if (m_makingX11Output) {
            // XWayland's X screen: on, its size, in the layout (so it has a wl_output and an xdg-output), never drawn.
            m_x11Output = output;
            wlr_output_init_render(output, allocator, renderer);
            wlr_output_state state;
            wlr_output_state_init(&state);
            wlr_output_state_set_enabled(&state, true);
            wlr_output_state_set_custom_mode(&state, kX11ScreenWidth, kX11ScreenHeight, 0);
            const bool on = wlr_output_commit_state(output, &state);
            wlr_output_state_finish(&state);
            wlr_output_layout_add(outputLayout, output, 0, 0);
            m_x11OutputDestroy.connect(&output->events.destroy, [this](void *) {
                m_x11Output = nullptr;
                m_x11OutputDestroy.disconnect();
            });
            qCInfo(lcWayland).noquote() << QStringLiteral("X11 screen %1x%2 (%3, XWayland's alone, not drawn)")
                                               .arg(kX11ScreenWidth)
                                               .arg(kX11ScreenHeight)
                                               .arg(on ? QStringLiteral("on") : QStringLiteral("NOT on: X11 apps get the usual output"));
            return;
        }
        if (output->non_desktop) {
            offerHeadset(output); // the headset: never a monitor of ours
            return;
        }
        m_outputs.append(new Output(*this, output));
        m_outputs.last()->showStatus(statusState());
        confineCursor();
    });
    m_newInput.connect(&backend->events.new_input, [this](void *data) {
        m_input->addDevice(static_cast<wlr_input_device *>(data));
    });

    for (int i = 0; i < 10 && m_socketName.isEmpty(); ++i) {
        const QByteArray name = "rift-home-" + QByteArray::number(i);
        if (wl_display_add_socket(display, name.constData()) == 0) {
            m_socketName = QString::fromLatin1(name);
        }
    }
    if (m_socketName.isEmpty()) {
        qCCritical(lcWayland) << "no free socket rift-home-0..9 in XDG_RUNTIME_DIR";
        return false;
    }

    wl_event_loop_add_signal(m_loop, SIGINT, onSignal, this);
    wl_event_loop_add_signal(m_loop, SIGTERM, onSignal, this);

    if (!wlr_backend_start(backend)) {
        qCCritical(lcWayland) << "the wlroots backend did not start";
        return false;
    }
    if (m_options.backend == Backend::Headless) {
        wlr_headless_add_output(backend, m_options.width, m_options.height);
    }
    addX11Screen();
    if (m_options.backend == Backend::Drm) {
        // The monitors show a still page: their frames would stop. The windows' frame callbacks and the headset's scene
        // keep their own pace (about the headset's 90 Hz), with or without a monitor.
        m_frameClock = new QTimer(this);
        m_frameClock->setTimerType(Qt::PreciseTimer);
        m_frameClock->setInterval(11);
        connect(m_frameClock, &QTimer::timeout, this, [this] {
            timespec now{};
            clock_gettime(CLOCK_MONOTONIC, &now);
            sendFrameDone(now);
            publishScene();
        });
        m_frameClock->start();
        m_statusTimer = new QTimer(this);
        m_statusTimer->setInterval(1000);
        connect(m_statusTimer, &QTimer::timeout, this, &Server::refreshStatus);
        m_statusTimer->start();
        qCInfo(lcWayland) << "own session: monitors and input from the login session; Ctrl+Alt+F<n> other consoles";
    }

    // wlroots' event loop inside Qt's: its fd wakes us, idle work and client flushes before Qt sleeps.
    m_notifier = new QSocketNotifier(wl_event_loop_get_fd(m_loop), QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &Server::dispatch);
    connect(QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::aboutToBlock, this, &Server::flush);

    m_statsTimer = new QTimer(this);
    m_statsTimer->setInterval(10000);
    connect(m_statsTimer, &QTimer::timeout, this, &Server::logStats);
    m_statsTimer->start();

    qCInfo(lcWayland).noquote() << "socket" << m_socketName << "| backend"
                                << (m_options.backend == Backend::Headless ? "headless" : m_options.backend == Backend::Drm ? "drm (its own session)" : "nested")
                                << "| preferred image description"
                                << (m_options.preferred == Preferred::Pq ? "PQ BT.2020" : "sRGB (default)");
    return true;
}

void Server::createColorManager()
{
    // Rift Home's own color-management-v1: wlroots' offers only what its scene maps (no HLG, no Display P3).
    ColorManagement::Preferred preferred;
    if (m_options.preferred == Preferred::Pq) {
        preferred = {WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020};
    }
    m_colorManagement = ColorManagement::create(display, preferred);
    if (!m_colorManagement) {
        qCWarning(lcWayland) << "color-management-v1 NOT available";
        return;
    }
    qCInfo(lcWayland).noquote() << "color-management-v1 (version 2, Rift Home's own): parametric, set_tf_power, set_primaries, set_luminances,"
                                << "mastering metadata; transfers" << ColorManagement::offeredTransfers().join(QLatin1Char(','))
                                << "| primaries" << ColorManagement::offeredPrimaries().join(QLatin1Char(',')) << "| intent perceptual";
}

void Server::dispatch()
{
    wl_event_loop_dispatch(m_loop, 0);
    flush();
}

void Server::flush()
{
    wl_event_loop_dispatch_idle(m_loop);
    wl_display_flush_clients(display);
}

Window *Server::windowAt(double lx, double ly, wlr_surface **surface, double *sx, double *sy) const
{
    wlr_scene_node *node = wlr_scene_node_at(&scene->tree.node, lx, ly, sx, sy);
    if (!node || node->type != WLR_SCENE_NODE_BUFFER) {
        return nullptr;
    }
    wlr_scene_surface *sceneSurface = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
    if (!sceneSurface) {
        return nullptr;
    }
    *surface = sceneSurface->surface;
    // Popup trees carry no data; the window's tree is the first ancestor that does.
    wlr_scene_tree *tree = node->parent;
    while (tree && !tree->node.data) {
        tree = tree->node.parent;
    }
    return tree ? static_cast<Window *>(tree->node.data) : nullptr;
}

void Server::focusWindow(Window *window)
{
    if (!window || window == m_focused) {
        return;
    }
    wlr_surface *surface = window->surface();
    if (!surface || !window->sceneTree()) {
        return;
    }
    if (m_focused) {
        m_focused->setActivated(false);
    }
    m_focused = window;
    wlr_scene_node_raise_to_top(&window->sceneTree()->node);
    window->setActivated(true);
    if (wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat)) {
        wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    } else {
        wlr_seat_keyboard_notify_enter(seat, surface, nullptr, 0, nullptr);
    }
    updateTextInputFocus();
}

Window *Server::windowOf(wlr_surface *surface) const
{
    if (!surface) {
        return nullptr;
    }
    wlr_surface *root = wlr_surface_get_root_surface(surface);
    for (Window *window : std::as_const(m_windows)) {
        if (window->surface() == root) {
            return window;
        }
    }
    return nullptr;
}

void Server::updateTextInputFocus()
{
    // The text inputs of the focused window's client enter its surface; others leave (the protocol leaves this to
    // the compositor, with the keyboard focus).
    wlr_surface *focused = m_focused ? m_focused->surface() : nullptr;
    for (const auto &t : m_textInputs) {
        wlr_text_input_v3 *ti = t->input;
        if (ti->focused_surface == focused) {
            continue;
        }
        if (ti->focused_surface) {
            wlr_text_input_v3_send_leave(ti);
        }
        if (focused && wl_resource_get_client(ti->resource) == wl_resource_get_client(focused->resource)) {
            wlr_text_input_v3_send_enter(ti, focused);
        }
    }
}

void Server::windowMapped(Window *window)
{
    // A program started from the monitor's page (its Terminal): on the monitor, in front of the page, not in the headset.
    pid_t pid = 0;
    if (window->xsurface()) {
        pid = window->xsurface()->pid;
    } else if (wlr_surface *surface = window->surface()) {
        wl_client_get_credentials(wl_resource_get_client(surface->resource), &pid, nullptr, nullptr);
    }
    if (pid > 0 && m_monitorPids.contains(pid)) {
        window->setOnMonitor(true);
        window->setSlot(-1);
        wlr_scene_node_reparent(&window->sceneTree()->node, m_monitorTree);
        if (!m_outputs.isEmpty()) {
            wlr_box box{}; // the first monitor's (the page is there too)
            wlr_output_layout_get_box(outputLayout, m_outputs.first()->output(), &box);
            const Window::Box g = window->geometry();
            wlr_scene_node_set_position(&window->sceneTree()->node, box.x + std::max(0, (box.width - g.width) / 2),
                                        box.y + std::max(0, (box.height - g.height) / 3));
        }
        qCInfo(lcSurface).noquote() << window->label() << "on the monitor (started from its page), not in the headset";
        focusWindow(window);
        return;
    }
    // Cascade, kept on the output so every window gets frame callbacks in the flat view.
    const int offset = 48 * (m_cascade++ % 8);
    wlr_scene_node_set_position(&window->sceneTree()->node, offset, offset);
    // Its place in front of the user: the first free one.
    int slot = 0;
    for (bool taken = true; taken; ++slot) {
        taken = false;
        for (Window *other : std::as_const(m_windows)) {
            taken |= other != window && other->slot() == slot;
        }
        if (!taken) {
            break;
        }
    }
    window->setSlot(slot);
    if (window->xsurface()) {
        publishCurrentBuffers(window);
    }
    focusWindow(window);
    publishScene(); // to the headset now, not at the flat view's next frame (that one pauses while nothing changes on it)
}

void Server::windowUnmapped(Window *window)
{
    window->setSlot(-1);
    publishScene(); // to the headset now, not at the flat view's next frame (that one pauses while nothing changes on it)
}

void Server::windowDestroyed(Window *window)
{
    m_windows.removeOne(window);
    if (m_focused == window) {
        m_focused = nullptr;
    }
}

void Server::outputDestroyed(Output *output)
{
    m_outputs.removeOne(output);
    confineCursor();
    if (m_outputs.isEmpty() && m_options.backend == Backend::Nested) {
        qCInfo(lcWayland) << "nested window closed - stopping";
        Q_EMIT quitRequested();
    }
}

void Server::offerHeadset(wlr_output *output)
{
    // RiftLinux's rule: offered only while connected; wlroots makes the output only for a connected
    // connector and destroys it when the headset drops off HDMI (its panel off) or while it is leased.
    const QString name = QString::fromUtf8(output->name ? output->name : "?");
    const QString what = QStringLiteral("%1 %2").arg(QString::fromUtf8(output->make ? output->make : ""), QString::fromUtf8(output->model ? output->model : "")).trimmed();
    if (!m_leaseManager || !wlr_drm_lease_v1_manager_offer_output(m_leaseManager, output)) {
        qCWarning(lcWayland).noquote() << "headset" << name << what << "found but could not be offered for lease";
        setHeadsetState(QStringLiteral("Found, cannot be lent"), false);
        return;
    }
    qCInfo(lcWayland).noquote() << "headset" << name << what << "offered for lease (waiting for the runtime)";
    setHeadsetState(QStringLiteral("Waiting for the runtime"), false);
    struct Watch {
        Listener destroy;
    };
    auto *watch = new Watch;
    watch->destroy.connect(&output->events.destroy, [this, watch](void *) {
        // Leased (the runtime has it) or gone from HDMI: only the second changes what the page says.
        if (m_headsetState != QStringLiteral("Lent to the runtime")) {
            setHeadsetState(QStringLiteral("Not connected"), false);
        }
        delete watch;
    });
}

void Server::setHeadsetState(const QString &state, bool ok)
{
    m_headsetState = state;
    m_headsetOk = ok;
    refreshStatus();
}

void Server::setRuntimeState(const QString &state, bool ok)
{
    m_runtimeState = state;
    m_runtimeOk = ok;
    refreshStatus();
}

StatusState Server::statusState() const
{
    StatusState state;
    state.headset = m_headsetState;
    state.headsetOk = m_headsetOk;
    state.runtime = m_runtimeState;
    state.runtimeOk = m_runtimeOk;
    for (const Window *window : std::as_const(m_windows)) {
        state.windows += window->isMapped() && !window->onMonitor() ? 1 : 0;
    }
    state.time = QTime::currentTime().toString(QStringLiteral("HH:mm"));
    state.confirmLeave = m_confirmLeave;
    return state;
}

void Server::refreshStatus()
{
    if (!isDrm()) {
        return;
    }
    const StatusState state = statusState();
    for (Output *output : std::as_const(m_outputs)) {
        output->showStatus(state);
    }
}

bool Server::monitorClick(double lx, double ly)
{
    if (!isDrm()) {
        return false;
    }
    for (Output *output : std::as_const(m_outputs)) {
        switch (output->pageButtonAt(lx, ly)) {
        case Output::PageButton::Leave:
            askLeave();
            return true;
        case Output::PageButton::Terminal:
            qCInfo(lcWayland) << "monitor: terminal asked for";
            Q_EMIT terminalRequested();
            return true;
        case Output::PageButton::None:
            break;
        }
    }
    return false;
}

void Server::askLeave()
{
    // Twice within a few seconds: a stray click or key does not end the session.
    if (m_confirmLeave) {
        qCInfo(lcWayland) << "monitor: leaving Rift Home";
        Q_EMIT quitRequested();
        return;
    }
    m_confirmLeave = true;
    refreshStatus();
    QTimer::singleShot(4000, this, [this] {
        m_confirmLeave = false;
        refreshStatus();
    });
}

void Server::leaveKey()
{
    askLeave();
}

void Server::switchVt(unsigned vt)
{
    if (m_session) {
        qCInfo(lcWayland) << "console" << vt;
        wlr_session_change_vt(m_session, vt);
    }
}

void Server::sendRelativeMotion(uint64_t timeUsec, double dx, double dy, double unaccelDx, double unaccelDy)
{
    if (m_relativePointer) {
        wlr_relative_pointer_manager_v1_send_relative_motion(m_relativePointer, seat, timeUsec, dx, dy, unaccelDx, unaccelDy);
    }
}

bool Server::pointerLocked() const
{
    return m_activeConstraint && m_activeConstraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED;
}

void Server::updatePointerConstraint()
{
    wlr_surface *focus = seat ? seat->pointer_state.focused_surface : nullptr;
    wlr_pointer_constraint_v1 *constraint = focus && m_constraints ? wlr_pointer_constraints_v1_constraint_for_surface(m_constraints, focus, seat) : nullptr;
    if (constraint == m_activeConstraint) {
        return;
    }
    if (m_activeConstraint) {
        m_constraintDestroy.disconnect();
        wlr_pointer_constraint_v1_send_deactivated(m_activeConstraint);
        qCInfo(lcSurface) << "pointer free again";
    }
    m_activeConstraint = constraint;
    if (constraint) {
        m_constraintDestroy.connect(&constraint->events.destroy, [this](void *) {
            m_constraintDestroy.disconnect();
            m_activeConstraint = nullptr;
        });
        wlr_pointer_constraint_v1_send_activated(constraint);
        qCInfo(lcSurface) << (constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED ? "pointer locked by its window (relative motion only)"
                                                                                   : "pointer confined by its window");
    }
}

void Server::trackX11Menu(wlr_xwayland_surface *xsurface)
{
    // An X11 menu or tooltip: once it has a surface and maps, shown with the X11 window it belongs to (its parent, else
    // the focused X11 window, else the newest one). Deletes itself with the X11 window. XWayland gives a menu a new
    // wl_surface each time it is shown and destroys the old one after dissociate: the map listener follows
    // (Steam's menu closed, its surface went with the listener still on it, and wlroots aborted).
    struct Tracker {
        Listener associate;
        Listener dissociate;
        Listener map;
        Listener destroy;
    };
    auto *tracker = new Tracker;
    const auto attach = [this, xsurface, tracker] {
        tracker->map.connect(&xsurface->surface->events.map, [this, xsurface](void *) {
            Window *owner = nullptr;
            for (wlr_xwayland_surface *p = xsurface->parent; p && !owner; p = p->parent) {
                for (Window *w : std::as_const(m_windows)) {
                    if (w->xsurface() == p) {
                        owner = w;
                    }
                }
            }
            if (!owner && m_focused && m_focused->xsurface()) {
                owner = m_focused;
            }
            for (auto it = m_windows.crbegin(); !owner && it != m_windows.crend(); ++it) {
                if ((*it)->xsurface() && (*it)->isMapped()) {
                    owner = *it;
                }
            }
            if (owner) {
                owner->addX11Child(xsurface);
                if (SurfaceExport *exporter = m_exports.value(xsurface->surface)) {
                    exporter->publishCurrent();
                }
                publishScene();
            } else {
                qCInfo(lcSurface) << "X11 menu without an X11 window to show it with: not shown";
            }
        });
    };
    if (xsurface->surface) {
        attach();
    }
    tracker->associate.connect(&xsurface->events.associate, [attach](void *) {
        attach();
    });
    tracker->dissociate.connect(&xsurface->events.dissociate, [tracker](void *) {
        tracker->map.disconnect();
    });
    tracker->destroy.connect(&xsurface->events.destroy, [tracker](void *) {
        delete tracker;
    });
}

void Server::addX11Screen()
{
    if (!m_xwayland || qEnvironmentVariableIntValue("RIFT_HOME_X11_SCREEN") == -1) { // -1: none (checks)
        return;
    }
    // A headless output: on the headless backend itself, else on a headless backend added to the monitors' (nested, DRM).
    wlr_backend *headless = nullptr;
    if (wlr_backend_is_headless(backend)) {
        headless = backend;
    } else if (wlr_backend_is_multi(backend)) {
        headless = wlr_headless_backend_create(m_loop);
        if (headless && !wlr_multi_backend_add(backend, headless)) {
            wlr_backend_destroy(headless);
            headless = nullptr;
        }
    }
    if (!headless) {
        qCWarning(lcWayland) << "no X11 screen of its own: X11 apps get the usual output's size";
        return;
    }
    m_makingX11Output = true;
    wlr_headless_add_output(headless, kX11ScreenWidth, kX11ScreenHeight);
    m_makingX11Output = false;
    confineCursor();
}

void Server::confineCursor()
{
    if (!cursor) {
        return;
    }
    wlr_box all{};
    for (const Output *output : std::as_const(m_outputs)) {
        wlr_box box{};
        wlr_output_layout_get_box(outputLayout, output->output(), &box);
        if (wlr_box_empty(&all)) {
            all = box;
        } else if (!wlr_box_empty(&box)) {
            const int right = std::max(all.x + all.width, box.x + box.width);
            const int bottom = std::max(all.y + all.height, box.y + box.height);
            all.x = std::min(all.x, box.x);
            all.y = std::min(all.y, box.y);
            all.width = right - all.x;
            all.height = bottom - all.y;
        }
    }
    wlr_cursor_map_to_region(cursor, m_x11Output && !wlr_box_empty(&all) ? &all : nullptr);
}

bool Server::clientSees(const wl_client *client, const wl_global *global) const
{
    const bool xwayland = m_xwayland && m_xwayland->server && client == m_xwayland->server->client;
    if (m_x11Output && global == m_x11Output->global) {
        return xwayland;
    }
    if (m_xwayland && m_xwayland->shell_v1 && global == m_xwayland->shell_v1->global) {
        return xwayland;
    }
    if (xwayland && m_x11Output) {
        for (const Output *output : m_outputs) {
            if (global == output->output()->global) {
                return false; // its X screen is the X11 screen alone
            }
        }
    }
    return true;
}

QString Server::x11Display() const
{
    return m_xwayland && m_xwayland->display_name ? QString::fromLatin1(m_xwayland->display_name) : QString();
}

void Server::sendFrameDone(const timespec &now)
{
    ++m_framesPaced;
    // Every mapped window, visible in the flat view or not: in the headset all of them are.
    for (Window *window : std::as_const(m_windows)) {
        if (window->isMapped()) {
            window->forEachSurface([&now](wlr_surface *surface, int, int, bool) {
                wlr_surface_send_frame_done(surface, &now);
            });
        }
    }
}

void Server::registerSurface(wlr_surface *surface, uint64_t id, SurfaceExport *exporter)
{
    m_surfaceIds.insert(surface, id);
    m_exports.insert(surface, exporter);
}

void Server::unregisterSurface(wlr_surface *surface, uint64_t id)
{
    m_surfaceIds.remove(surface);
    m_exports.remove(surface);
    m_videoDescriptions.remove(surface);
    if (m_bridge) {
        m_bridge->surfaceDestroyed(id);
    }
}

void Server::publishCurrentBuffers(Window *window)
{
    window->forEachSurface([this](wlr_surface *surface, int, int, bool) {
        if (SurfaceExport *exporter = m_exports.value(surface)) {
            exporter->publishCurrent();
        }
    });
}

uint64_t Server::lockForXr(wlr_buffer *buffer)
{
    const uint64_t token = m_nextToken++;
    m_lockedBuffers.insert(token, wlr_buffer_lock(buffer));
    return token;
}

void Server::releaseBuffer(uint64_t token)
{
    if (wlr_buffer *buffer = m_lockedBuffers.take(token)) {
        wlr_buffer_unlock(buffer);
    }
}

uint64_t Server::bufferId(wlr_buffer *buffer, bool *isNew)
{
    if (isNew) {
        *isNew = false;
    }
    if (const auto it = m_bufferIds.find(buffer); it != m_bufferIds.end()) {
        return it->second->id;
    }
    if (isNew) {
        *isNew = true;
    }
    auto tracked = std::make_unique<TrackedBuffer>();
    const uint64_t id = m_nextBufferId++;
    tracked->id = id;
    tracked->destroy.connect(&buffer->events.destroy, [this, buffer](void *) {
        const auto it = m_bufferIds.find(buffer);
        if (it == m_bufferIds.end()) {
            return;
        }
        const uint64_t gone = it->second->id;
        m_bufferIds.erase(it); // this listener with it (Listener runs a copy of its handler; wlroots emits mutably)
        if (m_bridge) {
            m_bridge->bufferDestroyed(gone);
        }
    });
    m_bufferIds.emplace(buffer, std::move(tracked));
    return id;
}

namespace
{

wlr_box windowGeometry(Window *window)
{
    const Window::Box g = window->geometry();
    return {g.x, g.y, g.width, g.height};
}

} // namespace

void Server::publishScene()
{
    if (!m_bridge) {
        return;
    }
    std::vector<Xr::WindowPlacement> windows;
    for (Window *window : std::as_const(m_windows)) {
        if (!window->isMapped() || window->slot() < 0) {
            continue;
        }
        Xr::WindowPlacement placement;
        placement.windowId = window->id();
        placement.slot = window->slot();
        placement.appId = window->appId().toStdString();
        placement.title = window->title().toStdString();
        placement.minimizeRequests = window->minimizeRequests();
        // The window is its visible geometry: client-side shadows around it are not part of it (rounded corners,
        // hit testing and the frame go by it).
        // In window pixels (what the app draws: its own units times its scale), as the XR side sizes and hits windows.
        const double scale = pixelScale(window);
        const auto pixels = [scale](int units) {
            return int(std::lround(units * scale));
        };
        const wlr_box geometry = windowGeometry(window);
        placement.width = pixels(geometry.width);
        placement.height = pixels(geometry.height);
        placement.pixelScale = float(scale);
        placement.fullscreen = window->isFullscreen();
        placement.maximized = window->isMaximized();
        placement.focused = window == m_focused;
        for (const auto &t : m_textInputs) {
            if (t->input->current_enabled && windowOf(t->input->focused_surface) == window) {
                placement.wantsKeyboard = true;
            }
        }
        // Bottom to top, positions relative to the window's geometry: the window's own surfaces (main and
        // subsurfaces), then its popups and X11 menus (they may reach outside the window: not cut to it).
        window->forEachSurface([this, &placement, geometry, pixels](wlr_surface *surface, int sx, int sy, bool own) {
            const uint64_t id = surfaceId(surface);
            if (id && wlr_surface_has_buffer(surface)) {
                placement.surfaces.push_back(
                    {id, pixels(sx - geometry.x), pixels(sy - geometry.y), pixels(surface->current.width), pixels(surface->current.height), own});
            }
        });
        windows.push_back(std::move(placement));
    }
    m_bridge->publishScene(std::move(windows));
}

std::vector<std::tuple<int, int, int, double>> Server::mappedWindowSizes() const
{
    std::vector<std::tuple<int, int, int, double>> sizes;
    for (Window *window : std::as_const(m_windows)) {
        if (window->isMapped()) {
            const wlr_box geometry = windowGeometry(window);
            const double scale = pixelScale(window);
            sizes.emplace_back(window->id(), int(std::lround(geometry.width * scale)), int(std::lround(geometry.height * scale)), scale);
        }
    }
    return sizes;
}

double Server::pixelScale(const Window *window) const
{
    return window && window->xsurface() ? 1.0 : m_options.appScale;
}

void Server::setAppScale(double scale)
{
    scale = std::clamp(scale, 1.0, 3.0);
    if (std::abs(scale - m_options.appScale) < 0.001) {
        return;
    }
    m_options.appScale = scale;
    for (Output *output : std::as_const(m_outputs)) {
        output->setScale(scalesPerSurface() ? 1.0 : scale);
    }
    for (SurfaceExport *exporter : std::as_const(m_exports)) {
        exporter->tellScale(); // its own session: told surface by surface
    }
    qCInfo(lcWayland).noquote() << QStringLiteral("apps at %1 % (their windows' pixels a side for each of their own)").arg(qRound(scale * 100.0));
    publishScene();
}

void Server::dumpWindows(const QString &dir, const QString &suffix) const
{
    // From wlroots' textures: shared-memory buffers are released to the client once uploaded. Every surface of the
    // window (Firefox draws its page into a subsurface): window<id>-<suffix>.rhwd for the main surface,
    // window<id>-<suffix>-s<n>-<x>_<y>.rhwd for the others at x,y in main surface coordinates.
    QDir().mkpath(dir);
    struct Context {
        QString base;
        int index = 0;
    };
    for (Window *window : std::as_const(m_windows)) {
        if (!window->isMapped()) {
            continue;
        }
        Context context{QStringLiteral("%1/window%2-%3").arg(dir).arg(window->id()).arg(suffix)};
        if (!window->surface()) {
            continue;
        }
        wlr_surface_for_each_surface(
            window->surface(),
            [](wlr_surface *surface, int sx, int sy, void *data) {
                auto *c = static_cast<Context *>(data);
                const int index = c->index++;
                wlr_texture *texture = wlr_surface_get_texture(surface);
                if (!texture) {
                    return;
                }
                const uint32_t stride = texture->width * 4;
                QByteArray pixels(qsizetype(stride) * texture->height, '\0');
                wlr_texture_read_pixels_options options{};
                options.data = pixels.data();
                options.format = DRM_FORMAT_ARGB8888;
                options.stride = stride;
                if (!wlr_texture_read_pixels(texture, &options)) {
                    return;
                }
                const QString name = index == 0 ? c->base + QStringLiteral(".rhwd")
                                                : QStringLiteral("%1-s%2-%3_%4.rhwd").arg(c->base).arg(index).arg(sx).arg(sy);
                QFile file(name);
                if (file.open(QIODevice::WriteOnly)) {
                    const uint32_t header[5] = {0x44574852u /* "RHWD" */, texture->width, texture->height, stride, DRM_FORMAT_ARGB8888};
                    file.write(reinterpret_cast<const char *>(header), sizeof(header));
                    file.write(pixels);
                }
            },
            &context);
    }
}

void Server::handleXrInput(const Xr::PointerEvent &event)
{
    using Type = Xr::PointerEvent::Type;
    if (event.type == Type::Leave) {
        wlr_seat_pointer_notify_clear_focus(seat);
        return;
    }
    if (event.type == Type::Key) {
        // The virtual keyboard: to the focused window, timed on the monotonic clock as wlroots' own events.
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        m_input->typeKey(event.button, event.pressed, uint32_t(now.tv_sec * 1000 + now.tv_nsec / 1000000));
        return;
    }
    Window *window = nullptr;
    for (Window *w : std::as_const(m_windows)) {
        if (w->id() == event.windowId && w->isMapped()) {
            window = w;
        }
    }
    if (!window) {
        return;
    }
    switch (event.type) {
    case Type::Close:
        qCInfo(lcSurface).noquote() << window->label() << "asked to close (controller)";
        window->close();
        return;
    case Type::Maximize:
        qCInfo(lcSurface).noquote() << window->label() << "maximized (controller)";
        window->setSizeMode(Window::SizeMode::Maximized);
        return;
    case Type::Fullscreen:
        qCInfo(lcSurface).noquote() << window->label() << "fullscreen (controller)";
        window->setSizeMode(Window::SizeMode::Fullscreen);
        return;
    case Type::Restore:
        qCInfo(lcSurface).noquote() << window->label() << "normal size (controller)";
        window->setSizeMode(Window::SizeMode::Normal);
        return;
    case Type::Focus:
        focusWindow(window);
        return;
    default:
        break;
    }
    // Events come in window pixels (Server::pixelScale): the app's own units here.
    const double scale = pixelScale(window);
    if (event.type == Type::Resize) {
        const int width = int(std::lround(event.x / scale));
        const int height = int(std::lround(event.y / scale));
        qCInfo(lcSurface).noquote() << window->label() << QStringLiteral("resized to %1x%2 (controller)").arg(width).arg(height);
        window->resize(width, height);
        return;
    }
    // The surface under the ray: a popup or subsurface on top, else the window itself. Events come in the window's
    // geometry; surfaces are found in main surface coordinates.
    const wlr_box geometry = windowGeometry(window);
    const double wx = event.x / scale + geometry.x;
    const double wy = event.y / scale + geometry.y;
    double sx = 0, sy = 0;
    wlr_surface *surface = window->surfaceAt(wx, wy, &sx, &sy);
    if (event.type == Type::Button) {
        qCInfo(lcSurface).noquote() << window->label() << QStringLiteral("button %1 %2 at %3,%4 ->")
                                                              .arg(event.button)
                                                              .arg(event.pressed ? QStringLiteral("down") : QStringLiteral("up"))
                                                              .arg(event.x, 0, 'f', 1)
                                                              .arg(event.y, 0, 'f', 1)
                                    << (surface ? QStringLiteral("surface %1 at %2,%3").arg(surfaceId(surface)).arg(sx, 0, 'f', 1).arg(sy, 0, 'f', 1)
                                                : QStringLiteral("NO SURFACE (input region)"))
                                    << "| pointer grab" << (seat->pointer_state.grab != seat->pointer_state.default_grab ? "ACTIVE (popup)" : "none");
    }
    if (!surface) {
        // Outside the input region (shadows of client-side decorations): keep the focus for a held button.
        if (seat->pointer_state.button_count == 0) {
            wlr_seat_pointer_notify_clear_focus(seat);
            return;
        }
        surface = seat->pointer_state.focused_surface;
        sx = wx;
        sy = wy;
        if (!surface) {
            return;
        }
    }
    wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
    updatePointerConstraint();
    switch (event.type) {
    case Type::Motion:
        if (pointerLocked()) {
            // A locked pointer (a game's mouse look): the controller's moves on the window become relative motion.
            const double dx = (event.x - m_lastXrX) / scale;
            const double dy = (event.y - m_lastXrY) / scale;
            sendRelativeMotion(uint64_t(event.timeMs) * 1000, dx, dy, dx, dy);
        } else {
            wlr_seat_pointer_notify_motion(seat, event.timeMs, sx, sy);
        }
        m_lastXrX = event.x;
        m_lastXrY = event.y;
        break;
    case Type::Button:
        if (!pointerLocked()) { // a locked pointer does not move (a game's mouse look)
            wlr_seat_pointer_notify_motion(seat, event.timeMs, sx, sy);
        }
        wlr_seat_pointer_notify_button(seat, event.timeMs, event.button,
                                       event.pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
        if (event.pressed) {
            focusWindow(window);
        }
        break;
    case Type::Axis:
        if (event.axisY != 0.0) {
            wlr_seat_pointer_notify_axis(seat, event.timeMs, WL_POINTER_AXIS_VERTICAL_SCROLL, event.axisY, 0,
                                         WL_POINTER_AXIS_SOURCE_FINGER, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
        }
        if (event.axisX != 0.0) {
            wlr_seat_pointer_notify_axis(seat, event.timeMs, WL_POINTER_AXIS_HORIZONTAL_SCROLL, event.axisX, 0,
                                         WL_POINTER_AXIS_SOURCE_FINGER, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
        }
        break;
    case Type::AxisStop:
        // A finger axis of 0 tells the client the scroll stopped (kinetic scrolling may follow).
        wlr_seat_pointer_notify_axis(seat, event.timeMs, WL_POINTER_AXIS_VERTICAL_SCROLL, 0.0, 0,
                                     WL_POINTER_AXIS_SOURCE_FINGER, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
        wlr_seat_pointer_notify_axis(seat, event.timeMs, WL_POINTER_AXIS_HORIZONTAL_SCROLL, 0.0, 0,
                                     WL_POINTER_AXIS_SOURCE_FINGER, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
        break;
    default:
        break;
    }
    wlr_seat_pointer_notify_frame(seat);
}

void Server::logStats()
{
    QStringList parts;
    int subsurfaces = 0;
    for (Window *window : std::as_const(m_windows)) {
        subsurfaces += window->subsurfaceCount();
        const auto [commits, subsurfaceCommits] = window->takeCommitCounts();
        if (window->isMapped()) {
            QString part = QStringLiteral("%1 %2/s").arg(window->label(), QString::number(commits / 10.0, 'f', 1));
            if (subsurfaceCommits > 0) {
                part += QStringLiteral(" (+%1/s subsurfaces)").arg(QString::number(subsurfaceCommits / 10.0, 'f', 1));
            }
            parts << part;
        }
    }
    // The server's own pace (frame callbacks, the scene to the XR side): a frame clock that stops leaves new windows
    // unseen in the headset.
    const int paced = std::exchange(m_framesPaced, 0);
    if (!parts.isEmpty() || paced < 100) {
        qCInfo(lcSurface).noquote() << "commits in 10 s:" << (parts.isEmpty() ? QStringLiteral("none") : parts.join(QStringLiteral(", ")))
                                    << QStringLiteral("| %1 frame(s) paced").arg(paced)
                                    // What is held for surfaces (browsers make and drop video surfaces all the time):
                                    // these must come back down, not pile up.
                                    << QStringLiteral("| held: %1 surfaces (%2 subsurfaces), %3 locked for the XR side, %4")
                                           .arg(m_exports.size())
                                           .arg(subsurfaces)
                                           .arg(m_lockedBuffers.size())
                                           .arg(LinuxDmabuf::heldSummary())
                                    // The C library's heap: what is in use, and what it keeps from the system.
                                    << QStringLiteral("| heap %1 MB in use of %2 MB").arg(double(mallinfo2().uordblks) / 1e6, 0, 'f', 1)
                                           .arg(double(mallinfo2().arena + mallinfo2().hblkhd) / 1e6, 0, 'f', 1);
    }
}

} // namespace RiftHome::Wayland
