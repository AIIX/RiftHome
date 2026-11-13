// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "color/ColorDescription.hpp"

#include <optional>
#include "wayland/Listener.hpp"
#include "wayland/StatusPage.hpp"

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

#include <memory>
#include <tuple>
#include <unordered_map>
#include <vector>

struct timespec;
struct wl_display;
struct wl_event_loop;
struct wlr_allocator;
struct wlr_backend;
struct wlr_buffer;
struct wlr_cursor;
struct wlr_output_layout;
struct wlr_renderer;
struct wlr_output;
struct wlr_scene;
struct wlr_scene_tree;
struct wlr_scene_output_layout;
struct wlr_seat;
struct wlr_surface;
struct wlr_xcursor_manager;
struct wlr_xdg_shell;
struct wlr_xdg_toplevel;
struct wlr_xdg_toplevel_decoration_v1;
struct wlr_xwayland;
struct wlr_xwayland_surface;
struct wlr_session;
struct wlr_drm_lease_v1_manager;
struct wlr_pointer_constraints_v1;
struct wlr_pointer_constraint_v1;
struct wlr_relative_pointer_manager_v1;

class QSocketNotifier;
class QTimer;

namespace RiftHome::Xr
{
class LayerBridge;
struct PointerEvent;
}

namespace RiftHome::Wayland
{

class Input;
class Output;
class SurfaceExport;
class Window;

/*!
 * Rift Home's Wayland server: wlroots on its own socket, run from Qt's event loop. Windows can be shown in a flat view
 * (nested window or headless output); every window's buffer and colour description is logged.
 */
class Server : public QObject
{
    Q_OBJECT

public:
    enum class Backend {
        Headless, //!< no window, no input: for checks over SSH
        Nested, //!< a window on the current desktop: keyboard, mouse and a flat view
        Drm, //!< its own session: the monitors and input itself, the headset leased to the runtime
    };
    enum class Preferred {
        Pq, //!< windows are told PQ / BT.2020 is preferred, so players pass HDR through
        Srgb, //!< the protocol's default; for comparison
    };
    struct Options {
        Backend backend = Backend::Headless;
        Preferred preferred = Preferred::Pq;
        int width = 1920;
        int height = 1080;
        int refreshMilliHz = 90000;
        bool verbose = false;
        bool virtualPointer = false; //!< offer wl_pointer without a device (the controllers' rays, test clicks)
        bool virtualKeyboard = false; //!< offer wl_keyboard without a device (the virtual keyboard, test typing)
        //! Apps' scale (HiDPI): Wayland apps draw this many pixels a side for each of their own (the
        //! output's scale; fractional for apps that take wp_fractional_scale_v1). X11 apps have their own (Xft.dpi).
        double appScale = 2.0;
    };

    explicit Server(const Options &options, QObject *parent = nullptr);
    ~Server() override;

    //! The XR side's handoff; set before start(). Without one, Rift Home is only a flat compositor.
    void setLayerBridge(Xr::LayerBridge *bridge)
    {
        m_bridge = bridge;
    }
    Xr::LayerBridge *layerBridge() const
    {
        return m_bridge;
    }
    //! Creates the display, backend and globals and opens the socket. False (logged) on failure.
    bool start();
    QString socketName() const
    {
        return m_socketName;
    }

    // Used by Window, Output and Input.
    wl_display *display = nullptr;
    wlr_backend *backend = nullptr;
    wlr_renderer *renderer = nullptr;
    wlr_allocator *allocator = nullptr;
    wlr_scene *scene = nullptr;
    wlr_scene_output_layout *sceneLayout = nullptr;
    wlr_output_layout *outputLayout = nullptr;
    wlr_seat *seat = nullptr;
    wlr_cursor *cursor = nullptr;
    wlr_xcursor_manager *cursorManager = nullptr;
    bool explicitSync = false;

    /*!
     * XWayland's X screen: an output of its own that only XWayland sees, never drawn. X11 keeps the pointer
     * on the X screen, which XWayland sizes to the outputs it sees; at the 1920x1080 output the lower and right parts of
     * Steam's 2683x2078 window got no pointer at all (the library's highlight stopped half way, the
     * Install dialog's buttons took no clicks). Wayland apps keep seeing the usual output. X11 windows are kept to it, and
     * it is their fullscreen and maximized size: a 1440p monitor to X11 games (sharp on a cinema screen, not a 4K load).
     */
    static constexpr int kX11ScreenWidth = 2560;
    static constexpr int kX11ScreenHeight = 1440;

