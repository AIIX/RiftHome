// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "spatial/Controls.hpp"
#include "xr/FrameArt.hpp"

#include <QImage>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace RiftHome::Xr
{

class LayerBridge;

/*!
 * Rift Home's OpenXR session on RiftLinux's runtime: its own thread, Vulkan, one quad layer per window surface
 * with the surface's HDR description chained (XR_MNDX_composition_layer_hdr). Copies a surface's buffer into its
 * swapchain when the surface has a new one; the runtime shows each layer's last image every frame, so a slow or hung
 * app never holds up the headset.
 */
class XrCompositor
{
public:
    struct Options {
        //! Window size in the room: 1920 px = 1.92 m, about 60 degrees at 1.6 m, like a visionOS main window.
        float pixelsPerMeter = 1000.0f;
        float distance = 1.6f; //!< metres from the user
        float height = -0.1f; //!< window centres, metres below eye height
        float gapDegrees = 2.5f; //!< between neighbouring windows, which sit side by side around the user
        float cornerRadius = 22.0f; //!< rounded window corners, window pixels (0: square)
        FrameArt::Theme frameTheme = FrameArt::Theme::Dark; //!< the window frame's style
        //! The room around the windows: an image. A 2:1 one is a 360-degree panorama (on a sphere far away); any
        //! other is a photo on a curved screen in front of the home, environmentAngle degrees wide. Empty: black.
        QString environment;
        //! A photo's width around the user, degrees (150 bent it too much, the room too close).
        float environmentAngle = 135.0f;
        float environmentDistance = 4.75f; //!< metres to the environment (a room-sized sphere; 0: infinitely far)
        float roomBrightness = 1.0f; //!< the environment's light (Settings), 0.2..1
        //! Picture quality (Settings, display.composition): the runtime's picture, percent of the panel's pixels a side;
        //! windows' copies are made to match it (Spatial::Sharpness::targetDensity).
        int pictureQuality = 165;
        float cinemaRoomLight = 0.15f; //!< the room's light in cinema mode, of roomBrightness (Settings: dark 0, dim, soft)
        float roomTurnDegrees = 0.0f; //!< a 360-degree room turned around the user (Settings), positive to the right
        // The controllers' rays (Settings): their colour, thickness, and hidden while still.
        QColor pointerColour = QColor::fromRgbF(0.85f, 0.92f, 1.0f);
        float pointerWidth = 0.006f; //!< metres
        int pointerHideSeconds = 0; //!< hidden after this long without movement or a button; 0: never
        bool use24Hour = true; //!< the dock's clock
        float windowCurve = 0.5f; //!< how much the arrangement curves around the user (Settings): 1 around, 0 flat
        Spatial::ButtonMap buttons = Spatial::ButtonMap::defaults(); //!< what the controllers' buttons do (Settings)
        int settingsApp = -1; //!< the Settings app's index in apps: the dock's gear starts it, or brings its window in front
        //! The virtual keyboard shows by itself when an app focuses a text field (else only from the window menu).
        bool keyboardShowOnTextField = true;
        //! The launcher's apps (left menu button), in their order; starting one is a PointerEvent::Type::Launch.
        struct LauncherApp {
            QString name;
            QImage icon;
            QString id; //!< its desktop file id without ".desktop" (matched with windows' app ids)
        };
        std::vector<LauncherApp> apps;
        //! Debug: windows fixed to the view (centred, at eye height), as RiftLinux's HDR test app places its quad.
        bool viewLocked = false;
        //! Desk checks: the room laid out around the headset's view wherever it lies (the head is the origin); real
        //! controllers ignored (the self-test's hand works).
        bool deskView = false;
        //! Window placements (JSON), remembered by app id; empty: not kept.
        QString stateFile;
        //! Where the controllers' models are looked for (ControllerLeft.glb, ControllerRight.glb), first found wins.
        QStringList controllerModelDirs;
        //! Desk check: a scripted right hand points, clicks, grabs and moves, scrolls and toggles follow mode.
        bool selfTest = false;
    };

    XrCompositor(LayerBridge &bridge, const Options &options);
    ~XrCompositor();
    XrCompositor(const XrCompositor &) = delete;
    XrCompositor &operator=(const XrCompositor &) = delete;

    void start();
    //! Ends the session and joins the thread.
    void stop();
    //! Called (on the XR thread) when the runtime ended the session by itself, not stop(): the Rift panel closed Rift
    //! Home, or the runtime went away (Rift Home once kept running without the headset). Set before start().
    void setRuntimeClosedHandler(std::function<void()> handler)
    {
        m_runtimeClosed = std::move(handler);
    }

    /*!
     * Settings records a binding here (the D-Bus RecordBinding), since only Rift Home sees the controllers' buttons:
     * from now no button acts, and when all that were pressed are let go @p handler gets the binding's settings name
     * ("rightSecondary", "leftGrip+rightSecondary"), or an empty one when cancelled or after 10 s. Called on the XR
     * thread. Set before start().
     */
    void setBindingRecordedHandler(std::function<void(const QString &)> handler)
    {
        m_bindingRecorded = std::move(handler);
    }
    //! Safe from any thread: taken by the XR thread before its next frame.
    void startBindingRecording()
    {
        m_recordCommand = 1;
    }
    void cancelBindingRecording()
    {
        m_recordCommand = 2;
    }
    /*!
     * The lens test (Settings, the D-Bus ShowLensTest): only a test picture shown, as Oculus' "Adjust Your Lenses", each
     * eye's on its lens's axis, until any button is pressed. Safe from any thread: taken by the XR thread before its
     * next frame.
     */
    void showLensTest()
    {
        m_lensTestCommand = true;
    }

    //! Rift Home's settings that apply at once (settings.json, changed by the Settings app).
    struct LiveSettings {
        FrameArt::Theme frameTheme = FrameArt::Theme::Dark;
        bool keyboardShowOnTextField = true;
        QString environment; //!< the room's photo (a change rebuilds the room off the frame loop)
        float roomBrightness = 1.0f;
        float cinemaRoomLight = 0.15f;
        float roomTurnDegrees = 0.0f;
        QColor pointerColour = QColor::fromRgbF(0.85f, 0.92f, 1.0f);
        float pointerWidth = 0.006f;
        int pointerHideSeconds = 0;
        bool use24Hour = true;
        float windowCurve = 0.5f;
        Spatial::ButtonMap buttons = Spatial::ButtonMap::defaults();
    };
    //! Safe from any thread: taken by the XR thread before its next frame.
    void applySettings(const LiveSettings &settings);

private:
    struct Impl;
    void run();

    LayerBridge &m_bridge;
    Options m_options;
    std::unique_ptr<Impl> m_impl;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::function<void()> m_runtimeClosed;
    std::function<void(const QString &)> m_bindingRecorded;
    std::atomic<int> m_recordCommand{0};
    std::atomic<bool> m_lensTestCommand{false};
    std::mutex m_settingsMutex;
    std::optional<LiveSettings> m_pendingSettings;
};

} // namespace RiftHome::Xr
