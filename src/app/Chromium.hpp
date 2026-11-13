// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QProcessEnvironment>
#include <QString>

namespace RiftHome::App
{

/*!
 * WebXR in a Chromium-based browser Rift Home starts, ready with no setup by hand (typing a long
 * command in the headset is no way to start a browser).
 *
 * - Chromium's OpenXR is off on Linux unless its command line asks for it (--enable-features=OpenXR; chrome://flags alone
 *   does not turn it on), and it must be told to use OpenXR (--force-webxr-runtime=openxr). Both are added to the start,
 *   merged into a --enable-features the command already has; the user's own settings stay untouched.
 * - Its XR process's sandbox refuses a runtime library named by a relative path ("./libopenxr_monado.so": Permission
 *   denied, also with Landlock off), so the runtime file it gets names the library by an
 *   absolute path (AppLauncher's copy of RiftLinux's client library).
 * - Its XR process's system-call filter (seccomp) kills it when the GPU's Vulkan driver starts there: NVIDIA's asks
 *   sched_get_priority_max(SCHED_RR) at vkCreateDevice, which Chromium's GPU process
 *   may do and its XR process not ("XRSession creation failed"); there is no switch for the XR process alone. So the
 *   filter is off for the Chromium Rift Home starts with WebXR (--disable-seccomp-filter-sandbox; its namespace
 *   sandbox stays), on every GPU (Mesa's drivers set thread priorities too and were not tried).
 * - A flatpak (Flathub's org.chromium.Chromium and the other Chromium-based ones) gets, for this start only, the copy's
 *   folder read-only, the runtime's socket and the runtime file (flatpak run's own options): the user's overrides are
 *   not needed and stay as they are.
 * - A snap's confinement keeps the runtime's socket out of reach: its command is left as it is.
 */
class Chromium
{
public:
    enum class Install {
        None, //!< not a Chromium-based browser
        Native, //!< the distribution's package or the vendor's
        Snap, //!< Ubuntu's chromium (/snap/bin/chromium, or a /usr/bin script handing over to it)
        Flatpak, //!< flatpak run org.chromium.Chromium (or Chrome, Brave, Edge, Vivaldi, ungoogled Chromium)
    };
    //! RiftLinux's runtime as the browser is to find it.
    struct Runtime {
        QString json; //!< the runtime file, its library named by an absolute path
        QString libraryDir; //!< the folder of the runtime file and the library: given read-only to a flatpak
    };

    //! What @p command starts (after "env", "exec" and NAME=value words).
    static Install installOf(const QString &command);
    static bool isChromium(const QString &command)
    {
        return installOf(command) != Install::None;
    }
    //! @p command with WebXR on @p runtime (see above); unchanged when not Chromium-based, a snap, or already prepared.
    static QString prepare(const QString &command, const Runtime &runtime);
    /*!
     * @p command told to draw at @p scale (--force-device-scale-factor). On Wayland Chromium takes its scale from the
     * output it is on and not from what the compositor tells the window (fractional scale, preferred buffer scale; also
     * with WaylandFractionalScaleV1); Rift Home's own session tells windows only. Unchanged when the command
     * sets a scale itself or runs Chromium as an X11 app (the X resources' DPI does it there).
     */
    static QString withScale(const QString &command, double scale);

    /*!
     * Chromium draws its own title bar and border inside Rift Home's window frame, so it is set to use the
     * system's: its "Use system title bar and borders" (browser.custom_chrome_frame false, a profile preference; no
     * command-line switch) in every profile of the browser data @p command uses, while it runs. The values before are
     * noted in @p journal first (when given: a user's own data) and put back by restore() once the browser has closed.
     * Not while a browser has that data open (it keeps its own and writes it back). True when set.
     */
    static bool useSystemTitleBar(const QString &userDataDir, const QString &journal = {});
    //! The browser data back as it was before useSystemTitleBar(), unless a browser has it open; true when done.
    static bool restore(const QString &journal);
    //! The browser data folder @p command uses (--user-data-dir, else the browser's own under the configuration folder
    //! of @p env, or a flatpak's under ~/.var/app); empty: not Chromium-based or not known.
    static QString userDataDir(const QString &command, const QProcessEnvironment &env);
    //! Whether a running browser has @p userDataDir open (its SingletonLock; a flatpak's: one of its instances running).
    static bool inUse(const QString &userDataDir);
    //! Where the journals of browser data to put back are (the user's state folder).
    static QString journalDir();
    //! Every browser data folder with a journal put back where no browser has it open; how many are still waiting.
    static int restoreAll();
};

} // namespace RiftHome::App