    bool isDrm() const
    {
        return m_options.backend == Backend::Drm;
    }
    /*!
     * Whether apps are told their scale surface by surface (fractional scale, preferred buffer scale) instead of by an
     * output's: in its own session, where the outputs are the monitors. RIFT_HOME_TEST_SCALE_PER_SURFACE=1 does the same
     * in a headless run (its output at 100 %), to check apps as that session scales them (Chromium's text was
     * far too small there, Firefox's right).
     */
    bool scalesPerSurface() const
    {
        return isDrm() || m_scalePerSurfaceTest;
    }
    //! The headset's windows' scene trees go here; the monitor's page and its windows above, in monitorTree().
    wlr_scene_tree *roomTree() const
    {
        return m_roomTree;
    }
    wlr_scene_tree *monitorTree() const
    {
        return m_monitorTree;
    }
    //! What the monitor's page shows.
    StatusState statusState() const;
    //! The runtime's state, from whoever runs it (main: riftlinux-runtime).
    void setRuntimeState(const QString &state, bool ok);
    //! A program started from the monitor's page: its windows show on the monitor, not in the headset.
    void addMonitorProcess(qint64 pid)
    {
        m_monitorPids.insert(pid);
    }
    //! A click on the monitor at layout coordinates: true when it was the page's (a button).
    bool monitorClick(double lx, double ly);
    //! `Ctrl+Alt+F<n>`: another console. Ctrl+Alt+Backspace: leave (twice).
    void switchVt(unsigned vt);
    void leaveKey();
    // Games' mouse look: pointer lock and relative motion.
    //! Relative motion to the pointer's focus (time in microseconds), whether or not the pointer is locked.
    void sendRelativeMotion(uint64_t timeUsec, double dx, double dy, double unaccelDx, double unaccelDy);
    //! The pointer's focus has locked it (an FPS's mouse look): the cursor stays, only relative motion goes.
    bool pointerLocked() const;
    //! After the pointer's focus changed: its constraint (lock or confinement) on, the old one off.
    void updatePointerConstraint();
    const Options &options() const
    {
        return m_options;
    }
    Window *windowAt(double lx, double ly, wlr_surface **surface, double *sx, double *sy) const;
    void focusWindow(Window *window);
    //! XWayland's display (":1"): the DISPLAY of the programs started inside Rift Home; empty without XWayland.
    QString x11Display() const;
    void windowMapped(Window *window);
    void windowUnmapped(Window *window);
    void windowDestroyed(Window *window);
    // Surfaces and buffers shared with the XR side.
    void registerSurface(wlr_surface *surface, uint64_t id, SurfaceExport *exporter);
    void unregisterSurface(wlr_surface *surface, uint64_t id);
    //! Hands the window's current buffers to the XR side again: an X11 surface's first buffer can come before XWayland
    //! ties the surface to its window (it is not passed on then), and a window drawn once would stay empty.
    void publishCurrentBuffers(Window *window);
    uint64_t surfaceId(wlr_surface *surface) const
    {
        return m_surfaceIds.value(surface);
    }
    //! Locks a buffer for the XR side; the token comes back through releaseBuffer().
    uint64_t lockForXr(wlr_buffer *buffer);
    //! The buffer's id for the XR side, which keeps its import while the buffer lives: the same for the buffer's whole
    //! life and never reused. The XR side is told when the buffer is destroyed. @p isNew: given just now.
    uint64_t bufferId(wlr_buffer *buffer, bool *isNew = nullptr);
    void releaseBuffer(uint64_t token);
    //! The mapped windows and their surfaces, for the XR side (each output frame).
    void publishScene();
    //! Pointer input from the controllers, on the Wayland thread.
    void handleXrInput(const Xr::PointerEvent &event);
    //! Debug: the mapped windows' ids, sizes (visible geometry, in the XR side's window pixels) and pixel scales.
    std::vector<std::tuple<int, int, int, double>> mappedWindowSizes() const;
    //! Apps' scale (Options::appScale), live: the outputs' scale; open apps draw again at it.
    void setAppScale(double scale);
    //! Window pixels a pixel of the app's own for @p window: the apps' scale for a Wayland window, 1 for an X11 one. The
    //! XR side works in window pixels (what the app draws), Wayland in the app's own (surface-local) units.
    double pixelScale(const Window *window) const;
    //! Debug: each mapped window's shared-memory buffer to `<dir>/window<id>-<suffix>.rhwd` (see tools/rhwd.py).
    void dumpWindows(const QString &dir, const QString &suffix) const;
    void outputDestroyed(Output *output);
    //! The colour description a window's (@p root's) last tagged video (P010) surface had, for its next untagged ones.
    void rememberVideoDescription(wlr_surface *root, const Color::SurfaceColorDescription &description)
    {
        m_videoDescriptions.insert(root, description);
    }
    std::optional<Color::SurfaceColorDescription> videoDescription(wlr_surface *root) const
    {
        const auto it = m_videoDescriptions.constFind(root);
        return it == m_videoDescriptions.constEnd() ? std::nullopt : std::optional(*it);
    }
    //! Frame callbacks for every mapped window, once per output frame.
    void sendFrameDone(const timespec &now);
    wlr_xdg_toplevel_decoration_v1 *decorationFor(wlr_xdg_toplevel *toplevel) const
    {
        return m_decorations.value(toplevel);
    }

Q_SIGNALS:
    void quitRequested();
    //! The monitor page's Terminal button.
    void terminalRequested();
    //! XWayland is up (it starts with the first X11 app).
    void x11Ready();

private:
    void dispatch();
    void flush();
    void createColorManager();
    void logStats();

