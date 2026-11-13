// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "app/AppLauncher.hpp"
#include "app/Chromium.hpp"
#include "app/Command.hpp"
#include "app/FirefoxPrefs.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QLoggingCategory>
#include <QProcess>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <memory>
#include <sys/resource.h>
#include <utility>

Q_LOGGING_CATEGORY(lcApp, "rifthome.app")

namespace RiftHome::App
{

namespace
{

rlimit g_appFileLimit{}; //!< the open-file limit programs started here get (Rift Home's own before raiseFileLimit)
bool g_fileLimitRaised = false;

//! Appends @p text to @p path unless the file already mentions @p key (the user's own value wins).
void seed(const QString &path, const QString &key, const QString &text)
{
    QFile file(path);
    if (file.open(QIODevice::ReadOnly) && file.readAll().contains(key.toUtf8())) {
        return;
    }
    file.close();
    if (file.open(QIODevice::Append | QIODevice::Text)) {
        file.write(text.toUtf8());
        qCInfo(lcApp).noquote() << "app settings:" << key << "set in" << path;
    }
}

} // namespace

AppLauncher::AppLauncher(const Options &options, QObject *parent)
    : QObject(parent)
    , m_options(options)
{
    if (!m_options.isolatedXdgDir.isEmpty()) {
        // A double click with a controller's trigger is slower than a mouse's (500-850 ms between the
        // presses, so Dolphin saw two clicks and renamed the folder): 800 ms in Rift Home's own app settings, for KDE
        // and Qt apps (kdeglobals) and GTK ones (Firefox). Never in the user's own settings.
        const QString config = m_options.isolatedXdgDir + QStringLiteral("/config");
        QDir().mkpath(config + QStringLiteral("/gtk-3.0"));
        seed(config + QStringLiteral("/kdeglobals"), QStringLiteral("DoubleClickInterval"), QStringLiteral("\n[KDE]\nDoubleClickInterval=800\n"));
        seed(config + QStringLiteral("/gtk-3.0/settings.ini"), QStringLiteral("gtk-double-click-time"),
             QStringLiteral("[Settings]\ngtk-double-click-time=800\n"));
    }
    prepareVrApps();
    writeFontConfig();
    // The user's own Firefox profiles Rift Home did not put back last time (it ended without its usual exit).
    m_restoreTimer = new QTimer(this);
    m_restoreTimer->setInterval(5000);
    connect(m_restoreTimer, &QTimer::timeout, this, &AppLauncher::restoreFirefoxProfiles);
    const QDir journals(firefoxJournalDir());
    for (const QString &journal : journals.entryList({QStringLiteral("*.json")}, QDir::Files)) {
        m_firefoxRestores << journals.filePath(journal);
    }
    const QDir chromiumJournals(Chromium::journalDir());
    for (const QString &journal : chromiumJournals.entryList({QStringLiteral("*.json")}, QDir::Files)) {
        m_chromiumRestores << chromiumJournals.filePath(journal);
    }
    restoreFirefoxProfiles();
}

QString AppLauncher::firefoxJournalDir()
{
    return FirefoxPrefs::journalDir();
}

void AppLauncher::setGpuProfile(const GpuProfile &profile)
{
    m_gpuEnvironment = profile.appEnvironment();
    m_firefoxPrefs = profile.firefoxPrefs();
}

void AppLauncher::prepareFirefox(QString &command, const QProcessEnvironment &env, QString &journal)
{
    if (m_firefoxPrefs.empty()) {
        return;
    }
    const FirefoxPrefs::Install install = FirefoxPrefs::installOf(command);
    if (install == FirefoxPrefs::Install::Flatpak) {
        // A flatpak's sandbox gets only the environment it is given: the GPU's video decoding variables.
        QStringList names;
        for (const auto &[name, value] : m_gpuEnvironment) {
            names << name;
        }
        command = FirefoxPrefs::withSandboxEnvironment(command, env, names);
    }
    QString profile = FirefoxPrefs::profileInCommand(command);
    if (profile.isEmpty()) {
        if (FirefoxPrefs::words(command).contains(QLatin1String("-P"))) {
            return; // a profile by name, the user's own choice
        }
        // The profile Firefox would start with, named to it so the preferences are where it looks; a first start's own
        // profile made here when there is none yet (a new user's first Firefox).
        const QString root = FirefoxPrefs::profilesRoot(env, install);
        profile = FirefoxPrefs::defaultProfile(root);
        if (profile.isEmpty()) {
            profile = FirefoxPrefs::createProfile(root);
            if (!profile.isEmpty()) {
                qCInfo(lcApp).noquote() << "firefox: new profile" << profile;
            }
        }
        if (profile.isEmpty()) {
            qCWarning(lcApp).noquote() << "firefox: no profile in" << root << "(HDR video preferences not set)";
            return;
        }
        QString quoted = profile;
        quoted.replace(QLatin1Char('\''), QLatin1String("'\\''"));
        command += QStringLiteral(" --profile '") + quoted + QLatin1Char('\'');
    }
    // The user's own profile has them only while this Firefox runs (some are wrong for the desktop's Firefox);
    // Rift Home's own isolated ones keep them. A snap's or flatpak's is always the user's own (their sandbox ignores
    // Rift Home's isolated app data).
    const bool own = m_ownData || m_options.isolatedXdgDir.isEmpty() || install != FirefoxPrefs::Install::Native;
    if (own && FirefoxPrefs::inUse(profile)) {
        qCInfo(lcApp).noquote() << "firefox: profile" << profile << "is open in another Firefox: preferences not set";
        return;
    }
    if (own) {
        journal = firefoxJournalDir() + QLatin1Char('/')
            + QString::fromLatin1(QCryptographicHash::hash(profile.toUtf8(), QCryptographicHash::Sha1).toHex().left(16)) + QStringLiteral(".json");
    }
    if (FirefoxPrefs::apply(profile, m_firefoxPrefs, journal)) {
        qCInfo(lcApp).noquote() << "firefox: HDR video preferences in" << profile << (own ? "(until it closes)" : "");
    } else {
        qCWarning(lcApp).noquote() << "firefox: preferences not written to" << profile;
        journal.clear();
    }
}

void AppLauncher::prepareMpv(QString &command)
{
    // mpv tags HDR video with the screen's range by default (Rift Home's output: PQ up to 10000 cd/m²), so the
    // runtime's HDR mapping took films for far brighter than they are; with the film's own (mastering, MaxCLL) they
    // map as made. Given on its command line only (the user's mpv settings untouched), unless the command says otherwise.
    const QStringList words = FirefoxPrefs::words(command);
    qsizetype program = 0;
    while (program < words.size() && (words.at(program) == QLatin1String("env") || words.at(program) == QLatin1String("exec")
                                      || (words.at(program).contains(QLatin1Char('=')) && !words.at(program).startsWith(QLatin1Char('-'))))) {
        ++program;
    }
    if (program >= words.size() || QFileInfo(words.at(program)).fileName() != QLatin1String("mpv")
        || command.contains(QLatin1String("--target-colorspace-hint"))) {
        return;
    }
    const qsizetype at = command.indexOf(words.at(program));
    if (at < 0) {
        return;
    }
    command.insert(at + words.at(program).size(), QStringLiteral(" --target-colorspace-hint=yes --target-colorspace-hint-mode=source"));
}

void AppLauncher::prepareChromium(QString &command, const QProcessEnvironment &env) const
{
    if (!env.contains(QStringLiteral("XR_RUNTIME_JSON"))) {
        return; // no VR runtime of Rift Home's
    }
    const Chromium::Install install = Chromium::installOf(command);
    if (install == Chromium::Install::Snap) {
        qCInfo(lcApp).noquote() << "chromium: a snap; its confinement keeps RiftLinux's runtime out of reach (no WebXR)";
        return;
    }
    if (m_sharedRuntimeJson.isEmpty()) {
        qCWarning(lcApp).noquote() << "chromium: no copy of RiftLinux's client library for it (no WebXR)";
        return;
    }
    const QString prepared = Chromium::prepare(command, {m_sharedRuntimeJson, QFileInfo(m_runtimeCopy).absolutePath()});
    if (prepared != command) {
        qCInfo(lcApp).noquote() << "chromium: WebXR on RiftLinux's runtime" << (install == Chromium::Install::Flatpak ? "(flatpak: given the runtime for this start)" : "");
        command = prepared;
    }
}

void AppLauncher::restoreFirefoxProfiles()
{
    for (auto it = m_firefoxRestores.begin(); it != m_firefoxRestores.end();) {
        if (FirefoxPrefs::restore(*it)) {
            qCInfo(lcApp).noquote() << "firefox: profile put back as it was (" << *it << ")";
            it = m_firefoxRestores.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = m_chromiumRestores.begin(); it != m_chromiumRestores.end();) {
        if (Chromium::restore(*it)) {
            qCInfo(lcApp).noquote() << "chromium: its title bar setting put back as it was (" << *it << ")";
            it = m_chromiumRestores.erase(it);
        } else {
            ++it;
        }
    }
    if (m_firefoxRestores.isEmpty() && m_chromiumRestores.isEmpty()) {
        m_restoreTimer->stop();
    } else if (!m_restoreTimer->isActive()) {
        m_restoreTimer->start(); // a browser still has it open
    }
}

void AppLauncher::prepareChromiumTitleBar(const QString &command, const QProcessEnvironment &env, QString &journal)
{
    const QString data = Chromium::userDataDir(command, env);
    if (data.isEmpty()) {
        return;
    }
    // The user's own data has it only while this browser runs; Rift Home's own (isolated) keeps it.
    const bool own = m_ownData || m_options.isolatedXdgDir.isEmpty() || !QDir::cleanPath(data).startsWith(QDir::cleanPath(m_options.isolatedXdgDir) + QLatin1Char('/'));
    const QString note = own ? Chromium::journalDir() + QLatin1Char('/')
            + QString::fromLatin1(QCryptographicHash::hash(data.toUtf8(), QCryptographicHash::Sha1).toHex().left(16)) + QStringLiteral(".json")
                             : QString();
    if (Chromium::useSystemTitleBar(data, note)) {
        journal = note;
        qCInfo(lcApp).noquote() << "chromium: its own title bar off (Rift Home's frame) in" << data << (own ? "(until it closes)" : "");
    } else {
        qCInfo(lcApp).noquote() << "chromium: title bar setting not changed in" << data << "(open in another browser, or a first start)";
    }
}

void AppLauncher::writeFontConfig()
{
    // Text in the room never lines up with the headset's pixels (a window is turned, curved, scaled and resampled twice
    // on the way): subpixel (RGB) antialiasing only adds coloured fringes there, the more on the CV1's PenTile panel.
    // The apps' fontconfig: the system's and the user's own settings, then grayscale antialiasing and
    // slight hinting for every font. Never written into the user's own configuration.
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty()) {
        return;
    }
    const QString base = qEnvironmentVariable("FONTCONFIG_FILE", QStringLiteral("/etc/fonts/fonts.conf"));
    QSaveFile file(dir + QStringLiteral("/rift-home-fonts.conf"));
    if (!file.open(QIODevice::WriteOnly)) {
        return;
    }
    file.write(QStringLiteral("<?xml version=\"1.0\"?>\n<!DOCTYPE fontconfig SYSTEM \"urn:fontconfig:fonts.dtd\">\n<fontconfig>\n"
                              "  <!-- Rift Home's apps: grayscale antialiasing, slight hinting (written by Rift Home) -->\n"
                              "  <include ignore_missing=\"yes\">%1</include>\n"
                              "  <match target=\"font\">\n"
                              "    <edit name=\"antialias\" mode=\"assign\"><bool>true</bool></edit>\n"
                              "    <edit name=\"rgba\" mode=\"assign\"><const>none</const></edit>\n"
                              "    <edit name=\"lcdfilter\" mode=\"assign\"><const>lcdnone</const></edit>\n"
                              "    <edit name=\"hinting\" mode=\"assign\"><bool>true</bool></edit>\n"
                              "    <edit name=\"hintstyle\" mode=\"assign\"><const>hintslight</const></edit>\n"
                              "  </match>\n</fontconfig>\n")
                   .arg(base.toHtmlEscaped())
                   .toUtf8());
    if (file.commit()) {
        m_fontConfig = file.fileName();
    }
}

bool AppLauncher::refreshRuntimeCopy()
{
    // The runtime refuses a client library of another build (the runtime's ipc_client_check_git_tag),
    // so the copy is the installed library exactly: the same size and modification time (kept on the copy), checked
    // before every start. A new copy is written beside the old one and renamed over it: an app never loads a half-written
    // library, and running ones keep the one they loaded.
    const QFileInfo source(m_runtimeLibrary);
    if (m_runtimeCopy.isEmpty() || !source.isFile()) {
        return false;
    }
    const QFileInfo copy(m_runtimeCopy);
    if (copy.isFile() && copy.size() == source.size() && copy.lastModified() == source.lastModified()) {
        return true;
    }
    const QString fresh = m_runtimeCopy + QStringLiteral(".new");
    QFile::remove(fresh); // ours: a copy cut short before
    QFile written(fresh);
    bool ok = QFile::copy(m_runtimeLibrary, fresh) && written.open(QIODevice::ReadWrite)
              && written.setFileTime(source.lastModified(), QFileDevice::FileModificationTime);
    written.close();
    ok = ok && ::rename(QFile::encodeName(fresh).constData(), QFile::encodeName(m_runtimeCopy).constData()) == 0;
    if (!ok) {
        QFile::remove(fresh);
        qCWarning(lcApp).noquote() << "VR apps: the copy of RiftLinux's OpenXR client library could not be refreshed:" << m_runtimeCopy;
        return copy.isFile();
    }
    qCInfo(lcApp).noquote() << "VR apps: RiftLinux's OpenXR client library copied for apps (Steam's container included):" << m_runtimeLibrary;
    return true;
}

void AppLauncher::prepareVrApps()
{
    // Only on RiftLinux's runtime (Rift Home's own, from riftlinux-run or its session).
    const QString given = qEnvironmentVariable("XR_RUNTIME_JSON");
    if (given.isEmpty() || !given.contains(QLatin1String("riftlinux"))) {
        return;
    }
    // Steam's runtime container (Proton through umu or Steam: Faugus, Lutris, Steam) has a /usr of its own and does not
    // see /usr/lib/riftlinux: a copy of the runtime's OpenXR client library with a runtime file beside it, in Rift Home's
    // folder (the home is shared with the container), serves the apps in and out of it (Beat Saber through
    // Faugus, for one). Refreshed when RiftLinux's library changes. The runtime file names the copy by its absolute path:
    // Chromium's XR sandbox refuses "./libopenxr_monado.so" (Chromium), and a flatpak is given the same folder.
    QFile manifest(given);
    const QString library = manifest.open(QIODevice::ReadOnly)
        ? QFileInfo(QFileInfo(given).absolutePath() + QLatin1Char('/')
                    + QJsonDocument::fromJson(manifest.readAll()).object().value(QStringLiteral("runtime")).toObject().value(QStringLiteral("library_path")).toString())
              .absoluteFilePath()
        : QString();
    const QString base = qEnvironmentVariableIsEmpty("RIFT_HOME_STATE_DIR")
        ? QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/rift-home")
        : qEnvironmentVariable("RIFT_HOME_STATE_DIR");
    const QString dir = QFileInfo(base + QStringLiteral("/openxr")).absoluteFilePath();
    m_runtimeLibrary = library;
    m_runtimeCopy = dir + QStringLiteral("/libopenxr_monado.so");
    QSaveFile json(dir + QStringLiteral("/openxr_riftlinux.json"));
    if (QDir().mkpath(dir) && refreshRuntimeCopy() && json.open(QIODevice::WriteOnly)) {
        json.write(QJsonDocument(QJsonObject{{QStringLiteral("file_format_version"), QStringLiteral("1.0.0")},
                                             {QStringLiteral("runtime"), QJsonObject{{QStringLiteral("name"), QStringLiteral("RiftLinux (Rift Home's apps)")},
                                                                                    {QStringLiteral("library_path"), m_runtimeCopy}}}})
                       .toJson());
        if (json.commit()) {
            m_sharedRuntimeJson = dir + QStringLiteral("/openxr_riftlinux.json");
        }
    }
    // VR games through Proton: its VR start goes through OpenVR (SteamVR's interface) before its OpenXR bridge works,
    // even for OpenXR-only games; xrizer answers it on RiftLinux's runtime, for the apps started here only (SteamVR's
    // own setting stays). RIFT_HOME_XRIZER: another folder, or "0" for none.
    const QString chosen = qEnvironmentVariable("RIFT_HOME_XRIZER");
    const QString xrizer = chosen.isEmpty() ? QStringLiteral("/opt/xrizer") : chosen;
    if (chosen != QLatin1String("0") && QFileInfo::exists(xrizer + QStringLiteral("/bin/linux64/vrclient.so"))) {
        m_xrizer = xrizer;
    }
    qCInfo(lcApp).noquote() << QStringLiteral("VR apps: RiftLinux's runtime %1; %2")
                                   .arg(m_sharedRuntimeJson.isEmpty() ? QStringLiteral("as given (Steam's container cannot reach it)")
                                                                      : QStringLiteral("also inside Steam's container (%1)").arg(m_sharedRuntimeJson),
                                        m_xrizer.isEmpty() ? QStringLiteral("no xrizer: Proton VR games need SteamVR")
                                                           : QStringLiteral("Proton VR games through xrizer (%1), not SteamVR").arg(m_xrizer));
}

AppLauncher::~AppLauncher()
{
    terminateAll();
}

QProcessEnvironment AppLauncher::environment() const
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("WAYLAND_DISPLAY"), m_options.socketName);
    for (const char *name : {"DISPLAY", "WAYLAND_SOCKET", "XDG_ACTIVATION_TOKEN", "DESKTOP_STARTUP_ID"}) {
        env.remove(QString::fromLatin1(name));
    }
    env.insert(QStringLiteral("XDG_SESSION_TYPE"), QStringLiteral("wayland"));
    if (!m_x11Display.isEmpty()) {
        env.insert(QStringLiteral("DISPLAY"), m_x11Display); // Rift Home's own XWayland, never the desktop's X display
    }
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("wayland"));
    env.insert(QStringLiteral("GDK_BACKEND"), QStringLiteral("wayland"));
    env.insert(QStringLiteral("MOZ_ENABLE_WAYLAND"), QStringLiteral("1"));
    // No SDL_VIDEODRIVER (a user's own passes through): SDL 2 (most games, some with an old SDL built in) takes X11,
    // through Rift Home's XWayland; forced onto Wayland, Unity games built with an old SDL saw a 0x0 desktop and never
    // opened a window. SDL 3 takes Wayland by itself.
    env.insert(QStringLiteral("ELECTRON_OZONE_PLATFORM_HINT"), QStringLiteral("wayland"));
    env.insert(QStringLiteral("_JAVA_AWT_WM_NONREPARENTING"), QStringLiteral("1"));
    env.insert(QStringLiteral("STEAM_FORCE_DESKTOPUI_SCALING"), QString::number(m_x11Scale, 'g', 3));
    if (!m_fontConfig.isEmpty()) {
        env.insert(QStringLiteral("FONTCONFIG_FILE"), m_fontConfig);
    }
    // Video decoding on the GPU Rift Home renders on (GpuProfile: NVIDIA's VA-API driver named, with its direct backend
    // and Firefox's decoder unsandboxed; elsewhere libva finds the GPU's driver). The user's own values win.
    for (const auto &[name, value] : m_gpuEnvironment) {
        if (!env.contains(name)) {
            env.insert(name, value);
        }
    }
    // VR apps on RiftLinux's runtime from inside Steam's runtime container (Proton through umu or Steam:
    // Faugus, Lutris, Steam; Beat Saber through Faugus): the container takes the host's OpenXR runtime and
    // reaches its socket only when asked.
    if (env.contains(QStringLiteral("XR_RUNTIME_JSON"))) {
        const QString socket = env.value(QStringLiteral("XDG_RUNTIME_DIR")) + QStringLiteral("/monado_comp_ipc");
        QStringList shared = env.value(QStringLiteral("PRESSURE_VESSEL_FILESYSTEMS_RW")).split(QLatin1Char(':'), Qt::SkipEmptyParts);
        if (!env.value(QStringLiteral("XDG_RUNTIME_DIR")).isEmpty() && !shared.contains(socket)) {
            shared << socket;
        }
        env.insert(QStringLiteral("PRESSURE_VESSEL_FILESYSTEMS_RW"), shared.join(QLatin1Char(':')));
        if (!m_sharedRuntimeJson.isEmpty()) {
            env.insert(QStringLiteral("XR_RUNTIME_JSON"), m_sharedRuntimeJson);
        }
        if (!m_xrizer.isEmpty()) {
            env.insert(QStringLiteral("VR_PATHREG_OVERRIDE"), m_xrizer + QStringLiteral("/openvrpaths.vrpath"));
            QStringList readOnly = env.value(QStringLiteral("PRESSURE_VESSEL_FILESYSTEMS_RO")).split(QLatin1Char(':'), Qt::SkipEmptyParts);
            if (!readOnly.contains(m_xrizer)) {
                readOnly << m_xrizer;
            }
            env.insert(QStringLiteral("PRESSURE_VESSEL_FILESYSTEMS_RO"), readOnly.join(QLatin1Char(':')));
        }
    }
    if (!m_options.isolatedXdgDir.isEmpty() && !m_ownData) {
        // Flatpak finds the apps installed for the user under XDG_DATA_HOME: that stays the user's (else every
        // Flathub app "not installed" with Rift Home's own app data). A flatpak's own data is in ~/.var/app either way.
        if (!env.contains(QStringLiteral("FLATPAK_USER_DIR"))) {
            const QString data = env.value(QStringLiteral("XDG_DATA_HOME"), QDir::homePath() + QStringLiteral("/.local/share"));
            env.insert(QStringLiteral("FLATPAK_USER_DIR"), data + QStringLiteral("/flatpak"));
        }
        for (const auto &[var, sub] : {std::pair{"XDG_CONFIG_HOME", "config"}, std::pair{"XDG_DATA_HOME", "data"},
                                       std::pair{"XDG_CACHE_HOME", "cache"}, std::pair{"XDG_STATE_HOME", "state"}}) {
            const QString dir = m_options.isolatedXdgDir + QLatin1Char('/') + QLatin1String(sub);
            QDir().mkpath(dir);
            env.insert(QString::fromLatin1(var), dir);
        }
    }
    return env;
}

