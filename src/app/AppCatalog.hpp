// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QImage>
#include <QString>
#include <QStringList>

#include <vector>

namespace RiftHome::App
{

//! An installed application, as its .desktop file describes it.
struct AppEntry {
    QString id; //!< the desktop file id ("org.kde.dolphin.desktop")
    QString name;
    QString command; //!< a shell command line (the Exec line without its field codes)
    QString iconName;
    QImage icon; //!< drawn from the icon theme (premultiplied RGBA8888)
};

/*!
 * The applications the launcher offers: every shown Application entry of the XDG applications folders,
 * favourites first (in their order), then the rest by name. Icons are drawn here, on the GUI thread (QIcon is not
 * thread-safe); the XR thread only gets images.
 */
std::vector<AppEntry> loadApps(const QStringList &favourites, int iconSize);

//! Qt's icons from the desktop's icon theme (kdeglobals, else Breeze) and the XDG icon folders (offscreen and plain
//! Wayland Qt know none by themselves).
void useDesktopIconTheme();

//! An icon by theme name or path (@p size pixels square); none anywhere: a tile with @p appName's initial.
QImage iconImage(const QString &name, const QString &appName, int size);
//! Rift Home's own Settings icon: a gear on a grey tile.
QImage settingsIcon(int size);

//! An Exec line made a shell command: field codes (%f, %U, ...) dropped, "%%" kept as "%"; a terminal app in Konsole.
QString commandFromExec(const QString &exec, bool terminal);

} // namespace RiftHome::App
