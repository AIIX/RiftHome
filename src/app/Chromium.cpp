// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "app/Chromium.hpp"
#include "app/Command.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QSysInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <tuple>
#include <unistd.h>
#include <vector>

namespace RiftHome::App
{

namespace
{

bool isChromiumApp(const QString &id)
{
    static const QStringList apps{
        QStringLiteral("org.chromium.Chromium"), QStringLiteral("com.google.Chrome"), QStringLiteral("com.google.ChromeDev"),
        QStringLiteral("com.brave.Browser"), QStringLiteral("com.microsoft.Edge"), QStringLiteral("com.microsoft.EdgeDev"),
        QStringLiteral("com.vivaldi.Vivaldi"), QStringLiteral("io.github.ungoogled_software.ungoogled_chromium"),
    };
    return apps.contains(id);
}

//! The browser data folder's name under the configuration folder, by flatpak app id or program name.
QString dataFolderOf(const QString &name)
{
    static const QList<std::pair<QRegularExpression, QString>> folders{
        {QRegularExpression(QStringLiteral("^(org\\.chromium\\.Chromium|io\\.github\\.ungoogled_software\\.ungoogled_chromium|chromium.*|ungoogled-chromium)$")), QStringLiteral("chromium")},
        {QRegularExpression(QStringLiteral("^(com\\.google\\.Chrome|google-chrome|google-chrome-stable)$")), QStringLiteral("google-chrome")},
        {QRegularExpression(QStringLiteral("^google-chrome-beta$")), QStringLiteral("google-chrome-beta")},
        {QRegularExpression(QStringLiteral("^(com\\.google\\.ChromeDev|google-chrome-unstable)$")), QStringLiteral("google-chrome-unstable")},
        {QRegularExpression(QStringLiteral("^(com\\.brave\\.Browser|brave.*)$")), QStringLiteral("BraveSoftware/Brave-Browser")},
        {QRegularExpression(QStringLiteral("^(com\\.microsoft\\.Edge|microsoft-edge|microsoft-edge-stable)$")), QStringLiteral("microsoft-edge")},
        {QRegularExpression(QStringLiteral("^(com\\.vivaldi\\.Vivaldi|vivaldi.*)$")), QStringLiteral("vivaldi")},
    };
    for (const auto &[pattern, folder] : folders) {
        if (pattern.match(name).hasMatch()) {
            return folder;
        }
    }
    return {};
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    // Replaced whole (never half written); with the permissions it had (a profile's files are the user's only).
    const QFileDevice::Permissions permissions = QFile::exists(path) ? QFile::permissions(path) : QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit() && QFile::setPermissions(path, permissions);
}

//! The profiles of a browser data folder: the ones its Local State lists that have a Preferences file.
QStringList profilesOf(const QString &userDataDir)
{
    QStringList profiles;
    const QJsonObject cache = QJsonDocument::fromJson(readFile(userDataDir + QStringLiteral("/Local State"))).object().value(QStringLiteral("profile")).toObject().value(QStringLiteral("info_cache")).toObject();
    QStringList names = cache.keys();
    if (!names.contains(QStringLiteral("Default"))) {
        names << QStringLiteral("Default");
    }
    for (const QString &name : names) {
        if (QFileInfo::exists(userDataDir + QLatin1Char('/') + name + QStringLiteral("/Preferences"))) {
            profiles << name;
        }
    }
    return profiles;
}

//! browser.custom_chrome_frame in @p prefs set to @p value, or taken out (@p value undefined).
QByteArray withCustomFrame(const QByteArray &prefs, const QJsonValue &value, bool *ok)
{
    QJsonParseError error{};
    QJsonDocument document = QJsonDocument::fromJson(prefs, &error);
    *ok = error.error == QJsonParseError::NoError && document.isObject();
    if (!*ok) {
        return {};
    }
    QJsonObject root = document.object();
    QJsonObject browser = root.value(QStringLiteral("browser")).toObject();
    if (value.isUndefined()) {
        browser.remove(QStringLiteral("custom_chrome_frame"));
    } else {
        browser.insert(QStringLiteral("custom_chrome_frame"), value);
    }
    root.insert(QStringLiteral("browser"), browser);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

} // namespace

Chromium::Install Chromium::installOf(const QString &command)
{
    const std::vector<Command::Word> words = Command::words(command);
    const size_t program = Command::programIndex(words);
    if (program >= words.size()) {
        return Install::None;
    }
    const QString name = QFileInfo(words[program].text).fileName();
    if (name == QLatin1String("flatpak")) {
        size_t run = 0;
        size_t app = 0;
        return Command::flatpakRun(words, run, app) && isChromiumApp(words[app].text) ? Install::Flatpak : Install::None;
    }
    static const QRegularExpression browsers(QStringLiteral(
        "^(chromium|chromium-browser|chromium-freeworld|ungoogled-chromium|google-chrome(-stable|-beta|-unstable)?|"
        "brave|brave-browser(-stable|-beta|-nightly)?|microsoft-edge(-stable|-beta|-dev)?|vivaldi(-stable|-snapshot)?)$"));
    if (!browsers.match(name).hasMatch()) {
        return Install::None;
    }
    // A snap: its own program, or (Ubuntu) a /usr/bin script handing over to it.
    const QString given = words[program].text;
    const QString path = given.contains(QLatin1Char('/')) ? given : QStandardPaths::findExecutable(given);
    const QString resolved = QFileInfo(path).canonicalFilePath();
    if (path.startsWith(QLatin1String("/snap/")) || resolved.startsWith(QLatin1String("/snap/"))) {
        return Install::Snap;
    }
    QFile script(resolved);
    if (!resolved.isEmpty() && QFileInfo(resolved).size() < 8192 && script.open(QIODevice::ReadOnly)) {
        const QByteArray head = script.read(8192);
        if (head.startsWith("#!") && head.contains("/snap/bin/")) {
            return Install::Snap;
        }
    }
    return Install::Native;
}

QString Chromium::prepare(const QString &command, const Runtime &runtime)
{
    const Install install = installOf(command);
    if (install == Install::None || install == Install::Snap || runtime.json.isEmpty()) {
        return command;
    }
    const std::vector<Command::Word> words = Command::words(command);
    size_t run = 0;
    size_t app = Command::programIndex(words);
    if (install == Install::Flatpak && !Command::flatpakRun(words, run, app)) {
        return command;
    }
    // Edits as (place, length replaced, text), applied from the end so the earlier places stay right.
    std::vector<std::tuple<qsizetype, qsizetype, QString>> edits;
    QStringList added;
    // Chromium takes the last --enable-features it is given: the features go into that one when the command has one
    // (Flathub's launcher merges its own default features into it too).
    const QStringList features{QStringLiteral("OpenXR"), QStringLiteral("WebXRIncubations")};
    const Command::Word *enable = nullptr;
    bool forced = false;
    bool unfiltered = false;
    for (size_t i = app + 1; i < words.size(); ++i) {
        if (words[i].text.startsWith(QLatin1String("--enable-features="))) {
            enable = &words[i];
        }
        forced = forced || words[i].text.startsWith(QLatin1String("--force-webxr-runtime"));
        unfiltered = unfiltered || words[i].text == QLatin1String("--disable-seccomp-filter-sandbox") || words[i].text == QLatin1String("--no-sandbox");
    }
    if (enable) {
        QStringList list = enable->text.mid(qsizetype(qstrlen("--enable-features="))).split(QLatin1Char(','), Qt::SkipEmptyParts);
        const qsizetype before = list.size();
        for (const QString &feature : features) {
            if (!list.contains(feature)) {
                list << feature;
            }
        }
        if (list.size() != before) {
            edits.emplace_back(enable->begin, enable->end - enable->begin,
                               Command::quoted(QStringLiteral("--enable-features=") + list.join(QLatin1Char(','))));
        }
    } else {
        added << QStringLiteral("--enable-features=") + features.join(QLatin1Char(','));
    }
    if (!forced) {
        added << QStringLiteral("--force-webxr-runtime=openxr");
    }
    if (!unfiltered) {
        added << QStringLiteral("--disable-seccomp-filter-sandbox"); // the GPU driver in the XR process (see the header)
    }
    if (!added.isEmpty()) {
        edits.emplace_back(words[app].end, 0, QLatin1Char(' ') + Command::joined(added));
    }
    std::sort(edits.begin(), edits.end(), [](const auto &a, const auto &b) { return std::get<0>(a) > std::get<0>(b); });
    QString result = command;
    for (const auto &[at, length, text] : edits) {
        result.replace(at, length, text);
    }
    if (install == Install::Flatpak) {
        // For this start only: the library's folder, the runtime's socket, the runtime file (flatpak run's options win
        // over the user's overrides, which stay as they are).
        result = Command::withFlatpakOptions(result, {QStringLiteral("--filesystem=") + runtime.libraryDir + QStringLiteral(":ro"),
                                                      QStringLiteral("--filesystem=xdg-run/monado_comp_ipc"),
                                                      QStringLiteral("--env=XR_RUNTIME_JSON=") + runtime.json});
    }
    return result;
}

QString Chromium::userDataDir(const QString &command, const QProcessEnvironment &env)
{
    const Install install = installOf(command);
    if (install == Install::None || install == Install::Snap) {
        return {};
    }
    const std::vector<Command::Word> words = Command::words(command);
    size_t run = 0;
    size_t app = Command::programIndex(words);
    if (install == Install::Flatpak && !Command::flatpakRun(words, run, app)) {
        return {};
    }
    for (size_t i = app + 1; i < words.size(); ++i) {
        const QString &text = words[i].text;
        if (text.startsWith(QLatin1String("--user-data-dir="))) {
            return QDir::cleanPath(text.mid(qsizetype(qstrlen("--user-data-dir="))));
        }
        if (text == QLatin1String("--user-data-dir") && i + 1 < words.size()) {
            return QDir::cleanPath(words[i + 1].text);
        }
    }
    const QString home = env.value(QStringLiteral("HOME"), QDir::homePath());
    if (install == Install::Flatpak) {
        const QString folder = dataFolderOf(words[app].text);
        return folder.isEmpty() ? QString() : home + QStringLiteral("/.var/app/") + words[app].text + QStringLiteral("/config/") + folder;
    }
    const QString folder = dataFolderOf(QFileInfo(words[app].text).fileName());
    const QString config = env.value(QStringLiteral("XDG_CONFIG_HOME"), home + QStringLiteral("/.config"));
    return folder.isEmpty() ? QString() : config + QLatin1Char('/') + folder;
}

bool Chromium::inUse(const QString &userDataDir)
{
    // Its SingletonLock: a link to "<host>-<pid>" while a browser has the data open (left behind by a crash).
    const QFileInfo lock(userDataDir + QStringLiteral("/SingletonLock"));
    if (!lock.isSymLink()) {
        return false;
    }
    const QString target = lock.symLinkTarget().section(QLatin1Char('/'), -1);
    const qsizetype dash = target.lastIndexOf(QLatin1Char('-'));
    bool ok = false;
    const pid_t pid = dash < 0 ? 0 : target.mid(dash + 1).toInt(&ok);
    if (userDataDir.contains(QLatin1String("/.var/app/"))) {
        // A flatpak's: its pid is the sandbox's own; open while an instance of the app runs.
        const QString appId = userDataDir.section(QStringLiteral("/.var/app/"), 1).section(QLatin1Char('/'), 0, 0);
        QProcess ps;
        ps.start(QStringLiteral("flatpak"), {QStringLiteral("ps"), QStringLiteral("--columns=application")});
        if (!ps.waitForFinished(3000)) {
            return true; // not known: as open (nothing is changed)
        }
        return QString::fromUtf8(ps.readAllStandardOutput()).split(QLatin1Char('\n')).contains(appId);
    }
    if (!ok || pid <= 0 || target.left(dash) != QSysInfo::machineHostName()) {
        return true; // not known (another machine's, a sandbox's): as open
    }
    return ::kill(pid, 0) == 0 || errno == EPERM;
}

int Chromium::restoreAll()
{
    int waiting = 0;
    const QDir journals(journalDir());
    for (const QString &journal : journals.entryList({QStringLiteral("*.json")}, QDir::Files)) {
        waiting += restore(journals.filePath(journal)) ? 0 : 1;
    }
    return waiting;
}

QString Chromium::journalDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/rift-home/chromium-restore");
}

bool Chromium::useSystemTitleBar(const QString &userDataDir, const QString &journal)
{
    if (userDataDir.isEmpty() || inUse(userDataDir)) {
        return false;
    }
    const QStringList profiles = profilesOf(userDataDir);
    if (profiles.isEmpty()) {
        return false; // a first start: the browser makes its profile (its own frame until the next start)
    }
    // The values before, noted first (kept from an earlier start while the journal waits to be put back).
    if (!journal.isEmpty() && !QFileInfo::exists(journal)) {
        QJsonObject before;
        for (const QString &profile : profiles) {
            const QJsonObject prefs = QJsonDocument::fromJson(readFile(userDataDir + QLatin1Char('/') + profile + QStringLiteral("/Preferences"))).object();
            const QJsonValue value = prefs.value(QStringLiteral("browser")).toObject().value(QStringLiteral("custom_chrome_frame"));
            before.insert(profile, value.isUndefined() ? QJsonValue(QJsonValue::Null) : value); // null: it had none
        }
        QDir().mkpath(QFileInfo(journal).absolutePath());
        if (!writeFile(journal, QJsonDocument(QJsonObject{{QStringLiteral("userDataDir"), userDataDir}, {QStringLiteral("before"), before}}).toJson())) {
            return false;
        }
    }
    bool all = true;
    for (const QString &profile : profiles) {
        const QString path = userDataDir + QLatin1Char('/') + profile + QStringLiteral("/Preferences");
        bool ok = false;
        const QByteArray changed = withCustomFrame(readFile(path), QJsonValue(false), &ok);
        all = all && ok && writeFile(path, changed);
    }
    return all;
}

bool Chromium::restore(const QString &journal)
{
    const QJsonObject note = QJsonDocument::fromJson(readFile(journal)).object();
    const QString userDataDir = note.value(QStringLiteral("userDataDir")).toString();
    if (userDataDir.isEmpty()) {
        QFile::remove(journal); // nothing to put back
        return true;
    }
    if (inUse(userDataDir)) {
        return false;
    }
    const QJsonObject before = note.value(QStringLiteral("before")).toObject();
    for (auto it = before.begin(); it != before.end(); ++it) {
        const QString path = userDataDir + QLatin1Char('/') + it.key() + QStringLiteral("/Preferences");
        if (!QFileInfo::exists(path)) {
            continue; // the profile is gone
        }
        bool ok = false;
        // An absent value comes back from the journal as null: taken out again.
        const QByteArray restored = withCustomFrame(readFile(path), it.value().isNull() || it.value().isUndefined() ? QJsonValue(QJsonValue::Undefined) : it.value(), &ok);
        if (!ok || !writeFile(path, restored)) {
            return false;
        }
    }
    QFile::remove(journal);
    return true;
}

QString Chromium::withScale(const QString &command, double scale)
{
    const Install install = installOf(command);
    if (install == Install::None || scale <= 0.0) {
        return command;
    }
    const std::vector<Command::Word> words = Command::words(command);
    size_t run = 0;
    size_t app = Command::programIndex(words);
    if (install == Install::Flatpak && !Command::flatpakRun(words, run, app)) {
        return command;
    }
    for (size_t i = app + 1; i < words.size(); ++i) {
        const QString &text = words[i].text;
        if (text.startsWith(QLatin1String("--force-device-scale-factor")) || text == QLatin1String("--ozone-platform=x11")) {
            return command;
        }
    }
    QString result = command;
    result.insert(words[app].end, QStringLiteral(" --force-device-scale-factor=") + QString::number(scale, 'g', 4));
    return result;
}

} // namespace RiftHome::App
