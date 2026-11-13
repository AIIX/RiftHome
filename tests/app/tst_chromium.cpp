// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "app/Chromium.hpp"
#include "app/Command.hpp"
#include "app/FirefoxPrefs.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTest>

using namespace RiftHome::App;

namespace
{
const Chromium::Runtime kRuntime{QStringLiteral("/home/u/.local/share/rift-home/openxr/openxr_riftlinux.json"),
                                   QStringLiteral("/home/u/.local/share/rift-home/openxr")};
// Flathub's desktop entry as the app list reads it (field codes dropped).
const QString kFlathub = QStringLiteral("/usr/bin/flatpak run --branch=stable --arch=x86_64 --command=/app/bin/chromium --file-forwarding org.chromium.Chromium @@u @@");
} // namespace

class TestChromium : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void recognisesChromium()
    {
        QCOMPARE(Chromium::installOf(QStringLiteral("/usr/bin/chromium")), Chromium::Install::Native);
        QCOMPARE(Chromium::installOf(QStringLiteral("env FOO=1 google-chrome-stable https://example.org")), Chromium::Install::Native);
        QCOMPARE(Chromium::installOf(QStringLiteral("brave-browser")), Chromium::Install::Native);
        QCOMPARE(Chromium::installOf(QStringLiteral("/snap/bin/chromium")), Chromium::Install::Snap);
        QCOMPARE(Chromium::installOf(kFlathub), Chromium::Install::Flatpak);
        QCOMPARE(Chromium::installOf(QStringLiteral("flatpak run com.google.Chrome")), Chromium::Install::Flatpak);
        // An option's value written apart is not taken for the app.
        QCOMPARE(Chromium::installOf(QStringLiteral("flatpak --user run --branch stable org.chromium.Chromium")), Chromium::Install::Flatpak);
        QCOMPARE(Chromium::installOf(QStringLiteral("flatpak run org.mozilla.firefox")), Chromium::Install::None);
        QCOMPARE(Chromium::installOf(QStringLiteral("firefox")), Chromium::Install::None);
        QCOMPARE(Chromium::installOf(QStringLiteral("chromiumlike")), Chromium::Install::None);
        QCOMPARE(Chromium::installOf(QStringLiteral("flatpak list")), Chromium::Install::None);
        QCOMPARE(Chromium::installOf(QString()), Chromium::Install::None);
    }

    void snapScriptIsASnap()
    {
        // Ubuntu's /usr/bin/chromium-browser: a script handing over to the snap.
        QTemporaryDir dir;
        const QString script = dir.filePath(QStringLiteral("chromium-browser"));
        QFile file(script);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("#!/bin/sh\nexec /snap/bin/chromium \"$@\"\n");
        file.close();
        QCOMPARE(Chromium::installOf(script), Chromium::Install::Snap);
        QCOMPARE(Chromium::prepare(script, kRuntime), script);
    }

    void nativeGetsTheFlags()
    {
        QCOMPARE(Chromium::prepare(QStringLiteral("chromium https://immersive-web.github.io"), kRuntime),
                 QStringLiteral("chromium --enable-features=OpenXR,WebXRIncubations --force-webxr-runtime=openxr --disable-seccomp-filter-sandbox https://immersive-web.github.io"));
        QCOMPARE(Chromium::prepare(QStringLiteral("env A=1 chromium"), kRuntime),
                 QStringLiteral("env A=1 chromium --enable-features=OpenXR,WebXRIncubations --force-webxr-runtime=openxr --disable-seccomp-filter-sandbox"));
    }

    void featuresMergedIntoTheCommandsOwn()
    {
        // Chromium takes the last --enable-features: the command's own one gets them, nothing else changes.
        QCOMPARE(Chromium::prepare(QStringLiteral("chromium --enable-features=VaapiVideoDecoder --ozone-platform=wayland"), kRuntime),
                 QStringLiteral("chromium --force-webxr-runtime=openxr --disable-seccomp-filter-sandbox --enable-features=VaapiVideoDecoder,OpenXR,WebXRIncubations --ozone-platform=wayland"));
        QCOMPARE(Chromium::prepare(QStringLiteral("chromium --enable-features=OpenXR --force-webxr-runtime=openxr"), kRuntime),
                 QStringLiteral("chromium --disable-seccomp-filter-sandbox --enable-features=OpenXR,WebXRIncubations --force-webxr-runtime=openxr"));
        // The user's own choice of runtime stays.
        QCOMPARE(Chromium::prepare(QStringLiteral("chromium --force-webxr-runtime=orientation-sensors"), kRuntime),
                 QStringLiteral("chromium --enable-features=OpenXR,WebXRIncubations --disable-seccomp-filter-sandbox --force-webxr-runtime=orientation-sensors"));
    }

    void flatpakGetsTheRuntimeForThisStart()
    {
        const QString prepared = Chromium::prepare(kFlathub, kRuntime);
        QCOMPARE(prepared,
                 QStringLiteral("/usr/bin/flatpak run --filesystem=/home/u/.local/share/rift-home/openxr:ro --filesystem=xdg-run/monado_comp_ipc "
                                "--env=XR_RUNTIME_JSON=/home/u/.local/share/rift-home/openxr/openxr_riftlinux.json --branch=stable --arch=x86_64 "
                                "--command=/app/bin/chromium --file-forwarding org.chromium.Chromium --enable-features=OpenXR,WebXRIncubations "
                                "--force-webxr-runtime=openxr --disable-seccomp-filter-sandbox @@u @@"));
        // Still a Chromium flatpak, and prepared once only.
        QCOMPARE(Chromium::installOf(prepared), Chromium::Install::Flatpak);
        QCOMPARE(Chromium::prepare(prepared, kRuntime), prepared);
    }

    void pathsWithSpacesQuoted()
    {
        const Chromium::Runtime spaced{QStringLiteral("/home/a b/it's/openxr.json"), QStringLiteral("/home/a b/it's")};
        const QString prepared = Chromium::prepare(QStringLiteral("flatpak run org.chromium.Chromium 'https://x.org/a b'"), spaced);
        QCOMPARE(FirefoxPrefs::words(prepared),
                 (QStringList{QStringLiteral("flatpak"), QStringLiteral("run"), QStringLiteral("--filesystem=/home/a b/it's:ro"),
                              QStringLiteral("--filesystem=xdg-run/monado_comp_ipc"), QStringLiteral("--env=XR_RUNTIME_JSON=/home/a b/it's/openxr.json"),
                              QStringLiteral("org.chromium.Chromium"), QStringLiteral("--enable-features=OpenXR,WebXRIncubations"),
                              QStringLiteral("--force-webxr-runtime=openxr"), QStringLiteral("--disable-seccomp-filter-sandbox"), QStringLiteral("https://x.org/a b")}));
        // The rest of the command exactly as written (sh still expands what it would have).
        QCOMPARE(Chromium::prepare(QStringLiteral("chromium --user-data-dir=$HOME/xr"), kRuntime),
                 QStringLiteral("chromium --enable-features=OpenXR,WebXRIncubations --force-webxr-runtime=openxr --disable-seccomp-filter-sandbox --user-data-dir=$HOME/xr"));
    }

    void flatpakOptionsForAnyApp()
    {
        // Rift Home's font settings for any flatpak (its sandbox does not see the runtime folder).
        const QString fonts = QStringLiteral("--filesystem=xdg-run/rift-home-fonts.conf:ro");
        QCOMPARE(Command::withFlatpakOptions(QStringLiteral("flatpak run org.kde.kate"), {fonts}),
                 QStringLiteral("flatpak run --filesystem=xdg-run/rift-home-fonts.conf:ro org.kde.kate"));
        QCOMPARE(Command::withFlatpakOptions(QStringLiteral("flatpak run --filesystem=xdg-run/rift-home-fonts.conf:ro org.kde.kate"), {fonts}),
                 QStringLiteral("flatpak run --filesystem=xdg-run/rift-home-fonts.conf:ro org.kde.kate"));
        QCOMPARE(Command::withFlatpakOptions(QStringLiteral("kate"), {fonts}), QStringLiteral("kate"));
        QCOMPARE(Command::withFlatpakOptions(QStringLiteral("flatpak update"), {fonts}), QStringLiteral("flatpak update"));
        QCOMPARE(Command::flatpakApp(kFlathub), QStringLiteral("org.chromium.Chromium"));
        QCOMPARE(Command::flatpakApp(QStringLiteral("env A=1 flatpak run --branch stable org.kde.kate file.txt")), QStringLiteral("org.kde.kate"));
        QVERIFY(Command::flatpakApp(QStringLiteral("flatpak run --arch")).isEmpty());
        // Words keep their places in the command (quoted ones included).
        const auto words = Command::words(QStringLiteral(" a 'b c'  d"));
        QCOMPARE(int(words.size()), 3);
        QCOMPARE(words[1].text, QStringLiteral("b c"));
        QCOMPARE(words[1].begin, qsizetype(3));
        QCOMPARE(words[1].end, qsizetype(8));
    }

    void scaleToldOnTheCommandLine()
    {
        // Rift Home's own session (Chromium's text far too small, Firefox's right).
        QCOMPARE(Chromium::withScale(QStringLiteral("chromium https://x.org"), 1.65), QStringLiteral("chromium --force-device-scale-factor=1.65 https://x.org"));
        QCOMPARE(Chromium::withScale(kFlathub, 2.0),
                 QStringLiteral("/usr/bin/flatpak run --branch=stable --arch=x86_64 --command=/app/bin/chromium --file-forwarding org.chromium.Chromium "
                                "--force-device-scale-factor=2 @@u @@"));
        // The user's own scale, an X11 Chromium, or not Chromium at all: as given.
        QCOMPARE(Chromium::withScale(QStringLiteral("chromium --force-device-scale-factor=1.25"), 2.0), QStringLiteral("chromium --force-device-scale-factor=1.25"));
        QCOMPARE(Chromium::withScale(QStringLiteral("chromium --ozone-platform=x11"), 2.0), QStringLiteral("chromium --ozone-platform=x11"));
        QCOMPARE(Chromium::withScale(QStringLiteral("firefox"), 2.0), QStringLiteral("firefox"));
    }

    void userDataFolders()
    {
        QProcessEnvironment env;
        env.insert(QStringLiteral("HOME"), QStringLiteral("/home/u"));
        QCOMPARE(Chromium::userDataDir(kFlathub, env), QStringLiteral("/home/u/.var/app/org.chromium.Chromium/config/chromium"));
        QCOMPARE(Chromium::userDataDir(QStringLiteral("flatpak run com.google.Chrome"), env), QStringLiteral("/home/u/.var/app/com.google.Chrome/config/google-chrome"));
        QCOMPARE(Chromium::userDataDir(QStringLiteral("chromium"), env), QStringLiteral("/home/u/.config/chromium"));
        QCOMPARE(Chromium::userDataDir(QStringLiteral("brave-browser"), env), QStringLiteral("/home/u/.config/BraveSoftware/Brave-Browser"));
        env.insert(QStringLiteral("XDG_CONFIG_HOME"), QStringLiteral("/iso/config"));
        QCOMPARE(Chromium::userDataDir(QStringLiteral("google-chrome-stable"), env), QStringLiteral("/iso/config/google-chrome"));
        QCOMPARE(Chromium::userDataDir(QStringLiteral("chromium --user-data-dir=/x/y/"), env), QStringLiteral("/x/y"));
        QCOMPARE(Chromium::userDataDir(QStringLiteral("chromium --user-data-dir '/a b'"), env), QStringLiteral("/a b"));
        QVERIFY(Chromium::userDataDir(QStringLiteral("firefox"), env).isEmpty());
    }

    void systemTitleBarWhileItRunsThenBack()
    {
        QTemporaryDir dir;
        const QString data = dir.filePath(QStringLiteral("chromium"));
        QDir().mkpath(data + QStringLiteral("/Default"));
        QDir().mkpath(data + QStringLiteral("/Profile 1"));
        const auto write = [](const QString &path, const QByteArray &bytes) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(bytes);
        };
        const auto frame = [&](const QString &profile) {
            QFile file(data + QLatin1Char('/') + profile + QStringLiteral("/Preferences"));
            return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("browser")).toObject().value(QStringLiteral("custom_chrome_frame"))
                                                  : QJsonValue(QJsonValue::Undefined);
        };
        write(data + QStringLiteral("/Local State"), R"({"profile":{"info_cache":{"Default":{},"Profile 1":{}}}})");
        // Default: never set (absent); Profile 1: the user turned the custom frame on. Big numbers stay exact.
        write(data + QStringLiteral("/Default/Preferences"), R"({"browser":{"window_placement":{"left":10}},"profile":{"created":13345678901234567}})");
        write(data + QStringLiteral("/Profile 1/Preferences"), R"({"browser":{"custom_chrome_frame":true}})");
        QFile::setPermissions(data + QStringLiteral("/Default/Preferences"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        const QFileDevice::Permissions privateFile = QFile::permissions(data + QStringLiteral("/Default/Preferences"));
        const QString journal = dir.filePath(QStringLiteral("journal/x.json"));
        QVERIFY(Chromium::useSystemTitleBar(data, journal));
        QCOMPARE(frame(QStringLiteral("Default")), QJsonValue(false));
        QCOMPARE(frame(QStringLiteral("Profile 1")), QJsonValue(false));
        QCOMPARE(QFile::permissions(data + QStringLiteral("/Default/Preferences")), privateFile); // still the user's only
        QFile prefs(data + QStringLiteral("/Default/Preferences"));
        QVERIFY(prefs.open(QIODevice::ReadOnly));
        QVERIFY(prefs.readAll().contains("13345678901234567"));
        prefs.close();
        QVERIFY(Chromium::restore(journal));
        QVERIFY(frame(QStringLiteral("Default")).isUndefined()); // taken out again, as it was
        QCOMPARE(frame(QStringLiteral("Profile 1")), QJsonValue(true));
        QVERIFY(!QFileInfo::exists(journal));
        // Open in a browser (its lock, this machine, a living process: this test): left alone.
        QVERIFY(QFile::link(QSysInfo::machineHostName() + QStringLiteral("-") + QString::number(QCoreApplication::applicationPid()), data + QStringLiteral("/SingletonLock")));
        QVERIFY(Chromium::inUse(data));
        QVERIFY(!Chromium::useSystemTitleBar(data, journal));
        QCOMPARE(frame(QStringLiteral("Profile 1")), QJsonValue(true));
    }

    void noRuntimeNoChange()
    {
        QCOMPARE(Chromium::prepare(QStringLiteral("chromium"), {}), QStringLiteral("chromium"));
        QCOMPARE(Chromium::prepare(QStringLiteral("dolphin"), kRuntime), QStringLiteral("dolphin"));
    }
};

QTEST_GUILESS_MAIN(TestChromium)
#include "tst_chromium.moc"
