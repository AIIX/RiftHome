// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "app/FirefoxPrefs.hpp"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

using namespace RiftHome::App;

namespace
{
void write(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(bytes);
}

QByteArray read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

const FirefoxPrefs::Prefs kPrefs = {{QStringLiteral("gfx.wayland.hdr"), QStringLiteral("true")},
                                    {QStringLiteral("media.hardware-video-decoding.force-enabled"), QStringLiteral("true")}};
} // namespace

class TestFirefoxPrefs : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void recognisesFirefox()
    {
        QVERIFY(FirefoxPrefs::isFirefox(QStringLiteral("firefox %u")));
        QVERIFY(FirefoxPrefs::isFirefox(QStringLiteral("/usr/lib/firefox/firefox https://www.youtube.com")));
        QVERIFY(FirefoxPrefs::isFirefox(QStringLiteral("env MOZ_LOG=Dmabuf:5 firefox-esr")));
        QVERIFY(!FirefoxPrefs::isFirefox(QStringLiteral("dolphin")));
        QVERIFY(!FirefoxPrefs::isFirefox(QStringLiteral("firefoxlike")));
        QCOMPARE(FirefoxPrefs::installOf(QStringLiteral("/snap/bin/firefox %u")), FirefoxPrefs::Install::Snap);
        QCOMPARE(FirefoxPrefs::installOf(QStringLiteral("/usr/bin/flatpak run --branch=stable --arch=x86_64 --command=firefox org.mozilla.firefox @@u %u @@")),
                 FirefoxPrefs::Install::Flatpak);
        QCOMPARE(FirefoxPrefs::installOf(QStringLiteral("flatpak run org.kde.dolphin")), FirefoxPrefs::Install::None);
        // The flatpak's sandbox gets the video variables.
        QProcessEnvironment env;
        env.insert(QStringLiteral("LIBVA_DRIVER_NAME"), QStringLiteral("nvidia"));
        QCOMPARE(FirefoxPrefs::withSandboxEnvironment(QStringLiteral("flatpak run org.mozilla.firefox"), env, {QStringLiteral("LIBVA_DRIVER_NAME")}),
                 QStringLiteral("flatpak run '--env=LIBVA_DRIVER_NAME=nvidia' org.mozilla.firefox"));
        QCOMPARE(FirefoxPrefs::withSandboxEnvironment(QStringLiteral("firefox"), env, {QStringLiteral("LIBVA_DRIVER_NAME")}), QStringLiteral("firefox"));
        QCOMPARE(FirefoxPrefs::profileInCommand(QStringLiteral("firefox --no-remote --profile '/a b/c' url")), QStringLiteral("/a b/c"));
        QCOMPARE(FirefoxPrefs::profileInCommand(QStringLiteral("firefox --profile=/x")), QStringLiteral("/x"));
        QVERIFY(FirefoxPrefs::profileInCommand(QStringLiteral("firefox url")).isEmpty());
        // As the launcher adds it (a quote in the path too).
        QCOMPARE(FirefoxPrefs::profileInCommand(QStringLiteral("firefox %u --profile '/h/it'\\''s'")), QStringLiteral("/h/it's"));
        QCOMPARE(FirefoxPrefs::words(QStringLiteral("a \"b c\" d\\ e")), (QStringList{QStringLiteral("a"), QStringLiteral("b c"), QStringLiteral("d e")}));
    }

    void findsTheDefaultProfile()
    {
        QTemporaryDir root;
        QDir(root.path()).mkpath(QStringLiteral("abc.default-release"));
        QDir(root.path()).mkpath(QStringLiteral("old.default"));
        // The installation's default wins over the profile marked default.
        write(root.filePath(QStringLiteral("profiles.ini")),
              "[Profile1]\nName=default\nIsRelative=1\nPath=old.default\nDefault=1\n\n"
              "[Profile0]\nName=default-release\nIsRelative=1\nPath=abc.default-release\n\n"
              "[Install4F96D1932A9F858E]\nDefault=abc.default-release\nLocked=1\n\n[General]\nStartWithLastProfile=1\nVersion=2\n");
        QCOMPARE(FirefoxPrefs::defaultProfile(root.path()), root.filePath(QStringLiteral("abc.default-release")));
        // Without one, the profile marked default.
        write(root.filePath(QStringLiteral("profiles.ini")),
              "[Profile1]\nName=default\nIsRelative=1\nPath=old.default\nDefault=1\n\n"
              "[Profile0]\nName=default-release\nIsRelative=1\nPath=abc.default-release\n");
        QCOMPARE(FirefoxPrefs::defaultProfile(root.path()), root.filePath(QStringLiteral("old.default")));
    }

    void makesAFirstProfile()
    {
        QTemporaryDir home;
        QProcessEnvironment env;
        env.insert(QStringLiteral("HOME"), home.path());
        const QString root = FirefoxPrefs::profilesRoot(env);
        QCOMPARE(root, home.filePath(QStringLiteral(".config/mozilla/firefox")));
        QVERIFY(FirefoxPrefs::defaultProfile(root).isEmpty());
        const QString made = FirefoxPrefs::createProfile(root);
        QVERIFY(!made.isEmpty());
        QCOMPARE(FirefoxPrefs::defaultProfile(root), made);
        // A snap's and a flatpak's own places.
        QCOMPARE(FirefoxPrefs::profilesRoot(env, FirefoxPrefs::Install::Snap), home.filePath(QStringLiteral("snap/firefox/common/.mozilla/firefox")));
        QCOMPARE(FirefoxPrefs::profilesRoot(env, FirefoxPrefs::Install::Flatpak),
                 home.filePath(QStringLiteral(".var/app/org.mozilla.firefox/config/mozilla/firefox")));
        QDir(home.path()).mkpath(QStringLiteral(".var/app/org.mozilla.firefox/.mozilla/firefox"));
        write(home.filePath(QStringLiteral(".var/app/org.mozilla.firefox/.mozilla/firefox/profiles.ini")), "[General]\nVersion=2\n");
        QCOMPARE(FirefoxPrefs::profilesRoot(env, FirefoxPrefs::Install::Flatpak), home.filePath(QStringLiteral(".var/app/org.mozilla.firefox/.mozilla/firefox")));
        // An older Firefox's folder is used where only it has profiles.
        QTemporaryDir legacyHome;
        QDir(legacyHome.path()).mkpath(QStringLiteral(".mozilla/firefox"));
        write(legacyHome.filePath(QStringLiteral(".mozilla/firefox/profiles.ini")), "[General]\nVersion=2\n");
        env.insert(QStringLiteral("HOME"), legacyHome.path());
        QCOMPARE(FirefoxPrefs::profilesRoot(env), legacyHome.filePath(QStringLiteral(".mozilla/firefox")));
    }

    void ownProfileComesBackExactly()
    {
        QTemporaryDir dir;
        const QString profile = dir.filePath(QStringLiteral("p"));
        QDir().mkpath(profile);
        const QByteArray userJs = "user_pref(\"browser.startup.page\", 3);\n";
        const QByteArray prefsJs = "// Mozilla User Preferences\n\nuser_pref(\"app.update.lastUpdateTime\", 1);\n"
                                   "user_pref(\"gfx.wayland.hdr\", false);\nuser_pref(\"zoom\", 2);\n";
        write(profile + QStringLiteral("/user.js"), userJs);
        write(profile + QStringLiteral("/prefs.js"), prefsJs);
        const QString journal = dir.filePath(QStringLiteral("journal/p.json"));
        QVERIFY(FirefoxPrefs::apply(profile, kPrefs, journal));
        QVERIFY(QFile::exists(journal));
        QVERIFY(read(profile + QStringLiteral("/user.js")).contains("media.hardware-video-decoding.force-enabled\", true"));
        QVERIFY(read(profile + QStringLiteral("/user.js")).startsWith(userJs));
        // A second start in the same session keeps the first notes.
        QVERIFY(FirefoxPrefs::apply(profile, kPrefs, journal));
        // Firefox ran and wrote what it had.
        write(profile + QStringLiteral("/prefs.js"), "// Mozilla User Preferences\n\nuser_pref(\"app.update.lastUpdateTime\", 1);\n"
                                                     "user_pref(\"gfx.wayland.hdr\", true);\nuser_pref(\"media.hardware-video-decoding.force-enabled\", true);\n"
                                                     "user_pref(\"zoom\", 2);\n");
        QVERIFY(FirefoxPrefs::restore(journal));
        QVERIFY(!QFile::exists(journal));
        QCOMPARE(read(profile + QStringLiteral("/user.js")), userJs);
        const QByteArray restored = read(profile + QStringLiteral("/prefs.js"));
        QVERIFY(restored.contains("user_pref(\"gfx.wayland.hdr\", false);"));
        QVERIFY(!restored.contains("force-enabled"));
        QVERIFY(restored.contains("user_pref(\"zoom\", 2);"));
        QVERIFY(restored.contains("app.update.lastUpdateTime"));
    }

    void noUserJsBeforeMeansNoneAfter()
    {
        QTemporaryDir dir;
        const QString profile = dir.filePath(QStringLiteral("p"));
        QDir().mkpath(profile);
        const QString journal = dir.filePath(QStringLiteral("j.json"));
        QVERIFY(FirefoxPrefs::apply(profile, kPrefs, journal));
        QVERIFY(QFile::exists(profile + QStringLiteral("/user.js")));
        QVERIFY(FirefoxPrefs::restore(journal));
        QVERIFY(!QFile::exists(profile + QStringLiteral("/user.js")));
    }

    void waitsWhileFirefoxHasItOpen()
    {
        QTemporaryDir dir;
        const QString profile = dir.filePath(QStringLiteral("p"));
        QDir().mkpath(profile);
        const QString journal = dir.filePath(QStringLiteral("j.json"));
        QVERIFY(FirefoxPrefs::apply(profile, kPrefs, journal));
        // A lock of a process that is not Firefox (this test): not taken for one.
        QVERIFY(::symlink(QByteArray("127.0.0.1:+" + QByteArray::number(getpid())).constData(),
                          QFile::encodeName(profile + QStringLiteral("/lock")).constData()) == 0);
        QVERIFY(!FirefoxPrefs::inUse(profile));
        // A stale lock (a crash's) does not hold it either.
        QFile::remove(profile + QStringLiteral("/lock"));
        QVERIFY(::symlink("127.0.0.1:+999999999", QFile::encodeName(profile + QStringLiteral("/lock")).constData()) == 0);
        QVERIFY(!FirefoxPrefs::inUse(profile));
        QVERIFY(FirefoxPrefs::restore(journal));
    }

    void ownProfileKeepsRiftHomesBlockOnlyOnce()
    {
        QTemporaryDir dir;
        const QString profile = dir.filePath(QStringLiteral("p"));
        QDir().mkpath(profile);
        // Rift Home's own (isolated) profiles: no journal, the block replaced each start, never doubled.
        QVERIFY(FirefoxPrefs::apply(profile, kPrefs));
        QVERIFY(FirefoxPrefs::apply(profile, kPrefs));
        QCOMPARE(read(profile + QStringLiteral("/user.js")).count("Rift Home: begin"), 1);
        QCOMPARE(read(profile + QStringLiteral("/user.js")).count("gfx.wayland.hdr"), 1);
    }
};

QTEST_GUILESS_MAIN(TestFirefoxPrefs)
#include "tst_firefoxprefs.moc"