void AppLauncher::setOwnData(bool on)
{
    if (on == m_ownData) {
        return;
    }
    m_ownData = on;
    qCInfo(lcApp).noquote() << (on || m_options.isolatedXdgDir.isEmpty() ? QStringLiteral("apps started from now on use the user's own settings and profiles")
                                                                         : QStringLiteral("apps started from now on use their own folder %1").arg(m_options.isolatedXdgDir));
}

void AppLauncher::setX11Scale(double scale)
{
    scale = std::clamp(scale, 1.0, 3.0);
    if (std::abs(scale - m_x11Scale) < 0.001) {
        return;
    }
    m_x11Scale = scale;
    qCInfo(lcApp).noquote() << QStringLiteral("X11 apps started from now on at %1 %").arg(int(std::lround(scale * 100)));
}

QStringList AppLauncher::runningTags() const
{
    QStringList tags;
    for (const QProcess *process : m_processes) {
        const QString tag = process->property("riftTag").toString();
        if (!tag.isEmpty() && process->state() != QProcess::NotRunning && !tags.contains(tag)) {
            tags << tag;
        }
    }
    return tags;
}

void AppLauncher::raiseFileLimit()
{
    rlimit limit{};
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
        return;
    }
    g_appFileLimit = limit;
    if (limit.rlim_cur >= limit.rlim_max) {
        return;
    }
    rlimit raised = limit;
    raised.rlim_cur = limit.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &raised) == 0) {
        g_fileLimitRaised = true;
        qCInfo(lcApp).noquote() << QStringLiteral("open files: up to %1 (was %2, programs started here keep %2)")
                                       .arg(qulonglong(raised.rlim_cur))
                                       .arg(qulonglong(limit.rlim_cur));
    }
}

