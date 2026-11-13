// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QProcessEnvironment>
#include <QString>

#include <utility>
#include <vector>

namespace RiftHome::App
{

/*!
 * Firefox's preferences for the Firefox Rift Home starts (HDR video, the GPU's decoding path: GpuProfile), with no hand
 * setup on any machine.
 *
 * - Rift Home's own (isolated) profiles keep them.
 * - A user's own profile has them only while Rift Home's Firefox runs: some are wrong on the desktop (forced hardware
 *   decoding on NVIDIA made Firefox crash under KWin), and Firefox takes preferences only from the profile
 *   (no per-start policies). What is changed is noted first (a journal in Rift Home's state), and put back exactly when
 *   Firefox has closed the profile: at its exit, later if it still runs, or at Rift Home's next start after a crash.
 */
class FirefoxPrefs
{
public:
    using Prefs = std::vector<std::pair<QString, QString>>; //!< name, value in prefs.js syntax

    //! How a Firefox is installed: its profiles are elsewhere for each, and the sandboxed ones ignore Rift Home's own
    //! (isolated) app data: their profile is always the user's.
    enum class Install {
        None, //!< not Firefox
        Native, //!< the distribution's package (or Mozilla's tarball)
        Snap, //!< Ubuntu's: ~/snap/firefox/common/.mozilla/firefox
        Flatpak, //!< Flathub's org.mozilla.firefox: ~/.var/app/org.mozilla.firefox/...
    };

    //! @p command's words as the shell reads them (quotes, backslashes): the launcher runs commands through sh.
    static QStringList words(const QString &command);
    //! What @p command starts: Firefox natively (firefox, firefox-esr, firefox-bin, ...), as a snap (/snap/bin/firefox,
    //! or a /usr/bin/firefox that hands over to it) or a flatpak (flatpak run org.mozilla.firefox), or not Firefox.
    static Install installOf(const QString &command);
    static bool isFirefox(const QString &command)
    {
        return installOf(command) != Install::None;
    }
    //! The profile folder @p command names (--profile / -profile); empty: none named.
    static QString profileInCommand(const QString &command);
    //! Firefox's profiles folder for @p env: $XDG_CONFIG_HOME/mozilla/firefox (Firefox's own place now) unless only
    //! ~/.mozilla/firefox has a profiles.ini; a snap's or flatpak's in its own folder under the home.
    static QString profilesRoot(const QProcessEnvironment &env, Install install = Install::Native);
    //! @p command with @p env's @p names given to a flatpak's sandbox (--env=NAME=VALUE after "run"); others unchanged.
    static QString withSandboxEnvironment(const QString &command, const QProcessEnvironment &env, const QStringList &names);
    //! The profile Firefox starts with in @p root: the installation's default, else the one marked default, else the
    //! only one; empty: none.
    static QString defaultProfile(const QString &root);
    //! A new profile in @p root, made as Firefox's first start makes one (and the default); its folder, or empty.
    static QString createProfile(const QString &root);

    /*!
     * @p prefs into @p profile (user.js: Rift Home's block, replaced each time). With @p journal (a user's own profile):
     * the profile's values before are noted there first, once while it exists, for restore().
     */
    static bool apply(const QString &profile, const Prefs &prefs, const QString &journal = {});
    /*!
     * The profile in @p journal back as it was before apply() (Rift Home's block out of user.js, prefs.js's lines for
     * those names as they were), unless a Firefox has it open. True when restored, or nothing to restore.
     */
    static bool restore(const QString &journal);
    //! Whether a running Firefox has @p profile open (its lock).
    static bool inUse(const QString &profile);
    //! Where the journals of profiles to put back are (the user's state folder).
    static QString journalDir();
    //! Every profile with a journal put back where no Firefox has it open; how many are still waiting.
    static int restoreAll();
};

} // namespace RiftHome::App
