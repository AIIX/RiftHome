// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "app/AppCatalog.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFont>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>

#include <algorithm>
#include <cmath>

namespace RiftHome::App
{

namespace
{

//! The [Desktop Entry] group's keys (unlocalised).
QHash<QString, QString> readDesktopEntry(const QString &path)
{
    QHash<QString, QString> keys;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return keys;
    }
    QTextStream in(&file);
    bool inEntry = false;
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.startsWith(QLatin1Char('['))) {
            inEntry = line == QLatin1String("[Desktop Entry]");
            continue;
        }
        const int eq = line.indexOf(QLatin1Char('='));
        if (!inEntry || eq <= 0 || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const QString key = line.left(eq).trimmed();
        if (!key.contains(QLatin1Char('['))) {
            keys.insert(key, line.mid(eq + 1).trimmed());
        }
    }
    return keys;
}

bool listContains(const QString &list, const QString &value)
{
    return list.split(QLatin1Char(';'), Qt::SkipEmptyParts).contains(value);
}

} // namespace

QImage iconImage(const QString &name, const QString &appName, int size)
{
    // The app's own icon (apps install theirs into hicolor, which every system has; a desktop's theme only overrides).
    const QIcon icon = QFileInfo(name).isAbsolute() ? QIcon(name) : QIcon::fromTheme(name);
    QImage image(size, size, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    if (!icon.isNull()) {
        icon.paint(&p, QRect(0, 0, size, size));
        return image;
    }
    // None anywhere: Rift Home's own tile with the app's initial (no desktop icon theme needed).
    const QColor tile = QColor::fromHsv(int(qHash(appName) % 360), 110, 150);
    p.setPen(Qt::NoPen);
    p.setBrush(tile);
    p.drawRoundedRect(QRectF(size * 0.1, size * 0.1, size * 0.8, size * 0.8), size * 0.2, size * 0.2);
    QFont font;
    font.setPixelSize(int(size * 0.44));
    font.setWeight(QFont::DemiBold);
    p.setFont(font);
    p.setPen(Qt::white);
    p.drawText(QRectF(0, 0, size, size), Qt::AlignCenter, appName.left(1).toUpper());
    return image;
}

QImage settingsIcon(int size)
{
    QImage image(size, size, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(142, 146, 156));
    p.drawRoundedRect(QRectF(size * 0.1, size * 0.1, size * 0.8, size * 0.8), size * 0.2, size * 0.2);
    // A gear: eight teeth around a ring, a hole in the middle.
    const QPointF c(size / 2.0, size / 2.0);
    const double r = size * 0.21;
    QPainterPath gear;
    for (int i = 0; i < 8; ++i) {
        const double a = i * M_PI / 4.0;
        const QPointF dir(std::cos(a), std::sin(a));
        const QPointF side(-dir.y(), dir.x());
        QPolygonF tooth;
        tooth << c + dir * (r * 0.9) + side * (r * 0.3) << c + dir * (r * 1.35) + side * (r * 0.22) << c + dir * (r * 1.35) - side * (r * 0.22)
              << c + dir * (r * 0.9) - side * (r * 0.3);
        gear.addPolygon(tooth);
        gear.closeSubpath();
    }
    gear.addEllipse(c, r, r);
    gear.setFillRule(Qt::WindingFill);
    p.setBrush(QColor(250, 250, 252));
    p.drawPath(gear.simplified());
    p.setBrush(QColor(142, 146, 156));
    p.drawEllipse(c, r * 0.42, r * 0.42);
    return image;
}

QString commandFromExec(const QString &exec, bool terminal)
{
    QString command;
    for (int i = 0; i < exec.size(); ++i) {
        if (exec[i] == QLatin1Char('%') && i + 1 < exec.size()) {
            if (exec[i + 1] == QLatin1Char('%')) {
                command += QLatin1Char('%');
            }
            ++i; // a field code: dropped (the launcher opens apps without files)
            continue;
        }
        command += exec[i];
    }
    command = command.simplified();
    return terminal ? QStringLiteral("konsole -e %1").arg(command) : command;
}

void useDesktopIconTheme()
{
    // The icon theme the user chose, where a desktop wrote one down (Plasma's kdeglobals; optional), else Breeze if
    // installed; apps' own icons come from hicolor either way.
    const QSettings kdeglobals(QStandardPaths::locate(QStandardPaths::GenericConfigLocation, QStringLiteral("kdeglobals")),
                               QSettings::IniFormat);
    QIcon::setThemeName(kdeglobals.value(QStringLiteral("Icons/Theme"), QStringLiteral("breeze")).toString());
    QIcon::setFallbackThemeName(QStringLiteral("hicolor"));
    // Rift Home runs Qt without a desktop platform (offscreen), which knows no icon folders: the XDG ones.
    QStringList themePaths = QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, QStringLiteral("icons"),
                                                       QStandardPaths::LocateDirectory);
    themePaths.prepend(QDir::homePath() + QStringLiteral("/.icons"));
    QIcon::setThemeSearchPaths(themePaths);
    QIcon::setFallbackSearchPaths(QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, QStringLiteral("pixmaps"),
                                                            QStandardPaths::LocateDirectory));
}

