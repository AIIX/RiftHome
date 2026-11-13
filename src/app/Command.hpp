// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QString>
#include <QStringList>

#include <vector>

namespace RiftHome::App::Command
{

/*!
 * The launcher's command lines (run through sh) read as the shell reads them, and changed in place: what is not changed
 * stays exactly as written (sh still expands what it would have).
 */

//! A word of a command, and where it stands in it.
struct Word {
    QString text;
    qsizetype begin = 0;
    qsizetype end = 0; //!< one past its last character
};

//! @p command's words (quotes, backslashes).
std::vector<Word> words(const QString &command);
//! Where the program is: after "env", "exec" and NAME=value words (words().size() when none).
size_t programIndex(const std::vector<Word> &words);
//! In "flatpak [options] run [options] APP [arguments]": the places of "run" and of the app's id; false when not that.
bool flatpakRun(const std::vector<Word> &words, size_t &run, size_t &app);
//! The flatpak's app id @p command runs; empty: not "flatpak run".
QString flatpakApp(const QString &command);
//! A word for sh: as it is when nothing in it is special, else in single quotes.
QString quoted(const QString &word);
QString joined(const QStringList &words);
//! @p command with @p options given to flatpak run (after "run"), those it has already left out; unchanged when not
//! "flatpak run" or it has them all.
QString withFlatpakOptions(const QString &command, const QStringList &options);

} // namespace RiftHome::App::Command
