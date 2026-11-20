// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "app/AppCatalog.hpp"
#include "app/AppLauncher.hpp"
#include "app/Chromium.hpp"
#include "app/FirefoxPrefs.hpp"
#include "app/GpuProfile.hpp"
#include "app/BusForwarder.hpp"
#include "app/ControlsService.hpp"
#include "app/HeadsetService.hpp"
#include "app/NotificationServer.hpp"
#include "spatial/VirtualKeyboard.hpp"
#include "wayland/Server.hpp"
#include "xr/LayerBridge.hpp"
#include "xr/XrCompositor.hpp"

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QProcess>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

#include <linux/input-event-codes.h>

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <functional>
#include <memory>

using RiftHome::Wayland::Server;

// AddressSanitizer's live-heap report, when Rift Home is built with it (else null).
extern "C" void __sanitizer_print_memory_profile(size_t topPercent, size_t maxContexts) __attribute__((weak));

namespace
{

QString settingsFile; //!< <state>/settings.json (Rift Home's settings; the Settings app writes it)

//! A process of this exact name runs (/proc/<pid>/comm: SteamVR's vrserver).
bool processRunning(const QByteArray &name)
{
    for (const QString &pid : QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!pid.front().isDigit()) {
            continue;
        }
        QFile comm(QStringLiteral("/proc/%1/comm").arg(pid));
        if (comm.open(QIODevice::ReadOnly) && comm.readAll().trimmed() == name) {
            return true;
        }
    }
    return false;
}

//! Whether Rift Home runs from a build folder in its source tree (development) rather than installed.
bool runsFromWorkTree()
{
    const QString dir = QCoreApplication::applicationDirPath();
    return QFileInfo::exists(dir + QStringLiteral("/CMakeCache.txt")) && QFileInfo::exists(dir + QStringLiteral("/../CMakeLists.txt"));
}

//! Rift Home's state (settings, window places, logs): run/ next to the build in the source tree, else
//! ~/.local/state/rift-home.
QString defaultStateDir()
{
    if (runsFromWorkTree()) {
        return QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/../run/state"));
    }
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/rift-home");
}

//! Rift Home's own rooms: the source tree's (data/rooms and the test media), then the installed ones
//! (`<data>/rift-home/rooms`).
QStringList ownRoomDirs()
{
    QStringList dirs;
    if (runsFromWorkTree()) {
        dirs << QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/../data/rooms"));
        dirs << QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/../test-media/rooms"));
    }
    dirs << QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, QStringLiteral("rift-home/rooms"), QStandardPaths::LocateDirectory);
    return dirs;
}

//! The room when none is given or chosen: the twilight lake, from the source tree or installed.
QString defaultRoomImage()
{
    if (runsFromWorkTree()) {
        const QString room = QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/../data/rooms/twilight-lake.hdr"));
        if (QFileInfo::exists(room)) {
            return room;
        }
    }
    return QStandardPaths::locate(QStandardPaths::GenericDataLocation, QStringLiteral("rift-home/rooms/twilight-lake.hdr"));
}

//! What of settings.json applies at once (the rest is read at start).
/*!
 * Apps' scale (else apps draw at 1x and enlarged windows show their pixels): Settings "Apps' size", apps.scale
 * (100-300 %, default 200 %): Wayland apps draw that many pixels a side for each of their own, their windows that much
 * bigger in the room. RIFT_HOME_APP_SCALE overrides it (checks).
 */
double appScaleOf(const QJsonObject &settings)
{
    if (const double scale = qEnvironmentVariable("RIFT_HOME_APP_SCALE").toDouble(); scale > 0.0) {
        return std::clamp(scale, 1.0, 3.0);
    }
    return std::clamp(settings.value(QStringLiteral("apps")).toObject().value(QStringLiteral("scale")).toDouble(2.0), 1.0, 3.0);
}

/*!
 * Rift Home's own bus (rift-home-run.sh's dbus-run-session, while nested in a desktop): the services it started on demand
 * (portals, KDE's wallet front end, ...) outlive it, as the bus goes with Rift Home but they were not its children (kwalletd6 and ksecretd
 * of earlier sessions kept running). Asked to end, each only when its own session bus
 * is this one, never the desktop's, after the apps have been ended. Not in its own session: that bus is the session's.
 */
void endOwnBusServices()
{
    const QByteArray ours = qgetenv("DBUS_SESSION_BUS_ADDRESS");
    if (qEnvironmentVariableIsEmpty("RIFT_HOME_DESKTOP_BUS") || ours.isEmpty()) {
        return;
    }
    QDBusConnectionInterface *bus = QDBusConnection::sessionBus().interface();
    if (!bus) {
        return;
    }
    const QStringList names = bus->registeredServiceNames().value();
    QSet<uint> ended;
    for (const QString &name : names) {
        if (!name.startsWith(QLatin1Char(':'))) {
            continue;
        }
        const uint pid = bus->servicePid(name).value();
        if (pid == 0 || pid == uint(QCoreApplication::applicationPid()) || ended.contains(pid)) {
            continue;
        }
        QFile environ(QStringLiteral("/proc/%1/environ").arg(pid));
        if (!environ.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QList<QByteArray> variables = environ.readAll().split('\0');
        if (!variables.contains("DBUS_SESSION_BUS_ADDRESS=" + ours)) {
            continue; // not started on this bus (a desktop service reached through it): never ended here
        }
        QFile comm(QStringLiteral("/proc/%1/comm").arg(pid));
        const QByteArray program = comm.open(QIODevice::ReadOnly) ? comm.readAll().trimmed() : QByteArray("?");
        if (::kill(pid_t(pid), SIGTERM) == 0) {
            ended.insert(pid);
            qInfo("own bus: %s (%u) ended with Rift Home", program.constData(), pid);
        }
    }
}

RiftHome::Xr::XrCompositor::LiveSettings liveSettings(const QJsonObject &settings)
{
    const auto group = [&](const char *name) {
        return settings.value(QLatin1String(name)).toObject();
    };
    RiftHome::Xr::XrCompositor::LiveSettings live;
    live.frameTheme = group("frame").value(QStringLiteral("theme")).toString() == QLatin1String("light") ? RiftHome::Xr::FrameArt::Theme::Light
                                                                                                         : RiftHome::Xr::FrameArt::Theme::Dark;
    live.keyboardShowOnTextField = group("keyboard").value(QStringLiteral("showOnTextField")).toBool(true);
    live.environment = group("room").value(QStringLiteral("photo")).toString();
    live.roomBrightness = float(std::clamp(group("room").value(QStringLiteral("brightness")).toDouble(1.0), 0.2, 1.0));
    live.roomTurnDegrees = float(std::clamp(group("room").value(QStringLiteral("turn")).toDouble(0.0), -180.0, 180.0));
    // The controllers' rays: colour, thickness, hidden while still.
    const QJsonObject pointer = group("pointer");
    const QString colour = pointer.value(QStringLiteral("colour")).toString();
    live.pointerColour = colour == QLatin1String("white")  ? QColor(255, 255, 255)
        : colour == QLatin1String("green")                 ? QColor(110, 255, 150)
        : colour == QLatin1String("amber")                 ? QColor(255, 180, 90)
        : colour == QLatin1String("pink")                  ? QColor(255, 120, 215)
                                                           : QColor::fromRgbF(0.85f, 0.92f, 1.0f); // "ice", the default
    const QString thickness = pointer.value(QStringLiteral("thickness")).toString();
    live.pointerWidth = thickness == QLatin1String("thin") ? 0.0035f : thickness == QLatin1String("thick") ? 0.011f : 0.006f;
    live.pointerHideSeconds = std::max(0, pointer.value(QStringLiteral("hideAfter")).toInt(0));
    live.use24Hour = group("clock").value(QStringLiteral("use24Hour")).toBool(true);
    // How much windows curve around the user (a slider, starting gentle).
    live.windowCurve = float(std::clamp(group("windows").value(QStringLiteral("curve")).toDouble(0.5), 0.0, 1.0));
    // What the controllers' buttons do (each one the user's choice, or back to the defaults).
    live.buttons = RiftHome::Spatial::ButtonMap::fromJson(group("controls"));
    // The room in cinema mode: off, dim (a quarter was not dim enough) or soft.
    const QString cinemaRoom = group("cinema").value(QStringLiteral("room")).toString();
    live.cinemaRoomLight = cinemaRoom == QLatin1String("dark") ? 0.0f : cinemaRoom == QLatin1String("soft") ? 0.4f : 0.15f;
    return live;
}

} // namespace

