// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "app/FirefoxPrefs.hpp"
#include "app/Command.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

#include <cerrno>
#include <csignal>
#include <unistd.h>

namespace RiftHome::App
{

namespace
{

const QByteArray kBegin = "// Rift Home: begin. Set by Rift Home for the Firefox it starts (HDR video, the GPU's decoding).";
const QByteArray kEnd = "// Rift Home: end";

QByteArray readAll(const QString &path, bool *exists = nullptr)
{
    QFile file(path);
    const bool ok = file.open(QIODevice::ReadOnly);
    if (exists) {
        *exists = ok;
    }
    return ok ? file.readAll() : QByteArray();
}

bool writeAll(const QString &path, const QByteArray &bytes)
{
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

//! @p text without Rift Home's block (and the blank line before it).
QByteArray withoutBlock(const QByteArray &text)
{
    const qsizetype begin = text.indexOf(kBegin);
    if (begin < 0) {
        return text;
    }
    qsizetype end = text.indexOf(kEnd, begin);
    end = end < 0 ? text.size() : end + kEnd.size();
    if (end < text.size() && text.at(end) == '\n') {
        ++end;
    }
    qsizetype from = begin;
    if (from > 0 && text.at(from - 1) == '\n' && (from == 1 || text.at(from - 2) == '\n')) {
        --from; // the blank line apply() put before it
    }
    return text.left(from) + text.mid(end);
}

QByteArray prefLinePrefix(const QString &name)
{
    return "user_pref(\"" + name.toUtf8() + "\",";
}

//! prefs.js's line for @p name, or null.
QByteArray prefLine(const QByteArray &prefsJs, const QString &name)
{
    const QByteArray prefix = prefLinePrefix(name);
    for (const QByteArray &line : prefsJs.split('\n')) {
        if (line.startsWith(prefix)) {
            return line;
        }
    }
    return {};
}

QStringList commandTokens(const QString &command)
{
    QStringList tokens = FirefoxPrefs::words(command);
    // "env A=1 B=2 firefox ..." and "A=1 firefox ...": the program comes after them.
    while (!tokens.isEmpty() && (tokens.first() == QLatin1String("env") || tokens.first() == QLatin1String("exec")
                                 || (tokens.first().contains(QLatin1Char('=')) && !tokens.first().startsWith(QLatin1Char('-'))))) {
        tokens.removeFirst();
    }
    return tokens;
}

QString resolve(const QString &root, const QString &path, bool relative)
{
    return relative || !QDir::isAbsolutePath(path) ? QDir(root).filePath(path) : path;
}

} // namespace

QStringList FirefoxPrefs::words(const QString &command)
{
    QStringList words;
    for (const Command::Word &word : Command::words(command)) {
        words << word.text;
    }
    return words;
}

FirefoxPrefs::Install FirefoxPrefs::installOf(const QString &command)
{
    const QStringList tokens = commandTokens(command);
    if (tokens.isEmpty()) {
        return Install::None;
    }
    const QString program = QFileInfo(tokens.first()).fileName();
    if (program == QLatin1String("flatpak")) {
        // flatpak run [options] org.mozilla.firefox [args]
        const qsizetype run = tokens.indexOf(QStringLiteral("run"));
        for (qsizetype i = run + 1; run > 0 && i < tokens.size(); ++i) {
            if (!tokens.at(i).startsWith(QLatin1Char('-'))) {
                return tokens.at(i) == QLatin1String("org.mozilla.firefox") ? Install::Flatpak : Install::None;
            }
        }
        return Install::None;
    }
    static const QRegularExpression name(QStringLiteral("^firefox(-esr|-bin|-developer-edition|-nightly|-beta)?$"));
    if (!name.match(program).hasMatch()) {
        return Install::None;
    }
    // A snap: its own program, or (Ubuntu) a /usr/bin/firefox script handing over to it.
    const QString path = tokens.first().contains(QLatin1Char('/')) ? tokens.first() : QStandardPaths::findExecutable(tokens.first());
    const QString resolved = QFileInfo(path).canonicalFilePath();
    if (resolved.startsWith(QLatin1String("/snap/")) || path.startsWith(QLatin1String("/snap/"))) {
        return Install::Snap;
    }
    QFile script(resolved);
    if (QFileInfo(resolved).size() < 8192 && script.open(QIODevice::ReadOnly)) {
        const QByteArray head = script.read(8192);
        if (head.startsWith("#!") && head.contains("/snap/bin/firefox")) {
            return Install::Snap;
        }
    }
    return Install::Native;
}

QString FirefoxPrefs::withSandboxEnvironment(const QString &command, const QProcessEnvironment &env, const QStringList &names)
{
    if (installOf(command) != Install::Flatpak) {
        return command;
    }
    const qsizetype run = command.indexOf(QLatin1String(" run"));
    if (run < 0) {
        return command;
    }
    QString flags;
    for (const QString &name : names) {
        if (env.contains(name)) {
            QString value = env.value(name);
            value.replace(QLatin1Char('\''), QLatin1String("'\\''"));
            flags += QStringLiteral(" '--env=%1=%2'").arg(name, value);
        }
    }
    QString result = command;
    result.insert(run + 4, flags);
    return result;
}

QString FirefoxPrefs::profileInCommand(const QString &command)
{
    const QStringList tokens = commandTokens(command);
    for (qsizetype i = 1; i < tokens.size(); ++i) {
        const QString &token = tokens.at(i);
        if ((token == QLatin1String("--profile") || token == QLatin1String("-profile")) && i + 1 < tokens.size()) {
            return tokens.at(i + 1);
        }
        if (token.startsWith(QLatin1String("--profile="))) {
            return token.mid(10);
        }
    }
    return {};
}

QString FirefoxPrefs::profilesRoot(const QProcessEnvironment &env, Install install)
{
    const QString home = env.value(QStringLiteral("HOME"), QDir::homePath());
    if (install == Install::Snap) {
        return home + QStringLiteral("/snap/firefox/common/.mozilla/firefox");
    }
    QString config = env.value(QStringLiteral("XDG_CONFIG_HOME"), home + QStringLiteral("/.config"));
    QString legacy = home + QStringLiteral("/.mozilla/firefox");
    if (install == Install::Flatpak) {
        // The sandbox's own home and configuration folders.
        const QString app = home + QStringLiteral("/.var/app/org.mozilla.firefox");
        config = app + QStringLiteral("/config");
        legacy = app + QStringLiteral("/.mozilla/firefox");
    }
    const QString xdg = config + QStringLiteral("/mozilla/firefox");
    if (!QFileInfo::exists(xdg + QStringLiteral("/profiles.ini")) && QFileInfo::exists(legacy + QStringLiteral("/profiles.ini"))) {
        return legacy;
    }
    return xdg;
}

QString FirefoxPrefs::defaultProfile(const QString &root)
{
    const QString ini = root + QStringLiteral("/profiles.ini");
    if (!QFileInfo::exists(ini)) {
        return {};
    }
    QSettings settings(ini, QSettings::IniFormat);
    const QStringList groups = settings.childGroups();
    for (const QString &group : groups) {
        if (group.startsWith(QLatin1String("Install"))) {
            const QString path = resolve(root, settings.value(group + QStringLiteral("/Default")).toString(), false);
            if (!settings.value(group + QStringLiteral("/Default")).toString().isEmpty() && QFileInfo(path).isDir()) {
                return path;
            }
        }
    }
    QStringList profiles;
    for (const QString &group : groups) {
        if (!group.startsWith(QLatin1String("Profile"))) {
            continue;
        }
        const QString path = resolve(root, settings.value(group + QStringLiteral("/Path")).toString(),
                                     settings.value(group + QStringLiteral("/IsRelative"), 1).toInt() != 0);
        if (settings.value(group + QStringLiteral("/Default")).toInt() == 1 && QFileInfo(path).isDir()) {
            return path;
        }
        if (QFileInfo(path).isDir()) {
            profiles << path;
        }
    }
    return profiles.size() == 1 ? profiles.first() : QString();
}

QString FirefoxPrefs::createProfile(const QString &root)
{
    if (!QDir().mkpath(root)) {
        return {};
    }
    QString salt;
    for (int i = 0; i < 8; ++i) {
        salt += QChar(QLatin1Char("abcdefghijklmnopqrstuvwxyz0123456789"[QRandomGenerator::global()->bounded(36)]));
    }
    const QString name = salt + QStringLiteral(".default-release");
    if (!QDir(root).mkpath(name)) {
        return {};
    }
    const QString ini = root + QStringLiteral("/profiles.ini");
    QSettings settings(ini, QSettings::IniFormat);
    int index = 0;
    while (settings.childGroups().contains(QStringLiteral("Profile%1").arg(index))) {
        ++index;
    }
    const QString group = QStringLiteral("Profile%1/").arg(index);
    settings.setValue(group + QStringLiteral("Name"), QStringLiteral("default-release"));
    settings.setValue(group + QStringLiteral("IsRelative"), 1);
    settings.setValue(group + QStringLiteral("Path"), name);
    settings.setValue(group + QStringLiteral("Default"), 1);
    if (!settings.contains(QStringLiteral("General/StartWithLastProfile"))) {
        settings.setValue(QStringLiteral("General/StartWithLastProfile"), 1);
        settings.setValue(QStringLiteral("General/Version"), 2);
    }
    settings.sync();
    return settings.status() == QSettings::NoError ? QDir(root).filePath(name) : QString();
}

bool FirefoxPrefs::apply(const QString &profile, const Prefs &prefs, const QString &journal)
{
    if (profile.isEmpty() || !QFileInfo(profile).isDir()) {
        return false;
    }
    const QString userJsPath = profile + QStringLiteral("/user.js");
    if (!journal.isEmpty()) {
        // What to put back, noted before anything changes (once: a second start in the same session keeps the first
        // notes; names not noted yet are added).
        bool exists = false;
        QJsonObject notes = QJsonDocument::fromJson(readAll(journal, &exists)).object();
        if (!exists) {
            bool userJsExists = false;
            const QByteArray userJs = readAll(userJsPath, &userJsExists);
            notes.insert(QStringLiteral("profile"), profile);
            notes.insert(QStringLiteral("userJs"), userJsExists ? QJsonValue(QString::fromUtf8(userJs)) : QJsonValue());
        }
        QJsonObject lines = notes.value(QStringLiteral("prefs")).toObject();
        const QByteArray prefsJs = readAll(profile + QStringLiteral("/prefs.js"));
        bool added = !exists;
        for (const auto &[name, value] : prefs) {
            if (!lines.contains(name)) {
                const QByteArray line = prefLine(prefsJs, name);
                lines.insert(name, line.isNull() ? QJsonValue() : QJsonValue(QString::fromUtf8(line)));
                added = true;
            }
        }
        notes.insert(QStringLiteral("prefs"), lines);
        if (added) {
            QDir().mkpath(QFileInfo(journal).absolutePath());
            if (!writeAll(journal, QJsonDocument(notes).toJson())) {
                return false; // nothing changed without a way back
            }
        }
    }
    QByteArray text = withoutBlock(readAll(userJsPath));
    if (!text.isEmpty() && !text.endsWith('\n')) {
        text += '\n';
    }
    if (!text.isEmpty()) {
        text += '\n';
    }
    text += kBegin + '\n';
    for (const auto &[name, value] : prefs) {
        text += "user_pref(\"" + name.toUtf8() + "\", " + value.toUtf8() + ");\n";
    }
    text += kEnd + '\n';
    return writeAll(userJsPath, text);
}

bool FirefoxPrefs::restore(const QString &journal)
{
    bool exists = false;
    const QJsonObject notes = QJsonDocument::fromJson(readAll(journal, &exists)).object();
    if (!exists) {
        return true;
    }
    const QString profile = notes.value(QStringLiteral("profile")).toString();
    if (profile.isEmpty() || !QFileInfo(profile).isDir()) {
        QFile::remove(journal); // the profile is gone: nothing to put back
        return true;
    }
    if (inUse(profile)) {
        return false;
    }
    // user.js: Rift Home's block out (anything else in it is the user's); no file if there was none.
    const QString userJsPath = profile + QStringLiteral("/user.js");
    const QByteArray userJs = withoutBlock(readAll(userJsPath));
    if (notes.value(QStringLiteral("userJs")).isNull() && userJs.trimmed().isEmpty()) {
        QFile::remove(userJsPath);
    } else if (!writeAll(userJsPath, userJs)) {
        return false;
    }
    // prefs.js: Firefox wrote the values it had into it; each noted name's line as it was before (or none).
    const QString prefsJsPath = profile + QStringLiteral("/prefs.js");
    bool prefsJsExists = false;
    const QByteArray prefsJs = readAll(prefsJsPath, &prefsJsExists);
    if (prefsJsExists) {
        const QJsonObject lines = notes.value(QStringLiteral("prefs")).toObject();
        QList<QByteArray> kept;
        for (const QByteArray &line : prefsJs.split('\n')) {
            bool noted = false;
            for (auto it = lines.begin(); it != lines.end() && !noted; ++it) {
                noted = line.startsWith(prefLinePrefix(it.key()));
            }
            if (!noted) {
                kept << line;
            }
        }
        while (!kept.isEmpty() && kept.last().isEmpty()) {
            kept.removeLast();
        }
        for (auto it = lines.begin(); it != lines.end(); ++it) {
            if (!it.value().isNull()) {
                kept << it.value().toString().toUtf8();
            }
        }
        if (!writeAll(prefsJsPath, kept.join('\n') + '\n')) {
            return false;
        }
    }
    QFile::remove(journal);
    return true;
}

QString FirefoxPrefs::journalDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/rift-home/firefox-restore");
}

int FirefoxPrefs::restoreAll()
{
    const QDir dir(journalDir());
    int waiting = 0;
    for (const QString &journal : dir.entryList({QStringLiteral("*.json")}, QDir::Files)) {
        waiting += restore(dir.filePath(journal)) ? 0 : 1;
    }
    return waiting;
}

bool FirefoxPrefs::inUse(const QString &profile)
{
    // Firefox's lock on Linux: a symlink "lock" -> "<address>:+<pid>" while it runs (left behind by a crash).
    char target[256] = {};
    const QByteArray lock = QFile::encodeName(profile + QStringLiteral("/lock"));
    const ssize_t length = ::readlink(lock.constData(), target, sizeof(target) - 1);
    if (length <= 0) {
        return false;
    }
    const QByteArray value(target, length);
    const qsizetype plus = value.lastIndexOf('+');
    bool ok = false;
    const pid_t pid = plus < 0 ? 0 : value.mid(plus + 1).toInt(&ok);
    if (!ok || pid <= 0 || (::kill(pid, 0) != 0 && errno != EPERM)) {
        return false;
    }
    // Not some other program that got the same number since: Firefox's own program (firefox, firefox-bin, ...).
    const QByteArray program = readAll(QStringLiteral("/proc/%1/cmdline").arg(pid)).split('\0').value(0);
    return QFileInfo(QFile::decodeName(program)).fileName().startsWith(QLatin1String("firefox"));
}

} // namespace RiftHome::App