void AppLauncher::launch(const QString &command, std::function<void(qint64)> onStarted, const QString &tag)
{
    const QString program = command.section(QLatin1Char(' '), 0, 0, QString::SectionSkipEmpty).section(QLatin1Char('/'), -1);
    QDir().mkpath(m_options.logDir);
    const QString log = QStringLiteral("%1/%2-%3.log").arg(m_options.logDir).arg(++m_count, 2, 10, QLatin1Char('0')).arg(program);

    auto *process = new QProcess(this);
    if (!m_sharedRuntimeJson.isEmpty()) {
        refreshRuntimeCopy(); // RiftLinux updated since the last start
    }
    const QProcessEnvironment env = environment();
    QString line = command;
    QString firefoxJournal;
    QString chromiumJournal;
    if (FirefoxPrefs::isFirefox(command)) {
        prepareFirefox(line, env, firefoxJournal);
    }
    prepareMpv(line);
    if (Chromium::isChromium(command)) {
        prepareChromium(line, env);
        if (m_scalePerSurface) {
            line = Chromium::withScale(line, m_appScale);
        }
        prepareChromiumTitleBar(line, env, chromiumJournal);
    }
    // A flatpak's sandbox does not see Rift Home's font settings (FONTCONFIG_FILE, in the runtime folder): fontconfig
    // then found no configuration at all (seen with Flathub's Chromium). Given to it read-only for this start.
    if (!m_fontConfig.isEmpty() && !Command::flatpakApp(line).isEmpty()) {
        const QString runtimeDir = env.value(QStringLiteral("XDG_RUNTIME_DIR"));
        const QString path = !runtimeDir.isEmpty() && m_fontConfig.startsWith(runtimeDir + QLatin1Char('/'))
            ? QStringLiteral("xdg-run") + m_fontConfig.mid(runtimeDir.size())
            : m_fontConfig;
        line = Command::withFlatpakOptions(line, {QStringLiteral("--filesystem=") + path + QStringLiteral(":ro")});
    }
    process->setProcessEnvironment(env);
    process->setProgram(QStringLiteral("/bin/sh"));
    process->setArguments({QStringLiteral("-c"), QStringLiteral("exec ") + line});
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setStandardOutputFile(log);
    // The server blocks SIGINT/SIGTERM for its event loop; programs get a clean signal mask, and the open-file limit
    // they would have had (raiseFileLimit).
    process->setChildProcessModifier([] {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        if (g_fileLimitRaised) {
            setrlimit(RLIMIT_NOFILE, &g_appFileLimit);
        }
    });
    connect(process, &QProcess::started, this, [process, program, log, onStarted] {
        qCInfo(lcApp).noquote() << "started" << program << "pid" << process->processId() << "log" << log;
        if (onStarted) {
            onStarted(process->processId());
        }
    });
    connect(process, &QProcess::finished, this, [this, program, firefoxJournal, chromiumJournal](int code, QProcess::ExitStatus status) {
        // QProcess reports any end by a signal as a crash; SIGTERM/SIGKILL are ours.
        if (status == QProcess::CrashExit) {
            qCInfo(lcApp).noquote() << program << "ended by signal" << code;
        } else {
            qCInfo(lcApp).noquote() << program << "ended, exit code" << code;
        }
        if (!firefoxJournal.isEmpty() && !m_firefoxRestores.contains(firefoxJournal)) {
            m_firefoxRestores << firefoxJournal;
        }
        if (!chromiumJournal.isEmpty() && !m_chromiumRestores.contains(chromiumJournal)) {
            m_chromiumRestores << chromiumJournal;
        }
        if (!m_firefoxRestores.isEmpty() || !m_chromiumRestores.isEmpty()) {
            restoreFirefoxProfiles(); // (later, while another browser still has it open)
        }
    });
    connect(process, &QProcess::errorOccurred, this, [program](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            qCWarning(lcApp).noquote() << program << "failed to start";
        }
    });
    process->setProperty("riftTag", tag);
    m_processes << process;
    process->start();
}