int main(int argc, char **argv)
{
    RiftHome::App::AppLauncher::raiseFileLimit(); // clients' dmabufs: one or more files each
    // A GUI application only for the fonts of the window frame's labels (drawn into images): no window system, and
    // apps started from Rift Home get their own QT_QPA_PLATFORM (AppLauncher).
    const QByteArray platform = qgetenv("QT_QPA_PLATFORM");
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    if (platform.isNull()) {
        qunsetenv("QT_QPA_PLATFORM");
    } else {
        qputenv("QT_QPA_PLATFORM", platform);
    }
    QCoreApplication::setApplicationName(QStringLiteral("rift-home"));
    QCoreApplication::setApplicationVersion(QStringLiteral(PROJECT_VERSION_STRING));
    qSetMessagePattern(QStringLiteral("%{time hh:mm:ss.zzz} %{if-category}%{category}: %{endif}%{message}"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Rift Home VR: a spatial Linux desktop for the Rift CV1. Without --xr: the Wayland compositor alone, no headset."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption backendOption(QStringLiteral("backend"),
                                           QStringLiteral("headless (default), nested (a window on this desktop) or drm (its own session: monitors, input, the headset leased)."),
                                           QStringLiteral("name"), QStringLiteral("headless"));
    const QCommandLineOption preferredOption(
        QStringLiteral("preferred"), QStringLiteral("Image description windows are told to prefer: pq (default) or srgb."),
        QStringLiteral("name"), QStringLiteral("pq"));
    const QCommandLineOption runOption(QStringLiteral("run"), QStringLiteral("Start a program inside Rift Home (repeatable)."),
                                       QStringLiteral("command"));
    const QCommandLineOption logDirOption(QStringLiteral("log-dir"),
                                          QStringLiteral("Folder for the programs' logs (default: logs/<time> in the state folder)."),
                                          QStringLiteral("dir"));
    const QCommandLineOption isolateOption(
        QStringLiteral("isolate-xdg"),
        QStringLiteral("Programs get their own XDG config/data/cache/state folders under this folder (default: ~/.local/share/rift-home/apps when installed)."),
        QStringLiteral("dir"));
    const QCommandLineOption dumpAfterOption(QStringLiteral("dump-after"),
                                             QStringLiteral("Debug: after this many seconds, save every window's picture (no click). Repeatable."),
                                             QStringLiteral("seconds"));
    const QCommandLineOption startRuntimeOption(
        QStringLiteral("start-runtime"),
        QStringLiteral("Start RiftLinux's runtime once Rift Home serves, in Wayland direct mode only (it leases the headset from Rift Home), then the headset side."));
    // Not "--session": QGuiApplication takes "-session <id>" / "--session <id>" for itself (X11 session management) and
    // removes it, with the argument after it, before this parser sees them: the login session's display never reached the
    // user's services (Stremio's player choice did not open, its portal had no display).
    const QCommandLineOption sessionOption(
        QStringLiteral("login-session"),
        QStringLiteral("Rift Home is the login session: its display goes to the user's service manager and D-Bus (programs they start reach it)."));
    const QCommandLineOption withProfileOption(
        QStringLiteral("with-profile"),
        QStringLiteral("Programs use the user's own settings and profiles, not the --isolate-xdg folder (also RIFT_HOME_WITH_PROFILE=1, or Settings)."));
    const QCommandLineOption quitAfterOption(QStringLiteral("quit-after"),
                                             QStringLiteral("Stop after this many seconds (0: until stopped)."),
                                             QStringLiteral("seconds"), QStringLiteral("0"));
    const QCommandLineOption sizeOption(QStringLiteral("size"), QStringLiteral("Headless output size."), QStringLiteral("WxH"),
                                        QStringLiteral("1920x1080"));
    const QCommandLineOption verboseOption(QStringLiteral("verbose"), QStringLiteral("wlroots' debug log."));
    const QCommandLineOption xrOption(QStringLiteral("xr"),
                                      QStringLiteral("Show the windows in the headset (OpenXR; start through riftlinux-run)."));
    const QCommandLineOption gapOption(QStringLiteral("xr-gap-degrees"),
                                       QStringLiteral("Degrees between neighbouring windows in the headset (default 2.5)."),
                                       QStringLiteral("degrees"), QStringLiteral("2.5"));
    const QCommandLineOption distanceOption(QStringLiteral("xr-distance"),
                                            QStringLiteral("Metres from the user to the windows (default 1.6)."),
                                            QStringLiteral("metres"), QStringLiteral("1.6"));
    const QCommandLineOption ppmOption(QStringLiteral("xr-ppm"),
                                       QStringLiteral("Window pixels per metre in the headset (default 1000)."),
                                       QStringLiteral("pixels"), QStringLiteral("1000"));
    const QCommandLineOption cornerOption(QStringLiteral("xr-corner-radius"),
                                          QStringLiteral("Rounded window corners in the headset, window pixels (default 22; 0: square)."),
                                          QStringLiteral("pixels"), QStringLiteral("22"));
    const QCommandLineOption themeOption(QStringLiteral("xr-frame-theme"),
                                         QStringLiteral("The window frame's style in the headset: dark or light (default dark)."),
                                         QStringLiteral("style"), QStringLiteral("dark"));
    const QCommandLineOption environmentOption(QStringLiteral("xr-environment"),
                                               QStringLiteral("The room around the windows: an image (2:1 = a 360-degree panorama; else a photo on a curved screen in front)."),
                                               QStringLiteral("image"));
    const QCommandLineOption environmentAngleOption(QStringLiteral("xr-environment-angle"),
                                                    QStringLiteral("How wide a photo environment is around the user, degrees (default 135)."),
                                                    QStringLiteral("degrees"), QStringLiteral("135"));
    const QCommandLineOption viewLockedOption(QStringLiteral("xr-view-locked"),
                                              QStringLiteral("Debug: windows fixed to the view, as RiftLinux's HDR test app places its quad."));
    const QCommandLineOption deskViewOption(QStringLiteral("xr-desk-view"),
                                            QStringLiteral("Desk check: the room laid out around the headset's view wherever it lies (controllers ignored)."));
    const QCommandLineOption stateDirOption(QStringLiteral("state-dir"),
                                            QStringLiteral("Folder for Rift Home's state (settings, window placements, logs; default: ~/.local/state/rift-home)."),
                                            QStringLiteral("dir"));
    const QCommandLineOption selfTestOption(QStringLiteral("xr-selftest"),
                                            QStringLiteral("Desk check: a scripted hand points, clicks, grabs, moves and scrolls."));
    const QCommandLineOption testClickOption(QStringLiteral("test-click"),
                                             QStringLiteral("Debug: after this many seconds, click the given point of every window (window pixels; default its centre). Repeatable."),
                                             QStringLiteral("seconds[:x,y]"));
    parser.addOptions({backendOption, preferredOption, runOption, logDirOption, isolateOption, withProfileOption, startRuntimeOption, sessionOption, dumpAfterOption, quitAfterOption, sizeOption,
                       verboseOption, xrOption, gapOption, distanceOption, ppmOption, cornerOption, themeOption, environmentOption, environmentAngleOption, viewLockedOption, deskViewOption, stateDirOption,
                       selfTestOption, testClickOption});
    const QCommandLineOption testTypeOption(QStringLiteral("test-type"),
                                            QStringLiteral("Debug: after this many seconds, type the text into the focused window (the virtual keyboard's keys)."),
                                            QStringLiteral("seconds:text"));
    const QCommandLineOption keyboardShowOption(QStringLiteral("keyboard-show"),
                                                QStringLiteral("The virtual keyboard shows by itself when a text field is focused (auto), or only from the window menu (button). Default: the settings file, else auto."),
                                                QStringLiteral("auto|button"));
    const QCommandLineOption restoreFirefoxOption(
        QStringLiteral("restore-firefox-profiles"),
        QStringLiteral("Only put the user's Firefox profiles (and Chromium-based browsers' title bar setting) back where Rift Home ended without doing so (run at desktop login), then exit."));
    parser.addOptions({testTypeOption, keyboardShowOption, restoreFirefoxOption});
    parser.process(app);
    if (parser.isSet(restoreFirefoxOption)) {
        // After a crash or power loss mid-session: before the desktop's own Firefox starts with Rift Home's preferences.
        const int waiting = RiftHome::App::FirefoxPrefs::restoreAll();
        if (waiting > 0) {
            qInfo("firefox: %d profile(s) still open in a Firefox: put back at Rift Home's next start", waiting);
        }
        if (const int chromium = RiftHome::App::Chromium::restoreAll(); chromium > 0) {
            qInfo("chromium: %d browser data folder(s) still open: put back at Rift Home's next start", chromium);
        }
        return 0;
    }

    // The apps' xdg-open (links, "open folder", Settings' Add your own) takes a KDE desktop without KDE_SESSION_VERSION
    // for KDE 3 and fails (kfmclient). Plasma's own environment has it; the console test and Rift Home's own session do
    // not. Plasma 6 is the Plasma of Qt 6, which Rift Home needs.
    if (qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(QLatin1Char(':')).contains(QLatin1String("KDE")) && qEnvironmentVariableIsEmpty("KDE_SESSION_VERSION")) {
        qputenv("KDE_SESSION_VERSION", "6");
    }

    Server::Options options;
    const QString backend = parser.value(backendOption);
    if (backend == QLatin1String("nested")) {
        options.backend = Server::Backend::Nested;
    } else if (backend == QLatin1String("drm")) {
        options.backend = Server::Backend::Drm; // its own session: from a console login or the login screen
    } else if (backend != QLatin1String("headless")) {
        std::fprintf(stderr, "unknown backend %s\n", qPrintable(backend));
        return 2;
    }
    const QString preferred = parser.value(preferredOption);
    if (preferred == QLatin1String("srgb")) {
        options.preferred = Server::Preferred::Srgb;
    } else if (preferred != QLatin1String("pq")) {
        std::fprintf(stderr, "unknown preferred description %s\n", qPrintable(preferred));
        return 2;
    }
    const QStringList size = parser.value(sizeOption).split(QLatin1Char('x'));
    if (size.size() == 2) {
        options.width = size[0].toInt();
        options.height = size[1].toInt();
    }
    options.verbose = parser.isSet(verboseOption);
    options.virtualPointer = parser.isSet(testClickOption);
    options.virtualKeyboard = parser.isSet(testTypeOption);

    const bool withProfile = parser.isSet(withProfileOption) || qEnvironmentVariableIntValue("RIFT_HOME_WITH_PROFILE") == 1;
    Server server(options);
    // The launcher's apps (read before the XR side starts; icons drawn here, on the GUI thread) and who starts them.
    std::vector<RiftHome::App::AppEntry> apps;
    RiftHome::App::AppLauncher *appLauncher = nullptr;
    std::function<void()> refreshXResources; //!< the X11 app size to XWayland again (set with the launcher)
    QString settingsCommand; //!< the Settings app, when built
    RiftHome::App::NotificationServer *notifications = nullptr; //!< the apps' notifications (to the headset with --xr)
    QProcess *runtime = nullptr; //!< --start-runtime: RiftLinux's runtime, ours to stop
    // The dock's volume: the default output through PipeWire's wpctl (the runtime makes the headset the default).
    std::function<void()> refreshVolume;
    const auto volumeCommand = [&refreshVolume](const QStringList &arguments) {
        auto *process = new QProcess;
        QObject::connect(process, &QProcess::finished, process, [process, &refreshVolume] {
            process->deleteLater();
            if (refreshVolume) {
                refreshVolume();
            }
        });
        process->start(QStringLiteral("wpctl"), arguments);
    };
    // Notifications: Rift Home serves them on its own session bus (rift-home-run.sh), the one
    // its apps use, so they reach the headset even nested in Plasma. Taken before any app starts; without the headset
    // they are only logged.
    notifications = new RiftHome::App::NotificationServer(
        [&apps](const QString &icon, const QString &appName, const QString &desktopEntry) {
            const QString name = icon.startsWith(QLatin1String("file://")) ? QUrl(icon).toLocalFile() : icon;
            if (!name.isEmpty() && (QFileInfo(name).isAbsolute() ? QFileInfo::exists(name) : QIcon::hasThemeIcon(name))) {
                return RiftHome::App::iconImage(name, appName, 128);
            }
            // The sender's app (its desktop entry, else its name) as Home knows it.
            for (const RiftHome::App::AppEntry &app : apps) {
                if ((!desktopEntry.isEmpty() && app.id == desktopEntry + QStringLiteral(".desktop"))
                    || (!appName.isEmpty() && app.name.compare(appName, Qt::CaseInsensitive) == 0)) {
                    return app.icon;
                }
            }
            return RiftHome::App::iconImage(QString(), appName, 128);
        },
        &server);
    notifications->registerOnBus();
    // The desktop's password stores on Rift Home's bus too, while nested: the ones the desktop's login unlocked, so no app
    // here meets a locked one asking for its password. Rift Home's own bus would
    // otherwise start its own, locked: flatpak Chromium on KDE goes through kwalletd6 to ksecretd's Secret Service
    // (org.kde.secretservicecompat), and flatpaks' keys through the Secret portal's backend (KDE: ksecretd's
    // org.freedesktop.impl.portal.desktop.kwallet; GNOME: gnome-keyring's org.freedesktop.impl.portal.Secret); apps such
    // as Proton VPN use the Secret Service (org.freedesktop.secrets; it found none and quit). Each is
    // forwarded on a connection of its own (several answer at the same paths, as /org/freedesktop/secrets), only when
    // the desktop runs it. Not kwalletd6 itself: Rift Home's bus starts its own, a front end to the forwarded store
    // (forwarded, its openAsync to the desktop's got no answer). In its own session the store is the
    // session's (rift-home-session-start unlocks it as Plasma does).
    if (const QString desktopBus = qEnvironmentVariable("RIFT_HOME_DESKTOP_BUS"); !desktopBus.isEmpty()) {
        const QDBusConnection desktop = QDBusConnection::connectToBus(desktopBus, QStringLiteral("desktop"));
        for (const char *service : {"org.freedesktop.secrets", "org.kde.secretservicecompat", "org.kde.ksecretd", "org.freedesktop.impl.portal.desktop.kwallet",
                                    "org.freedesktop.impl.portal.Secret"}) {
            const QString name = QLatin1String(service);
            const QDBusConnection local = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("rift-home-forward-") + name);
            auto *forwarder = new RiftHome::App::BusForwarder(name, QStringLiteral("/"), local, desktop, &server);
            if (!forwarder->start()) {
                delete forwarder;
                QDBusConnection::disconnectFromBus(QStringLiteral("rift-home-forward-") + name);
            }
        }
    }
    // The XR side: buffers go back to the Wayland thread through the event loop.
    std::unique_ptr<RiftHome::Xr::LayerBridge> bridge;
    if (parser.isSet(xrOption)) {
        bridge = std::make_unique<RiftHome::Xr::LayerBridge>([&server](uint64_t token) {
            QMetaObject::invokeMethod(&server, [&server, token] {
                server.releaseBuffer(token);
            }, Qt::QueuedConnection);
        });
        bridge->setInputHandler([&server, &apps, &appLauncher, &settingsCommand, &notifications, volumeCommand](const RiftHome::Xr::PointerEvent &event) {
            QMetaObject::invokeMethod(&server, [&server, &apps, &appLauncher, &settingsCommand, &notifications, volumeCommand, event] {
                using Type = RiftHome::Xr::PointerEvent::Type;
                const QString sink = QStringLiteral("@DEFAULT_AUDIO_SINK@");
                switch (event.type) {
                case Type::Launch:
                    if (appLauncher && event.button < apps.size()) {
                        appLauncher->launch(apps[event.button].command, {}, apps[event.button].id); // tagged: reopened next time
                    }
                    return;
                case Type::OpenSettings:
                    // A running Settings takes the page and this one ends (it is single-instance).
                    if (appLauncher && !settingsCommand.isEmpty()) {
                        appLauncher->launch(event.button == ~0u ? settingsCommand : QStringLiteral("%1 --page %2").arg(settingsCommand).arg(event.button));
                    }
                    return;
                case Type::VolumeDown:
                    volumeCommand({QStringLiteral("set-volume"), sink, QStringLiteral("5%-")});
                    return;
                case Type::VolumeUp:
                    volumeCommand({QStringLiteral("set-volume"), QStringLiteral("-l"), QStringLiteral("1.0"), sink, QStringLiteral("5%+")});
                    return;
                case Type::MuteToggle:
                    volumeCommand({QStringLiteral("set-mute"), sink, QStringLiteral("toggle")});
                    return;
                case Type::NotificationActivate:
                    if (notifications) {
                        notifications->activate(event.button);
                    }
                    return;
                case Type::NotificationAction:
                    if (notifications) {
                        notifications->invokeAction(event.button, event.index);
                    }
                    break;
                case Type::NotificationDismiss:
                    if (notifications) {
                        notifications->dismiss(event.button);
                    }
                    return;
                default:
                    server.handleXrInput(event);
                }
            }, Qt::QueuedConnection);
        });
        RiftHome::Xr::LayerBridge *status = bridge.get();
        refreshVolume = [status] {
            auto *process = new QProcess;
            QObject::connect(process, &QProcess::finished, process, [process, status] {
                // "Volume: 0.42" or "Volume: 0.42 [MUTED]"
                const QString out = QString::fromUtf8(process->readAllStandardOutput());
                const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("Volume:\\s*([0-9.]+)")).match(out);
                if (m.hasMatch()) {
                    status->setVolume(int(std::lround(m.captured(1).toDouble() * 100.0)), out.contains(QStringLiteral("MUTED")));
                }
                process->deleteLater();
            });
            process->start(QStringLiteral("wpctl"), {QStringLiteral("get-volume"), QStringLiteral("@DEFAULT_AUDIO_SINK@")});
        };
        auto *volumeTimer = new QTimer(&server);
        QObject::connect(volumeTimer, &QTimer::timeout, &server, [&refreshVolume] {
            refreshVolume();
        });
        volumeTimer->start(3000);
        refreshVolume();
        // The dock's network: wired or Wi-Fi connected (and how strong), else disconnected, from NetworkManager.
        const auto refreshNetwork = [status] {
            auto *devices = new QProcess;
            QObject::connect(devices, &QProcess::finished, devices, [devices, status](int code) {
                using Network = RiftHome::Xr::LayerBridge::Network;
                const QString out = QString::fromUtf8(devices->readAllStandardOutput());
                devices->deleteLater();
                if (code != 0) {
                    status->setNetwork(Network::Unknown, 0);
                    return;
                }
                const QStringList lines = out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
                // A VPN up (Proton VPN's WireGuard, an OpenVPN tunnel): the dock shows a lock.
                status->setVpn(lines.contains(QStringLiteral("wireguard:connected")) || lines.contains(QStringLiteral("tun:connected")));
                if (lines.contains(QStringLiteral("ethernet:connected"))) {
                    status->setNetwork(Network::Wired, 0);
                    return;
                }
                if (!lines.contains(QStringLiteral("wifi:connected"))) {
                    status->setNetwork(Network::Disconnected, 0);
                    return;
                }
                auto *wifi = new QProcess;
                QObject::connect(wifi, &QProcess::finished, wifi, [wifi, status] {
                    int signal = 0;
                    for (const QString &line : QString::fromUtf8(wifi->readAllStandardOutput()).split(QLatin1Char('\n'))) {
                        if (line.startsWith(QLatin1String("*:"))) {
                            signal = line.mid(2).toInt();
                        }
                    }
                    status->setNetwork(Network::Wifi, signal);
                    wifi->deleteLater();
                });
                wifi->start(QStringLiteral("nmcli"), {QStringLiteral("-t"), QStringLiteral("-f"), QStringLiteral("IN-USE,SIGNAL"), QStringLiteral("device"),
                                                      QStringLiteral("wifi"), QStringLiteral("list"), QStringLiteral("--rescan"), QStringLiteral("no")});
            });
            QProcessEnvironment untranslated = QProcessEnvironment::systemEnvironment();
            untranslated.insert(QStringLiteral("LC_ALL"), QStringLiteral("C")); // "connected", not its translation
            devices->setProcessEnvironment(untranslated);
            devices->start(QStringLiteral("nmcli"), {QStringLiteral("-t"), QStringLiteral("-f"), QStringLiteral("TYPE,STATE"), QStringLiteral("device")});
        };
        auto *networkTimer = new QTimer(&server);
        QObject::connect(networkTimer, &QTimer::timeout, &server, refreshNetwork);
        networkTimer->start(5000);
        refreshNetwork();
        server.setLayerBridge(bridge.get());
        QObject::connect(notifications, &RiftHome::App::NotificationServer::changed, &server, [status, &notifications] {
            std::vector<RiftHome::Xr::Notification> list;
            for (const RiftHome::App::NotificationEntry &e : notifications->entries()) {
                QStringList labels;
                for (const auto &[key, label] : RiftHome::App::NotificationStore::buttons(e)) {
                    labels << label;
                }
                list.push_back({e.id, e.app, e.summary, e.body, notifications->icon(e.id), e.urgency >= 2, labels});
            }
            status->publishNotifications(std::move(list), notifications->cardId());
        });
    }
    // The GPU (GpuProfile): the one the headset is plugged into, found by Rift Home itself on any machine. Rift Home
    // renders there (wlroots would take the first GPU, a laptop's integrated one), so apps draw and decode video there
    // too and their buffers never cross GPUs. Only for Rift Home's renderer: not passed on to the apps.
    const RiftHome::App::GpuProfile gpu = RiftHome::App::GpuProfile::detect();
    qInfo("%s", qPrintable(gpu.describe()));
    QByteArray gpuVariable;
    if (const RiftHome::App::GpuDevice *chosen = gpu.gpu()) {
        if (options.backend == Server::Backend::Headless && qEnvironmentVariableIsEmpty("WLR_RENDER_DRM_DEVICE")) {
            gpuVariable = "WLR_RENDER_DRM_DEVICE";
            qputenv(gpuVariable.constData(), chosen->renderNode.toLocal8Bit());
        } else if (options.backend == Server::Backend::Drm && gpu.devices.size() > 1 && qEnvironmentVariableIsEmpty("WLR_DRM_DEVICES")) {
            // wlroots renders on the first GPU listed; the others' monitors still work (through it).
            QStringList cards{chosen->card};
            for (const RiftHome::App::GpuDevice &device : gpu.devices) {
                if (device.card != chosen->card) {
                    cards << device.card;
                }
            }
            gpuVariable = "WLR_DRM_DEVICES";
            qputenv(gpuVariable.constData(), cards.join(QLatin1Char(':')).toLocal8Bit());
        }
    }
    const bool started = server.start();
    if (!gpuVariable.isEmpty()) {
        qunsetenv(gpuVariable.constData());
    }
    if (!started) {
        return 1;
    }
    std::unique_ptr<RiftHome::Xr::XrCompositor> xr;
    if (bridge) {
        RiftHome::Xr::XrCompositor::Options xrOptions;
        xrOptions.gapDegrees = parser.value(gapOption).toFloat();
        xrOptions.distance = parser.value(distanceOption).toFloat();
        xrOptions.pixelsPerMeter = parser.value(ppmOption).toFloat();
        xrOptions.cornerRadius = parser.value(cornerOption).toFloat();
        xrOptions.environment = parser.value(environmentOption).isEmpty() ? defaultRoomImage() : QFileInfo(parser.value(environmentOption)).absoluteFilePath();
        xrOptions.environmentAngle = parser.value(environmentAngleOption).toFloat();
        const QString theme = parser.value(themeOption);
        if (theme == QLatin1String("light")) {
            xrOptions.frameTheme = RiftHome::Xr::FrameArt::Theme::Light;
        } else if (theme != QLatin1String("dark")) {
            std::fprintf(stderr, "unknown frame theme %s (dark or light)\n", qPrintable(theme));
            return 2;
        }
        xrOptions.viewLocked = parser.isSet(viewLockedOption);
        xrOptions.deskView = parser.isSet(deskViewOption);
        xrOptions.selfTest = parser.isSet(selfTestOption);
        const QString stateDir = parser.isSet(stateDirOption) ? parser.value(stateDirOption) : defaultStateDir();
        xrOptions.stateFile = stateDir + QStringLiteral("/window-placements.json");
        // The controllers' models (ControllerLeft.glb and ControllerRight.glb): a folder of
        // one's choice, the state's, the user's and the system's data, then the work tree's data/controllers.
        if (const QString dir = qEnvironmentVariable("RIFT_HOME_CONTROLLER_MODELS"); !dir.isEmpty()) {
            xrOptions.controllerModelDirs << dir;
        }
        xrOptions.controllerModelDirs << stateDir + QStringLiteral("/controllers");
        xrOptions.controllerModelDirs << QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, QStringLiteral("rift-home/controllers"),
                                                                   QStandardPaths::LocateDirectory);
        xrOptions.controllerModelDirs << QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../data/controllers"));
        // Rift Home's settings (the Settings app changes them): <state>/settings.json. Missing ones are
        // written in from the command line's (or the defaults) so they can be seen and changed; after that the file wins.
        settingsFile = stateDir + QStringLiteral("/settings.json");
        QJsonObject settings;
        if (QFile file(settingsFile); file.open(QIODevice::ReadOnly)) {
            settings = QJsonDocument::fromJson(file.readAll()).object();
        }
        bool added = false;
        const auto seed = [&](const QString &group, const QString &key, const QJsonValue &value) {
            QJsonObject g = settings.value(group).toObject();
            if (!g.contains(key)) {
                g.insert(key, value);
                settings.insert(group, g);
                added = true;
            }
        };
        seed(QStringLiteral("keyboard"), QStringLiteral("showOnTextField"), true);
        seed(QStringLiteral("frame"), QStringLiteral("theme"), xrOptions.frameTheme == RiftHome::Xr::FrameArt::Theme::Light ? QStringLiteral("light") : QStringLiteral("dark"));
        seed(QStringLiteral("room"), QStringLiteral("photo"), xrOptions.environment);
        seed(QStringLiteral("room"), QStringLiteral("brightness"), 1.0);
        seed(QStringLiteral("clock"), QStringLiteral("use24Hour"), true);
        if (added) {
            QDir().mkpath(stateDir);
            QSaveFile file(settingsFile);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(QJsonDocument(settings).toJson());
                file.commit();
            }
        }
        const RiftHome::Xr::XrCompositor::LiveSettings live = liveSettings(settings);
        server.setAppScale(appScaleOf(settings));
        // The runtime's picture as it starts with Rift Home (rift-home-start.sh and the runtime's start below read the same).
        xrOptions.pictureQuality = std::clamp(settings.value(QStringLiteral("display")).toObject().value(QStringLiteral("composition")).toInt(165), 165, 200);
        const QString defaultRoom = xrOptions.environment; // --xr-environment: offered in Settings whatever room is chosen
        xrOptions.keyboardShowOnTextField = live.keyboardShowOnTextField;
        xrOptions.frameTheme = live.frameTheme;
        xrOptions.environment = live.environment;
        // A chosen room whose file has gone (deleted, moved, an uninstalled wallpaper): the default room, not darkness.
        // "None" (no room) is an empty path and stays dark.
        if (!xrOptions.environment.isEmpty() && !QFileInfo::exists(xrOptions.environment)) {
            qWarning("room %s is no longer there: the default room %s instead", qPrintable(xrOptions.environment), qPrintable(defaultRoom));
            xrOptions.environment = defaultRoom;
        }
        xrOptions.roomBrightness = live.roomBrightness;
        xrOptions.cinemaRoomLight = live.cinemaRoomLight;
        xrOptions.roomTurnDegrees = live.roomTurnDegrees;
        xrOptions.pointerColour = live.pointerColour;
        xrOptions.pointerWidth = live.pointerWidth;
        xrOptions.pointerHideSeconds = live.pointerHideSeconds;
        xrOptions.use24Hour = live.use24Hour;
        xrOptions.windowCurve = live.windowCurve;
        xrOptions.buttons = live.buttons;
        // The Settings app finds Rift Home's settings and Rift Home itself (to leave it) through these.
        qputenv("RIFT_HOME_STATE_DIR", QFileInfo(stateDir).absoluteFilePath().toUtf8());
        qputenv("RIFT_HOME_PID", QByteArray::number(QCoreApplication::applicationPid()));
        // Rooms Settings offers. Rift Home's own: the default room's folder (the photo rooms: once an
        // HDR room was chosen the living-room photo was no longer offered) and ownRoomDirs() (the HDR panoramas,
        // installed or the work tree's). The user's: a folder of their own, Pictures/RiftHome (as desktops have for
        // wallpapers, not all of Pictures); from here, as the apps' XDG folders may be Rift Home's own.
        QStringList rooms = ownRoomDirs();
        if (!defaultRoom.isEmpty()) {
            rooms.prepend(QFileInfo(defaultRoom).absolutePath());
        }
        rooms.removeDuplicates();
        qputenv("RIFT_HOME_ROOMS", rooms.join(QLatin1Char(':')).toUtf8());
        qputenv("RIFT_HOME_USER_ROOMS", (QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + QStringLiteral("/RiftHome")).toUtf8());
        // And the desktop's wallpapers, wherever Plasma (wallpapers) and GNOME (backgrounds) keep them.
        QStringList wallpapers;
        for (const QString &kind : {QStringLiteral("wallpapers"), QStringLiteral("backgrounds")}) {
            wallpapers << QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, kind, QStandardPaths::LocateDirectory);
        }
        qputenv("RIFT_HOME_WALLPAPER_DIRS", wallpapers.join(QLatin1Char(':')).toUtf8());
        // The launcher: favourites first (settings "launcher.favourites", desktop file ids), then every app by name.
        QStringList favourites{QStringLiteral("firefox.desktop"), QStringLiteral("org.kde.dolphin.desktop"),
                               QStringLiteral("org.kde.konsole.desktop"), QStringLiteral("mpv.desktop")};
        const QJsonObject launcherSettings = settings.value(QStringLiteral("launcher")).toObject();
        if (launcherSettings.contains(QStringLiteral("favourites"))) {
            favourites.clear();
            for (const QJsonValue &v : launcherSettings.value(QStringLiteral("favourites")).toArray()) {
                favourites << v.toString();
            }
        }
        apps = RiftHome::App::loadApps(favourites, 128);
        // Settings first in Home (as visionOS's), when it was built.
        const QString settingsApp = QCoreApplication::applicationDirPath() + QStringLiteral("/rift-home-settings");
        if (QFileInfo::exists(settingsApp)) {
            apps.insert(apps.begin(), {QStringLiteral("org.rifthome.settings.desktop"), QStringLiteral("Settings"), settingsApp, QString(),
                                       RiftHome::App::settingsIcon(128)});
            xrOptions.settingsApp = 0;
            settingsCommand = settingsApp;
        }
        for (const RiftHome::App::AppEntry &app : apps) {
            xrOptions.apps.push_back({app.name, app.icon, QString(app.id).remove(QStringLiteral(".desktop"))});
        }
        std::printf("launcher: %zu app(s)\n", apps.size());
        if (parser.isSet(keyboardShowOption)) {
            xrOptions.keyboardShowOnTextField = parser.value(keyboardShowOption) != QLatin1String("button");
        }
        xr = std::make_unique<RiftHome::Xr::XrCompositor>(*bridge, xrOptions);
        // Settings records a controller binding through Rift Home (it cannot see the buttons).
        auto *controls = new RiftHome::App::ControlsService([&xr] { if (xr) xr->startBindingRecording(); }, [&xr] { if (xr) xr->cancelBindingRecording(); }, &server);
        xr->setBindingRecordedHandler([controls](const QString &binding) { controls->recorded(binding); });
        // And shows the lens test (Display & Room) in the headset. Its object first: the controls take the bus name.
        auto *headset = new RiftHome::App::HeadsetService([&xr] { if (xr) xr->showLensTest(); }, &server);
        headset->registerOnBus();
        controls->registerOnBus();
        if (parser.isSet(startRuntimeOption)) {
            // Its own session: RiftLinux's runtime started now that Rift Home serves, pointed at it in
            // Wayland direct mode only (it leases the headset from Rift Home; never an ordinary window inside it), then
            // the headset side once the runtime's socket is there. It stops with Rift Home.
            const QString socket = qEnvironmentVariable("XDG_RUNTIME_DIR") + QStringLiteral("/monado_comp_ipc");
            // The headset side talks to RiftLinux's runtime (as riftlinux-run sets it), not whichever is the default.
            if (qEnvironmentVariableIsEmpty("XR_RUNTIME_JSON")) {
                const QString native = qEnvironmentVariable("RIFTLINUX_NATIVE_DIR", QStringLiteral("/usr/lib/riftlinux/native"));
                qputenv("XR_RUNTIME_JSON", (native + QStringLiteral("/openxr_riftlinux.json")).toUtf8());
            }
            auto steamvr = std::make_shared<bool>(false); //!< SteamVR has the headset
            auto startRuntime = std::make_shared<std::function<void()>>();
            *startRuntime = [&server, &xr, &runtime, socket, steamvr] {
                runtime = new QProcess(&server);
                QProcess *process = runtime;
                QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
                env.insert(QStringLiteral("WAYLAND_DISPLAY"), server.socketName());
                env.remove(QStringLiteral("DISPLAY"));
                env.insert(QStringLiteral("XRT_COMPOSITOR_FORCE_WAYLAND_DIRECT"), QStringLiteral("1"));
                // Picture quality (Settings, display.composition): the runtime's picture size,
                // percent of the panel's pixels a side, as its scratch headroom over the CV1's 1:1 size (1.3216 the panel's).
                // Also in the user manager's environment for its start, in case it runs itself as the user's service.
                QFile file(settingsFile);
                const int composition = file.open(QIODevice::ReadOnly)
                    ? QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("display")).toObject().value(QStringLiteral("composition")).toInt(165)
                    : 165;
                if (composition > 165) {
                    const QString headroom = QString::number(std::min(composition, 200) * 10000 / 13216);
                    env.insert(QStringLiteral("XRT_COMPOSITOR_SCRATCH_HEADROOM"), headroom);
                    QProcess::execute(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("set-environment"),
                                                                    QStringLiteral("XRT_COMPOSITOR_SCRATCH_HEADROOM=") + headroom});
                    QTimer::singleShot(20000, &server, [] {
                        QProcess::startDetached(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("unset-environment"),
                                                                             QStringLiteral("XRT_COMPOSITOR_SCRATCH_HEADROOM")});
                    });
                    qInfo("runtime: picture quality %d %% (scratch headroom %s %%)", composition, qPrintable(headroom));
                }
                process->setProcessEnvironment(env);
                process->setProcessChannelMode(QProcess::ForwardedChannels);
                QObject::connect(process, &QProcess::finished, &server, [&server, &runtime, process, steamvr](int code) {
                    qWarning("runtime: riftlinux-runtime ended (%d)", code);
                    if (!*steamvr) {
                        server.setRuntimeState(QStringLiteral("Stopped"), false);
                    }
                    if (runtime == process) {
                        runtime = nullptr;
                    }
                    process->deleteLater();
                });
                server.setRuntimeState(QStringLiteral("Starting"), false);
                process->start(QStringLiteral("riftlinux-runtime"), {});
                auto *waiting = new QTimer(&server);
                auto tries = std::make_shared<int>(0);
                QObject::connect(waiting, &QTimer::timeout, &server, [waiting, tries, socket, &server, &xr, steamvr] {
                    ++*tries;
                    if (*steamvr) {
                        waiting->deleteLater(); // SteamVR came first meanwhile
                    } else if (*tries >= 6 && QFileInfo::exists(socket)) { // at least 1.5 s: not a socket left from before
                        waiting->deleteLater();
                        qInfo("runtime: up (%s); the headset side starts", qPrintable(socket));
                        server.setRuntimeState(QStringLiteral("Running"), true);
                        xr->start();
                    } else if (*tries > 120) {
                        waiting->deleteLater();
                        qWarning("runtime: no socket after 30 s");
                        server.setRuntimeState(QStringLiteral("Did not start"), false);
                    }
                });
                waiting->start(250);
            };
            (*startRuntime)();
            // SteamVR: when it starts, Rift Home leaves it the headset (its session and the runtime end;
            // the apps keep running; SteamVR's driver leases the headset from Rift Home); when it ends, Rift Home comes
            // back.
            auto *steamvrWatch = new QTimer(&server);
            QObject::connect(steamvrWatch, &QTimer::timeout, &server, [&server, &xr, &runtime, steamvr, startRuntime] {
                const bool running = processRunning(QByteArrayLiteral("vrserver"));
                if (running && !*steamvr) {
                    *steamvr = true;
                    qInfo("SteamVR started: Rift Home leaves it the headset");
                    server.setRuntimeState(QStringLiteral("SteamVR has the headset"), true);
                    xr->stop();
                    if (runtime) {
                        runtime->terminate();
                    }
                } else if (!running && *steamvr) {
                    *steamvr = false;
                    qInfo("SteamVR ended: Rift Home takes the headset back");
                    (*startRuntime)();
                }
            });
            steamvrWatch->start(1000);        } else {
            // Closed from the Rift panel (or the runtime gone): Rift Home quits with its apps, as when left from Settings.
            xr->setRuntimeClosedHandler([&server] {
                emit server.quitRequested();
            });
            xr->start();
            // Inside a desktop the runtime is started around Rift Home (riftlinux-run): SteamVR cannot have the headset
            // meanwhile. Said in the headset when SteamVR starts, instead of SteamVR just failing.
            auto *steamvrWatch = new QTimer(&server);
            auto seen = std::make_shared<bool>(false);
            QObject::connect(steamvrWatch, &QTimer::timeout, &server, [seen, &notifications] {
                const bool running = processRunning(QByteArrayLiteral("vrserver"));
                if (running && !*seen && notifications) {
                    notifications->Notify(QStringLiteral("Rift Home"), 0, QString(), QStringLiteral("SteamVR needs the headset"),
                                          QStringLiteral("Rift Home has it now. Leave Rift Home (Settings, Power), then start SteamVR again."),
                                          {}, {{QStringLiteral("urgency"), 2}}, -1);
                }
                *seen = running;
            });
            steamvrWatch->start(2000);
        }
        // Changes to the settings file (the Settings app writes it whole) apply at once.
        auto *watcher = new QFileSystemWatcher({QFileInfo(settingsFile).absolutePath(), settingsFile}, &server);
        auto *debounce = new QTimer(&server);
        debounce->setSingleShot(true);
        debounce->setInterval(150);
        const auto reload = [debounce] {
            debounce->start();
        };
        QObject::connect(watcher, &QFileSystemWatcher::directoryChanged, &server, reload);
        QObject::connect(watcher, &QFileSystemWatcher::fileChanged, &server, reload);
        auto applied = std::make_shared<QJsonObject>(settings);
        QObject::connect(debounce, &QTimer::timeout, &server, [watcher, applied, &server, &xr, &appLauncher, &withProfile, &refreshXResources] {
            if (!watcher->files().contains(settingsFile) && QFileInfo::exists(settingsFile)) {
                watcher->addPath(settingsFile); // replaced by a new file: follow it again
            }
            // The folder also holds the window placements (saved often): only a changed settings file counts.
            QFile file(settingsFile);
            if (!xr || !file.open(QIODevice::ReadOnly)) {
                return;
            }
            const QJsonObject now = QJsonDocument::fromJson(file.readAll()).object();
            if (now != *applied) {
                *applied = now;
                xr->applySettings(liveSettings(now));
                server.setAppScale(appScaleOf(now));
                if (appLauncher) {
                    appLauncher->setAppScale(server.options().appScale, server.scalesPerSurface());
                    appLauncher->setOwnData(withProfile || now.value(QStringLiteral("apps")).toObject().value(QStringLiteral("ownData")).toBool(true));
                    const double x11Scale = now.value(QStringLiteral("apps")).toObject().value(QStringLiteral("x11Scale")).toDouble(2.0);
                    if (std::abs(x11Scale - appLauncher->x11Scale()) > 0.001) {
                        appLauncher->setX11Scale(x11Scale);
                        if (refreshXResources) {
                            refreshXResources();
                        }
                    }
                }
            }
        });
    }
    std::printf("RIFT_HOME_SOCKET=%s\n", qPrintable(server.socketName()));
    std::fflush(stdout);

    QString logDir = parser.value(logDirOption);
    if (logDir.isEmpty()) {
        const QString state = parser.isSet(stateDirOption) ? parser.value(stateDirOption) : defaultStateDir();
        logDir = state + QStringLiteral("/logs/") + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    }
    QString isolatedXdg = parser.value(isolateOption);
    if (isolatedXdg.isEmpty() && !runsFromWorkTree()) {
        isolatedXdg = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/rift-home/apps");
    }
    RiftHome::App::AppLauncher launcher({server.socketName(), logDir, isolatedXdg});
    appLauncher = &launcher;
    launcher.setX11Display(server.x11Display());
    launcher.setGpuProfile(gpu);
    launcher.setAppScale(server.options().appScale, server.scalesPerSurface());
    // What the GPU's best path lacks here (a package, a kernel setting): said once in the headset, where it is missed.
    if (const QString missing = gpu.missing(); !missing.isEmpty()) {
        qWarning("GPU: %s", qPrintable(missing));
        if (notifications) {
            QTimer::singleShot(8000, notifications, [notifications, missing] {
                notifications->Notify(QStringLiteral("Rift Home"), 0, QString(), QStringLiteral("Something to install"), missing, {}, {}, 0);
            });
        }
    }
    // Toolkits reading Xft.dpi (Qt, Chromium, GTK 2 on X11) draw X11 apps at their size here (Settings: Older apps; Steam
    // reads its own switch, AppLauncher): set when XWayland is up (it starts with the first X11 app; xrdb would start
    // it too) and again when the size changes while it runs.
    auto x11Up = std::make_shared<bool>(false);
    refreshXResources = [&server, &launcher, x11Up] {
        if (!*x11Up) {
            return;
        }
        auto *xrdb = new QProcess(&launcher);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("DISPLAY"), server.x11Display());
        xrdb->setProcessEnvironment(env);
        QObject::connect(xrdb, &QProcess::finished, xrdb, &QObject::deleteLater);
        QObject::connect(xrdb, &QProcess::errorOccurred, xrdb, &QObject::deleteLater);
        xrdb->start(QStringLiteral("xrdb"), {QStringLiteral("-merge")});
        // Grayscale antialiasing and slight hinting, as for the Wayland apps (AppLauncher::writeFontConfig).
        xrdb->write(QStringLiteral("Xft.dpi: %1\nXft.antialias: 1\nXft.rgba: none\nXft.lcdfilter: lcdnone\nXft.hinting: 1\nXft.hintstyle: hintslight\n")
                        .arg(qRound(96.0 * launcher.x11Scale()))
                        .toUtf8());
        xrdb->closeWriteChannel();
        qInfo("X11: Xft.dpi %d for X11 apps started from now on", int(qRound(96.0 * launcher.x11Scale())));
    };
    QObject::connect(&server, &Server::x11Ready, &launcher, [x11Up, &refreshXResources] {
        *x11Up = true;
        refreshXResources();
    });
    // The monitor page's Terminal (its own session): a terminal on the monitor, not in the headset, for commands.
    QObject::connect(&server, &Server::terminalRequested, &server, [&launcher, &server] {
        for (const QString &terminal : {QStringLiteral("konsole --separate"), QStringLiteral("foot"), QStringLiteral("alacritty"),
                                        QStringLiteral("kitty"), QStringLiteral("xterm")}) {
            if (!QStandardPaths::findExecutable(terminal.section(QLatin1Char(' '), 0, 0)).isEmpty()) {
                launcher.launch(terminal, [&server](qint64 pid) {
                    server.addMonitorProcess(pid);
                });
                return;
            }
        }
        qWarning("monitor: no terminal found (konsole, foot, alacritty, kitty, xterm)");
    });
    if (!parser.isSet(sessionOption) && !qEnvironmentVariableIsEmpty("RIFT_HOME_DESKTOP_BUS")) {
        // Rift Home's own bus (rift-home-run.sh): services it starts on demand (the file dialogs' portal, ...) open on Rift
        // Home's display, not nowhere (the KDE portal started without one, failed and was retried).
        QStringList vars{QStringLiteral("WAYLAND_DISPLAY=") + server.socketName(), QStringLiteral("XDG_SESSION_TYPE=wayland")};
        if (!server.x11Display().isEmpty()) {
            vars << QStringLiteral("DISPLAY=") + server.x11Display();
        }
        QProcess::execute(QStringLiteral("dbus-update-activation-environment"), vars);
        qInfo("own bus: services started on demand open on %s", qPrintable(server.socketName()));
    }
    if (parser.isSet(sessionOption) && !settingsCommand.isEmpty()) {
        // The session's password agent: Settings waiting without a window; a prompt shows it in front.
        launcher.launch(settingsCommand + QStringLiteral(" --session-agent"));
    }
    if (parser.isSet(sessionOption)) {
        // Rift Home is the login session: programs started by D-Bus or the service manager (portals, the password store)
        // reach its display. Never while a desktop runs (they would leave the desktop's).
        qputenv("WAYLAND_DISPLAY", server.socketName().toUtf8());
        if (!server.x11Display().isEmpty()) {
            qputenv("DISPLAY", server.x11Display().toUtf8());
        }
        qputenv("XDG_SESSION_TYPE", "wayland");
        const QStringList names{QStringLiteral("WAYLAND_DISPLAY"), QStringLiteral("DISPLAY"), QStringLiteral("XDG_SESSION_TYPE")};
        // The services' desktop name: the portals' (RIFT_HOME_PORTAL_DESKTOP, for rifthome-portals.conf), while Rift Home
        // and its apps keep KDE's (Qt takes its KDE theme only for exactly "KDE": the user's colours, icons and dialogs).
        const QString portalDesktop = qEnvironmentVariable("RIFT_HOME_PORTAL_DESKTOP", qEnvironmentVariable("XDG_CURRENT_DESKTOP"));
        const QString desktopName = QStringLiteral("XDG_CURRENT_DESKTOP=") + portalDesktop;
        QProcess::execute(QStringLiteral("systemctl"), QStringList{QStringLiteral("--user"), QStringLiteral("import-environment")} + names);
        QProcess::execute(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("set-environment"), desktopName});
        QProcess::execute(QStringLiteral("dbus-update-activation-environment"), QStringList{QStringLiteral("--systemd")} + names + QStringList{desktopName});
        // KDE's wallet, opened with the login's password: the login screen's PAM module started it and waits for the
        // session's environment, which Plasma hands it once its display runs (pam_kwallet_init). The same here, now that
        // Rift Home's display exists: without a display the wallet cannot start, and the first app to ask for a password
        // starts another one, locked, which asks for the wallet's password.
        if (!qEnvironmentVariableIsEmpty("PAM_KWALLET5_LOGIN")) {
            QStringList candidates;
            if (QFile entry(QStringLiteral("/etc/xdg/autostart/pam_kwallet_init.desktop")); entry.open(QIODevice::ReadOnly)) {
                for (const QByteArray &line : entry.readAll().split('\n')) {
                    if (line.startsWith("Exec=")) {
                        candidates << QString::fromUtf8(line.mid(5)).trimmed();
                        break;
                    }
                }
            }
            candidates << QStringLiteral("/usr/lib/pam_kwallet_init") << QStringLiteral("/usr/libexec/pam_kwallet_init")
                       << QStringLiteral("/usr/lib/x86_64-linux-gnu/libexec/pam_kwallet_init");
            QString init;
            for (const QString &candidate : std::as_const(candidates)) {
                if (QFileInfo(candidate).isExecutable()) {
                    init = candidate;
                    break;
                }
            }
            if (init.isEmpty()) {
                qWarning("session: no pam_kwallet_init: KDE's wallet stays locked and asks for its password");
            } else {
                const int status = QProcess::execute(init, {});
                qInfo("session: %s handed this session's environment to KDE's wallet (status %d)", qPrintable(init), status);
            }
        } else {
            qInfo("session: no PAM_KWALLET5_LOGIN: the login did not open KDE's wallet");
        }
        // The portals of an earlier session (they outlive it in the user's service manager) keep its display and desktop
        // name: the front end starts again with this session's, the back ends start on demand with it.
        QProcess::execute(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("try-restart"), QStringLiteral("xdg-desktop-portal.service")});
        QProcess::execute(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("stop"), QStringLiteral("xdg-desktop-portal-gtk.service"),
                                                        QStringLiteral("xdg-desktop-portal-kde.service"), QStringLiteral("plasma-xdg-desktop-portal-kde.service")});
        qInfo("session: display %s handed to the user's services", qPrintable(server.socketName()));
    }
    {
        // The user's own app data: asked at start (--with-profile, RIFT_HOME_WITH_PROFILE=1) or in Settings (apps.ownData,
        // on unless turned off: Firefox with the user's bookmarks and logins).
        QFile file(settingsFile);
        const QJsonObject saved = file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
        launcher.setOwnData(withProfile || saved.value(QStringLiteral("apps")).toObject().value(QStringLiteral("ownData")).toBool(true));
        launcher.setX11Scale(saved.value(QStringLiteral("apps")).toObject().value(QStringLiteral("x11Scale")).toDouble(2.0));
        // The apps opened from Home that were still open when Rift Home last ended, opened again
        // (Settings -> Power -> "Reopen my apps", on unless turned off).
        if (!settingsFile.isEmpty() && saved.value(QStringLiteral("apps")).toObject().value(QStringLiteral("reopen")).toBool(true)) {
            QFile reopen(QFileInfo(settingsFile).absolutePath() + QStringLiteral("/reopen.json"));
            const QJsonArray ids = reopen.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(reopen.readAll()).object().value(QStringLiteral("apps")).toArray()
                                                                    : QJsonArray();
            QTimer::singleShot(1500, &launcher, [&launcher, &apps, ids] {
                for (const QJsonValue &id : ids) {
                    for (const RiftHome::App::AppEntry &app : apps) {
                        if (app.id == id.toString()) {
                            qInfo("reopening %s (open when Rift Home last ended)", qPrintable(app.name));
                            launcher.launch(app.command, {}, app.id);
                        }
                    }
                }
            });
        }
    }
    const QStringList commands = parser.values(runOption);
    QTimer::singleShot(0, &launcher, [&launcher, commands] {
        for (const QString &command : commands) {
            launcher.launch(command);
        }
    });

    // Programs are asked to quit while the server still serves them, then the server stops.
    bool stopping = false;
    const auto stop = [&] {
        if (stopping) {
            return;
        }
        stopping = true;
        // The apps opened from Home that are still open: reopened next time (before they are asked to quit).
        if (!settingsFile.isEmpty()) {
            QJsonArray ids;
            for (const QString &tag : launcher.runningTags()) {
                ids.append(tag);
            }
            QSaveFile reopen(QFileInfo(settingsFile).absolutePath() + QStringLiteral("/reopen.json"));
            if (reopen.open(QIODevice::WriteOnly)) {
                reopen.write(QJsonDocument(QJsonObject{{QStringLiteral("apps"), ids}}).toJson());
                reopen.commit();
            }
        }
        if (runtime && runtime->state() != QProcess::NotRunning) {
            runtime->terminate(); // riftlinux-runtime stops the service it started
        }
        launcher.shutdown(5000, [&app] {
            app.quit();
        });
    };
    // Debug (an AddressSanitizer build only): the live heap by where it was allocated, at these seconds ("60,170").
    if (__sanitizer_print_memory_profile) {
        for (const QString &at : qEnvironmentVariable("RIFT_HOME_HEAP_PROFILE_S").split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            if (const int seconds = at.toInt(); seconds > 0) {
                QTimer::singleShot(seconds * 1000, &server, [] {
                    __sanitizer_print_memory_profile(95, 40);
                });
            }
        }
    }
    if (parser.isSet(testTypeOption)) {
        // The virtual keyboard's path without the headset: each character as the keys the keyboard would send.
        const QString spec = parser.value(testTypeOption);
        const int colon = spec.indexOf(QLatin1Char(':'));
        const int seconds = spec.left(colon).toInt();
        const QString text = spec.mid(colon + 1);
        QTimer::singleShot(seconds * 1000, &server, [&server, text] {
            const RiftHome::Spatial::VirtualKeyboard keyboard;
            const auto key = [&server](uint32_t code, bool pressed) {
                RiftHome::Xr::PointerEvent e;
                e.type = RiftHome::Xr::PointerEvent::Type::Key;
                e.button = code;
                e.pressed = pressed;
                server.handleXrInput(e);
            };
            for (const QChar c : text) {
                const auto k = keyboard.keyForChar(c);
                if (!k) {
                    std::fprintf(stderr, "test-type: no key for %s\n", qPrintable(QString(c)));
                    continue;
                }
                if (k->second) {
                    key(KEY_LEFTSHIFT, true);
                }
                key(k->first, true);
                key(k->first, false);
                if (k->second) {
                    key(KEY_LEFTSHIFT, false);
                }
            }
        });
    }
    // Window pictures only, no click (an app where a stray click would do something: Steam).
    for (const QString &when : parser.values(dumpAfterOption)) {
        const QString dumpDir = parser.value(logDirOption).isEmpty() ? QStringLiteral("run/dumps") : parser.value(logDirOption);
        QTimer::singleShot(int(when.toDouble() * 1000), &server, [&server, dumpDir, when] {
            server.dumpWindows(dumpDir, QStringLiteral("t") + when);
        });
    }
    // Repeatable: each click at its time (a dialog opened by one, a button in it by the next), window dumps around each.
    const QStringList testClicks = parser.values(testClickOption);
    for (int n = 0; n < testClicks.size(); ++n) {
        // The Wayland side of the controllers' input without the headset: motion, press, release on every window.
        const QStringList parts = testClicks[n].split(QLatin1Char(':'));
        const double seconds = parts.value(0).toDouble();
        const QStringList point = parts.value(1).split(QLatin1Char(','));
        const QString dumpDir = parser.value(logDirOption).isEmpty() ? QStringLiteral("run/dumps") : parser.value(logDirOption);
        const QString suffix = n == 0 ? QString() : QString::number(n + 1);
        QTimer::singleShot(int(seconds * 1000) + 1500, &server, [&server, dumpDir, suffix] {
            server.dumpWindows(dumpDir, QStringLiteral("after") + suffix);
        });
        QTimer::singleShot(int(seconds * 1000), &server, [&server, point, dumpDir, suffix, seconds] {
            server.dumpWindows(dumpDir, QStringLiteral("before") + suffix);
            // Debug: event times on another clock's base, as the XR side's were (RIFT_TEST_CLICK_TIME_BASE_MS).
            const uint32_t timeBase = uint32_t(qEnvironmentVariableIntValue("RIFT_TEST_CLICK_TIME_BASE_MS"));
            for (const auto &[id, width, height, scale] : server.mappedWindowSizes()) {
                RiftHome::Xr::PointerEvent e;
                e.timeMs = timeBase + uint32_t(seconds * 1000.0); // later clicks later (a double click is told by time)
                e.windowId = id;
                e.x = point.size() == 2 ? point[0].toDouble() * scale : width / 2.0; // a point in the app's own units
                e.y = point.size() == 2 ? point[1].toDouble() * scale : height / 2.0;
                e.type = RiftHome::Xr::PointerEvent::Type::Motion;
                server.handleXrInput(e);
                e.type = RiftHome::Xr::PointerEvent::Type::Button;
                e.button = 0x110; // BTN_LEFT
                e.pressed = true;
                server.handleXrInput(e);
                e.pressed = false;
                e.timeMs += 80;
                server.handleXrInput(e);
            }
        });
    }
    QObject::connect(&server, &Server::quitRequested, &app, stop);
    if (const int seconds = parser.value(quitAfterOption).toInt(); seconds > 0) {
        QTimer::singleShot(seconds * 1000, &app, [stop] {
            qInfo("quit-after reached - stopping");
            stop();
        });
    }

    const int result = app.exec();
    launcher.terminateAll();
    // The XR thread first, then the handoff; the server unlocks whatever is still locked.
    xr.reset();
    server.setLayerBridge(nullptr);
    bridge.reset();
    endOwnBusServices();
    return result;
}