    Options m_options;
    bool m_scalePerSurfaceTest = qEnvironmentVariableIntValue("RIFT_HOME_TEST_SCALE_PER_SURFACE") == 1;
    QString m_socketName;
    wl_event_loop *m_loop = nullptr;
    QSocketNotifier *m_notifier = nullptr;
    QTimer *m_statsTimer = nullptr;
    wlr_xdg_shell *m_xdgShell = nullptr;
    class ColorManagement *m_colorManagement = nullptr; //!< freed with the display
    std::unique_ptr<Input> m_input;
    QList<Output *> m_outputs;
    void addX11Screen();
    //! The mouse's cursor on the monitors only, never on the X11 screen beyond them (its own session).
    void confineCursor();
    //! Which client sees which global: the X11 screen and XWayland's shell are XWayland's alone, the monitors not its.
    bool clientSees(const wl_client *client, const wl_global *global) const;
    wlr_output *m_x11Output = nullptr;
    bool m_makingX11Output = false;
    Listener m_x11OutputDestroy;
    int m_framesPaced = 0; //!< sendFrameDone calls since the last stats line
    // Its own session (Backend::Drm): the login session, the headset's lease, the monitor's page.
    void offerHeadset(wlr_output *output);
    void setHeadsetState(const QString &state, bool ok);
    void refreshStatus();
    void askLeave();
    wlr_session *m_session = nullptr;
    wlr_drm_lease_v1_manager *m_leaseManager = nullptr;
    Listener m_leaseRequest;
    Listener m_leaseEnded;
    wlr_scene_tree *m_roomTree = nullptr;
    wlr_scene_tree *m_monitorTree = nullptr;
    QString m_headsetState = QStringLiteral("Not connected");
    bool m_headsetOk = false;
    QString m_runtimeState = QStringLiteral("Not started");
    bool m_runtimeOk = false;
    bool m_confirmLeave = false;
    QSet<qint64> m_monitorPids;
    QTimer *m_statusTimer = nullptr;
    QTimer *m_frameClock = nullptr; //!< DRM: the windows' frames and the headset's scene, whatever the monitors do
    wlr_relative_pointer_manager_v1 *m_relativePointer = nullptr;
    wlr_pointer_constraints_v1 *m_constraints = nullptr;
    wlr_pointer_constraint_v1 *m_activeConstraint = nullptr;
    Listener m_newConstraint;
    Listener m_constraintDestroy;
    double m_lastXrX = 0.0; //!< the controller's last place on a locked window: its moves become relative motion
    double m_lastXrY = 0.0;
    QList<Window *> m_windows;
    Window *m_focused = nullptr;
    wlr_xwayland *m_xwayland = nullptr; //!< X11 apps (started when the first connects)
    // Text fields (text-input-v3): an app with one enabled in the focused window wants the keyboard.
    struct TextInput;
    std::vector<std::unique_ptr<TextInput>> m_textInputs;
    Listener m_newTextInput;
    void updateTextInputFocus();
    Window *windowOf(wlr_surface *surface) const;
    void trackX11Menu(wlr_xwayland_surface *xsurface);
    QHash<wlr_xdg_toplevel *, wlr_xdg_toplevel_decoration_v1 *> m_decorations;
    int m_nextWindowId = 1;
    int m_cascade = 0;
    Xr::LayerBridge *m_bridge = nullptr;
    QHash<wlr_surface *, uint64_t> m_surfaceIds;
    QHash<wlr_surface *, SurfaceExport *> m_exports;
    QHash<wlr_surface *, Color::SurfaceColorDescription> m_videoDescriptions; //!< by window root surface
    uint64_t m_nextSurfaceId = 1;
    QHash<uint64_t, wlr_buffer *> m_lockedBuffers;
    uint64_t m_nextToken = 1;
    //! Buffers the XR side has seen (bufferId): their ids, until each is destroyed.
    struct TrackedBuffer {
        uint64_t id = 0;
        Listener destroy;
    };
    std::unordered_map<wlr_buffer *, std::unique_ptr<TrackedBuffer>> m_bufferIds;
    uint64_t m_nextBufferId = 1;

    Listener m_newOutput;
    Listener m_newInput;
    Listener m_newSurface;
    Listener m_newToplevel;
    Listener m_newX11Surface; //!< XWayland
    Listener m_x11Ready;
    Listener m_newPopup;
    Listener m_newDecoration;
    Listener m_requestSetCursor;
    Listener m_requestSetSelection;
    Listener m_requestSetPrimarySelection;
    Listener m_requestStartDrag;
    Listener m_dragDestroy; //!< the drag in progress ended (dropped or cancelled)
    Listener m_requestSetShape;
};

} // namespace RiftHome::Wayland