void AppLauncher::shutdown(int graceMs, std::function<void()> done)
{
    QList<QProcess *> running;
    for (QProcess *process : std::as_const(m_processes)) {
        if (process->state() != QProcess::NotRunning) {
            running << process;
            process->terminate();
        }
    }
    if (running.isEmpty()) {
        done();
        return;
    }
    auto remaining = std::make_shared<int>(running.size());
    auto finished = std::make_shared<bool>(false);
    auto finish = [running, finished, done] {
        if (*finished) {
            return;
        }
        *finished = true;
        for (QProcess *process : running) {
            if (process->state() != QProcess::NotRunning) {
                qCWarning(lcApp) << "pid" << process->processId() << "still running after SIGTERM: killed";
                process->kill();
            }
        }
        done();
    };
    for (QProcess *process : running) {
        connect(process, &QProcess::finished, this, [remaining, finish] {
            if (--*remaining == 0) {
                finish();
            }
        });
    }
    QTimer::singleShot(graceMs, this, finish);
}

void AppLauncher::terminateAll()
{
    for (QProcess *process : std::as_const(m_processes)) {
        if (process->state() != QProcess::NotRunning) {
            process->terminate();
        }
    }
    for (QProcess *process : std::as_const(m_processes)) {
        if (process->state() != QProcess::NotRunning && !process->waitForFinished(3000)) {
            qCWarning(lcApp) << "pid" << process->processId() << "ignored SIGTERM: killed";
            process->kill();
            process->waitForFinished(1000);
        }
    }
    // The user's Firefox profiles put back now; one still open (a Firefox not started here) at the next start.
    if (!m_firefoxRestores.isEmpty() || !m_chromiumRestores.isEmpty()) {
        restoreFirefoxProfiles();
        if (m_restoreTimer) {
            m_restoreTimer->stop();
        }
    }
}

} // namespace RiftHome::App
