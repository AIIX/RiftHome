// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "app/Command.hpp"

#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

namespace RiftHome::App::Command
{

namespace
{

//! flatpak run's options that take a value as the next word when not written with '=' (flatpak-run(1)).
bool takesValue(const QString &option)
{
    static const QStringList options{
        QStringLiteral("--arch"), QStringLiteral("--command"), QStringLiteral("--branch"), QStringLiteral("--cwd"),
        QStringLiteral("--runtime"), QStringLiteral("--runtime-version"), QStringLiteral("--commit"),
        QStringLiteral("--runtime-commit"), QStringLiteral("--parent-pid"), QStringLiteral("--app-path"),
        QStringLiteral("--usr-path"), QStringLiteral("--instance-id-fd"), QStringLiteral("--share"), QStringLiteral("--unshare"),
        QStringLiteral("--socket"), QStringLiteral("--nosocket"), QStringLiteral("--device"), QStringLiteral("--nodevice"),
        QStringLiteral("--allow"), QStringLiteral("--disallow"), QStringLiteral("--filesystem"), QStringLiteral("--nofilesystem"),
        QStringLiteral("--add-policy"), QStringLiteral("--remove-policy"), QStringLiteral("--env"), QStringLiteral("--unset-env"),
        QStringLiteral("--env-fd"), QStringLiteral("--own-name"), QStringLiteral("--talk-name"), QStringLiteral("--no-talk-name"),
        QStringLiteral("--system-own-name"), QStringLiteral("--system-talk-name"), QStringLiteral("--system-no-talk-name"),
        QStringLiteral("--persist"),
    };
    return options.contains(option);
}

} // namespace

std::vector<Word> words(const QString &command)
{
    std::vector<Word> words;
    Word word;
    bool inWord = false;
    QChar quote;
    for (qsizetype i = 0; i < command.size(); ++i) {
        const QChar c = command.at(i);
        if (!inWord && quote.isNull() && !c.isSpace()) {
            word.begin = i;
        }
        if (!quote.isNull()) {
            if (c == quote) {
                quote = QChar();
            } else if (c == QLatin1Char('\\') && quote == QLatin1Char('"') && i + 1 < command.size()) {
                word.text += command.at(++i);
            } else {
                word.text += c;
            }
        } else if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
            quote = c;
            inWord = true;
        } else if (c == QLatin1Char('\\') && i + 1 < command.size()) {
            word.text += command.at(++i);
            inWord = true;
        } else if (c.isSpace()) {
            if (inWord) {
                word.end = i;
                words.push_back(word);
                word = Word();
                inWord = false;
            }
        } else {
            word.text += c;
            inWord = true;
        }
    }
    if (inWord) {
        word.end = command.size();
        words.push_back(word);
    }
    return words;
}

size_t programIndex(const std::vector<Word> &words)
{
    size_t i = 0;
    while (i < words.size()
           && (words[i].text == QLatin1String("env") || words[i].text == QLatin1String("exec")
               || (words[i].text.contains(QLatin1Char('=')) && !words[i].text.startsWith(QLatin1Char('-'))))) {
        ++i;
    }
    return i;
}

bool flatpakRun(const std::vector<Word> &words, size_t &run, size_t &app)
{
    const size_t program = programIndex(words);
    if (program >= words.size() || QFileInfo(words[program].text).fileName() != QLatin1String("flatpak")) {
        return false;
    }
    run = program + 1;
    while (run < words.size() && words[run].text.startsWith(QLatin1Char('-'))) {
        ++run; // flatpak's own options (--user, --system, -v) before "run"
    }
    if (run >= words.size() || words[run].text != QLatin1String("run")) {
        return false;
    }
    for (app = run + 1; app < words.size(); ++app) {
        const QString &text = words[app].text;
        if (!text.startsWith(QLatin1Char('-'))) {
            return true;
        }
        if (!text.contains(QLatin1Char('=')) && takesValue(text)) {
            ++app; // its value
        }
    }
    return false;
}

QString flatpakApp(const QString &command)
{
    const std::vector<Word> list = words(command);
    size_t run = 0;
    size_t app = 0;
    return flatpakRun(list, run, app) ? list[app].text : QString();
}

QString quoted(const QString &word)
{
    static const QRegularExpression plain(QStringLiteral("^[A-Za-z0-9_@%+=:,./-]+$"));
    if (plain.match(word).hasMatch()) {
        return word;
    }
    QString escaped = word;
    escaped.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

QString joined(const QStringList &words)
{
    QStringList parts;
    for (const QString &word : words) {
        parts << quoted(word);
    }
    return parts.join(QLatin1Char(' '));
}

QString withFlatpakOptions(const QString &command, const QStringList &options)
{
    const std::vector<Word> list = words(command);
    size_t run = 0;
    size_t app = 0;
    if (!flatpakRun(list, run, app)) {
        return command;
    }
    QStringList missing;
    for (const QString &option : options) {
        if (std::none_of(list.begin() + qsizetype(run) + 1, list.begin() + qsizetype(app), [&](const Word &w) { return w.text == option; })) {
            missing << option;
        }
    }
    if (missing.isEmpty()) {
        return command;
    }
    QString result = command;
    result.insert(list[run].end, QLatin1Char(' ') + joined(missing));
    return result;
}

} // namespace RiftHome::App::Command