std::vector<AppEntry> loadApps(const QStringList &favourites, int iconSize)
{
    useDesktopIconTheme();

    // Earlier folders win (the user's own entries over the system's), as the XDG rules say.
    QHash<QString, QString> files;
    const QStringList dirs = QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, QStringLiteral("applications"),
                                                       QStandardPaths::LocateDirectory);
    for (const QString &dir : dirs) {
        QDirIterator it(dir, {QStringLiteral("*.desktop")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            const QString id = QDir(dir).relativeFilePath(path).replace(QLatin1Char('/'), QLatin1Char('-'));
            if (!files.contains(id)) {
                files.insert(id, path);
            }
        }
    }
    std::vector<AppEntry> apps;
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        const QHash<QString, QString> e = readDesktopEntry(it.value());
        if (e.value(QStringLiteral("Type")) != QLatin1String("Application") || e.value(QStringLiteral("NoDisplay")) == QLatin1String("true")
            || e.value(QStringLiteral("Hidden")) == QLatin1String("true") || e.value(QStringLiteral("Exec")).isEmpty()
            || listContains(e.value(QStringLiteral("NotShowIn")), QStringLiteral("KDE"))
            || (e.contains(QStringLiteral("OnlyShowIn")) && !listContains(e.value(QStringLiteral("OnlyShowIn")), QStringLiteral("KDE")))) {
            continue;
        }
        const QString tryExec = e.value(QStringLiteral("TryExec"));
        if (!tryExec.isEmpty() && QStandardPaths::findExecutable(tryExec).isEmpty() && !QFileInfo::exists(tryExec)) {
            continue;
        }
        AppEntry app;
        app.id = it.key();
        app.name = e.value(QStringLiteral("Name"), it.key());
        app.command = commandFromExec(e.value(QStringLiteral("Exec")), e.value(QStringLiteral("Terminal")) == QLatin1String("true"));
        app.iconName = e.value(QStringLiteral("Icon"));
        apps.push_back(std::move(app));
    }
    // Favourites first, in their order; then by name.
    const auto rank = [&](const AppEntry &a) {
        const int f = int(favourites.indexOf(a.id));
        return f < 0 ? int(favourites.size()) : f;
    };
    std::sort(apps.begin(), apps.end(), [&](const AppEntry &a, const AppEntry &b) {
        const int ra = rank(a);
        const int rb = rank(b);
        return ra != rb ? ra < rb : QString::localeAwareCompare(a.name, b.name) < 0;
    });
    for (AppEntry &app : apps) {
        app.icon = iconImage(app.iconName, app.name, iconSize);
    }
    return apps;
}

} // namespace RiftHome::App
