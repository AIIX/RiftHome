// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app/GpuProfile.hpp"

#include <QList>
#include <QObject>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <functional>
#include <utility>
#include <vector>

class QProcess;
class QTimer;

namespace RiftHome::App
{

/*!
 * Starts programs inside Rift Home: Wayland only (no X11 display: they would open on the desktop), Rift Home's
 * socket, each with its own log file. With an isolated XDG folder they read and write their settings there, so the
 * desktop's settings stay untouched.
 */
class AppLauncher : public QObject
{
    Q_OBJECT

public:
    struct Options {
        QString socketName;
        QString logDir;
        QString isolatedXdgDir; //!< empty: the user's own XDG folders
    };

    explicit AppLauncher(const Options &options, QObject *parent = nullptr);
    ~AppLauncher() override;

    //! Runs a shell command line; its output goes to `<logDir>/NN-<program>.log`.
    //! @p onStarted gets its process id once running (sh's exec keeps it the program's).
    //! @p tag: what it was started as (an app's desktop id), for reopening it next time (runningTags()).
    void launch(const QString &command, std::function<void(qint64)> onStarted = {}, const QString &tag = {});
    //! The tags of the programs still running (the apps to reopen when Rift Home starts again).
    QStringList runningTags() const;
    /*!
     * Rift Home may keep many files open: a client's every dmabuf (Firefox keeps a buffer, one or two files, for each
     * video frame surface it ever showed in a window: 2048 ran out after three minutes of videos
     * and Firefox crashed). So the soft limit goes up to the hard one, as KWin and sway do; programs started here get
     * the limit they had (select() cannot take descriptors past 1024). Call once at start.
     */
    static void raiseFileLimit();
    /*!
     * SIGTERM to every running program while the server keeps serving them (they say goodbye over Wayland), then
     * @p done once all have ended, or after @p graceMs with SIGKILL for the rest.
     */
    void shutdown(int graceMs, std::function<void()> done);
    //! The same, blocking (the server no longer runs): the last resort at exit.
    void terminateAll();
    QProcessEnvironment environment() const;
    /*!
     * Programs started from now on use the user's own settings and profiles (Firefox's, the apps' configuration) instead
     * of the isolated folder (--with-profile, or Settings' "Use my own app data"). Running ones keep
     * what they started with.
     */
    void setOwnData(bool on);
    //! XWayland's display: X11 programs started inside Rift Home open there; empty: no X11 at all.
    void setX11Display(const QString &display)
    {
        m_x11Display = display;
    }
    bool ownData() const
    {
        return m_ownData;
    }
    /*!
     * The size X11 programs started from now on draw at (Steam is barely readable at the desktop's
     * size): Steam by its own switch here, the toolkits reading Xft.dpi by Rift Home's X resources (main).
     */
    void setX11Scale(double scale);
    /*!
     * The apps' scale, and whether Rift Home tells it window by window (its own session) instead of by an output.
     * Chromium-based browsers on Wayland take their scale from an output only (Chromium's text is half size in
     * Rift Home's own session, Firefox's is right): told it on their command line there, from their next start.
     */
    void setAppScale(double scale, bool perSurface)
    {
        m_appScale = scale;
        m_scalePerSurface = perSurface;
    }
    double x11Scale() const
    {
        return m_x11Scale;
    }
    /*!
     * The GPU's path for the apps (GpuProfile): their environment for video decoding, and Firefox's preferences for HDR
     * video, given to each Firefox started here (FirefoxPrefs: kept in Rift Home's own profiles, only for the session
     * in the user's own).
     */
    void setGpuProfile(const GpuProfile &profile);

private:
    Options m_options;
    QList<QProcess *> m_processes;
    bool m_ownData = false;
    double m_x11Scale = 2.0;
    double m_appScale = 2.0;
    bool m_scalePerSurface = false;
    //! VR apps (RiftLinux's runtime): a runtime file that also works inside Steam's container, and xrizer.
    void prepareVrApps();
    void writeFontConfig();
    /*!
     * Firefox about to start with @p command (with the launch's @p env): its profile (named, the default, or one made as
     * its first start would) is passed to it, with the GPU's preferences in it. @p journal: set when they must be taken
     * out again after it closes (the user's own profile).
     */
    void prepareFirefox(QString &command, const QProcessEnvironment &env, QString &journal);
    //! mpv about to start with @p command: told to describe HDR video by the film's own metadata.
    static void prepareMpv(QString &command);
    //! A Chromium-based browser about to start with @p command: WebXR on RiftLinux's runtime (Chromium).
    void prepareChromium(QString &command, const QProcessEnvironment &env) const;
    //! The user's own Firefox profiles put back (FirefoxPrefs::restore) where Firefox has closed them; tried again later.
    void restoreFirefoxProfiles();
    static QString firefoxJournalDir();
    std::vector<std::pair<QString, QString>> m_gpuEnvironment;
    std::vector<std::pair<QString, QString>> m_firefoxPrefs;
    QStringList m_firefoxRestores; //!< journals of profiles to put back
    QStringList m_chromiumRestores; //!< journals of Chromium-based browsers' data to put back (Chromium::restore)
    /*!
     * A Chromium-based browser about to start with @p command: its own title bar and border off inside Rift Home's
     * frame (Chromium::useSystemTitleBar). @p journal: set when it must be put back after it closes (the user's own data).
     */
    void prepareChromiumTitleBar(const QString &command, const QProcessEnvironment &env, QString &journal);
    QTimer *m_restoreTimer = nullptr;
    //! The copy of RiftLinux's OpenXR client library the same as the installed one (the runtime takes a client library
    //! of its own build only); false: none could be made.
    bool refreshRuntimeCopy();
    QString m_runtimeLibrary; //!< RiftLinux's OpenXR client library, as installed
    QString m_runtimeCopy; //!< its copy beside m_sharedRuntimeJson
    QString m_sharedRuntimeJson; //!< RiftLinux's OpenXR runtime as apps in Steam's container find it too; empty: as given
    QString m_xrizer; //!< xrizer's folder (Proton's VR start on RiftLinux's runtime instead of SteamVR); empty: none
    QString m_x11Display;
    QString m_fontConfig; //!< FONTCONFIG_FILE for the apps: grayscale antialiasing (empty: not written)
    int m_count = 0;
};

} // namespace RiftHome::App
