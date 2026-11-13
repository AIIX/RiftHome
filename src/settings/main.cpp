// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
// Rift Home's Settings: a window in Rift Home like any app, for the system (network, sound, date and
// time, power) and for Rift Home itself (frames, room, keyboard), without Plasma or GNOME.

#include "settings/Authentication.hpp"
#include "settings/Backends.hpp"
#include "graphics/RadianceHdr.hpp"

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QImageReader>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPainter>
#include <QPainterPath>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickImageProvider>
#include <QQuickStyle>
#include <QQuickWindow>

#include <memory>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <unistd.h>

namespace
{

//! "image://icon/dark/<name>" or ".../light/<name>": Rift Home's own icons, drawn here (no icon theme needed, the
//! same line style as the window frame's), white on the dark style, near-black on the light one.
class IconProvider : public QQuickImageProvider
{
public:
    IconProvider()
        : QQuickImageProvider(QQuickImageProvider::Image)
    {
    }
    QImage requestImage(const QString &id, QSize *size, const QSize &requested) override
    {
        const int side = requested.width() > 0 ? requested.width() : 64;
        const bool dark = id.section(QLatin1Char('/'), 0, 0) == QLatin1String("dark");
        const QString name = id.section(QLatin1Char('/'), 1);
        QImage image(side, side, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(side / 64.0, side / 64.0); // drawn in a 64 x 64 box
        const QColor ink = dark ? QColor(242, 242, 245) : QColor(28, 28, 30);
        p.setPen(QPen(ink, 4.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        if (name == QLatin1String("wifi")) {
            for (int i = 0; i < 3; ++i) {
                const double r = 10.0 + i * 10.0;
                p.drawArc(QRectF(32 - r, 46 - r, 2 * r, 2 * r), 45 * 16, 90 * 16);
            }
            p.setBrush(ink);
            p.drawEllipse(QPointF(32, 46), 3.5, 3.5);
        } else if (name == QLatin1String("bluetooth")) {
            // The rune: a B of two triangles on a staff.
            QPainterPath rune;
            rune.moveTo(22, 21);
            rune.lineTo(42, 41);
            rune.lineTo(32, 51);
            rune.lineTo(32, 13);
            rune.lineTo(42, 23);
            rune.lineTo(22, 43);
            p.drawPath(rune);
        } else if (name == QLatin1String("sound")) {
            QPainterPath body;
            body.moveTo(10, 26);
            body.lineTo(20, 26);
            body.lineTo(32, 15);
            body.lineTo(32, 49);
            body.lineTo(20, 38);
            body.lineTo(10, 38);
            body.closeSubpath();
            p.drawPath(body);
            p.drawArc(QRectF(30, 22, 16, 20), -50 * 16, 100 * 16);
            p.drawArc(QRectF(30, 13, 26, 38), -50 * 16, 100 * 16);
        } else if (name == QLatin1String("clock")) {
            p.drawEllipse(QPointF(32, 32), 22, 22);
            p.drawLine(QPointF(32, 32), QPointF(32, 18));
            p.drawLine(QPointF(32, 32), QPointF(42, 38));
        } else if (name == QLatin1String("room")) {
            p.drawRoundedRect(QRectF(8, 14, 48, 36), 6, 6);
            QPainterPath hills;
            hills.moveTo(10, 46);
            hills.lineTo(24, 30);
            hills.lineTo(34, 40);
            hills.lineTo(42, 32);
            hills.lineTo(54, 46);
            p.drawPath(hills);
            p.setBrush(ink);
            p.drawEllipse(QPointF(44, 23), 3.5, 3.5);
        } else if (name == QLatin1String("keyboard")) {
            p.drawRoundedRect(QRectF(6, 18, 52, 30), 6, 6);
            p.setPen(QPen(ink, 4.5, Qt::SolidLine, Qt::RoundCap));
            for (int row = 0; row < 2; ++row) {
                for (int col = 0; col < 5; ++col) {
                    p.drawPoint(QPointF(15 + col * 8.5, 26 + row * 7));
                }
            }
            p.drawLine(QPointF(22, 40.5), QPointF(42, 40.5));
        } else if (name == QLatin1String("controller")) {
            // A Touch controller: the ring over the handle, a thumbstick and a button on its face.
            p.drawEllipse(QPointF(30, 22), 17, 13);
            QPainterPath handle;
            handle.moveTo(26, 34);
            handle.cubicTo(24, 44, 26, 54, 32, 58);
            handle.cubicTo(38, 54, 40, 44, 38, 34);
            p.drawPath(handle);
            p.setBrush(ink);
            p.drawEllipse(QPointF(26, 21), 3.5, 3.5);
            p.drawEllipse(QPointF(37, 24), 2.5, 2.5);
        } else if (name == QLatin1String("power")) {
            p.drawArc(QRectF(12, 14, 40, 40), 125 * 16, 290 * 16);
            p.drawLine(QPointF(32, 8), QPointF(32, 32));
        } else if (name == QLatin1String("about")) {
            p.drawEllipse(QPointF(32, 32), 22, 22);
            p.drawLine(QPointF(32, 29), QPointF(32, 44));
            p.setBrush(ink);
            p.setPen(Qt::NoPen);
            p.drawEllipse(QPointF(32, 21), 3.2, 3.2);
        } else if (name == QLatin1String("lock")) {
            p.drawRoundedRect(QRectF(16, 28, 32, 24), 4, 4);
            p.drawArc(QRectF(22, 12, 20, 26), 0, 180 * 16);
            p.drawLine(QPointF(22, 25), QPointF(22, 28));
            p.drawLine(QPointF(42, 25), QPointF(42, 28));
        }
        p.end();
        if (size) {
            *size = image.size();
        }
        return image;
    }
};

//! "image://room/<path>": a room's thumbnail; a Radiance HDR panorama tone-mapped to show (QML cannot read .hdr).
class RoomThumbnailProvider : public QQuickImageProvider
{
public:
    RoomThumbnailProvider()
        : QQuickImageProvider(QQuickImageProvider::Image, QQmlImageProviderBase::ForceAsynchronousImageLoading)
    {
    }
    QImage requestImage(const QString &id, QSize *size, const QSize &requested) override
    {
        const int width = requested.width() > 0 ? requested.width() : 360;
        QImage image;
        if (id.endsWith(QLatin1String(".hdr"), Qt::CaseInsensitive)) {
            if (const auto hdr = RiftHome::Graphics::loadRadianceHdr(id)) {
                const float exposure = 1.0f; // as authored, as the room shows it
                const int height = std::max(1, width * hdr->height / std::max(1, hdr->width));
                image = QImage(width, height, QImage::Format_RGB32);
                for (int y = 0; y < height; ++y) {
                    auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
                    const int sy = y * hdr->height / height;
                    for (int x = 0; x < width; ++x) {
                        const float *p = &hdr->rgb[(size_t(sy) * size_t(hdr->width) + size_t(x * hdr->width / width)) * 3];
                        const auto tone = [exposure](float v) { // Reinhard, then sRGB's curve
                            v *= exposure;
                            v = v / (1.0f + v);
                            return int(std::clamp(std::pow(v, 1.0f / 2.2f) * 255.0f, 0.0f, 255.0f));
                        };
                        line[x] = qRgb(tone(p[0]), tone(p[1]), tone(p[2]));
                    }
                }
            }
        } else {
            // Decoded at about the thumbnail's size where the format can (JPEG): desktop wallpapers are 5K and more.
            QImageReader reader(id);
            reader.setAutoTransform(true);
            const QSize full = reader.size();
            if (full.isValid() && full.width() > width * 2) {
                reader.setScaledSize(QSize(width * 2, std::max(1, width * 2 * full.height() / full.width())));
            }
            image = reader.read().scaledToWidth(width, Qt::SmoothTransformation);
        }
        if (size) {
            *size = image.size();
        }
        return image;
    }
};

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("Settings"));
    QGuiApplication::setDesktopFileName(QStringLiteral("org.rifthome.settings")); // its app id: Rift Home finds it by it
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption pageOption(QStringLiteral("page"), QStringLiteral("The page to open (0 network, 1 Bluetooth ... 6 controllers, 7 power, 8 about)."), QStringLiteral("n"), QStringLiteral("-1"));
    const QCommandLineOption screenshotOption(QStringLiteral("screenshot"), QStringLiteral("Save the window as a PNG after 3 s and quit (previews)."), QStringLiteral("file"));
    const QCommandLineOption previewSheetOption(QStringLiteral("preview-sheet"), QStringLiteral("Open a sheet at start (previews): rooms."), QStringLiteral("name"));
    const QCommandLineOption screenshotHeightOption(QStringLiteral("screenshot-height"), QStringLiteral("The window's height for --screenshot (whole pages)."), QStringLiteral("pixels"));
    // Rift Home's own session: Settings waits in the background as the session's password agent; its
    // window comes for a prompt (and goes after), or when the dock asks for Settings.
    const QCommandLineOption agentOption(QStringLiteral("session-agent"), QStringLiteral("Wait without a window as the session's password agent."));
    parser.addOptions({pageOption, screenshotOption, screenshotHeightOption, previewSheetOption, agentOption});
    parser.process(app);
    const int page = parser.value(pageOption).toInt();

    // One Settings: a second start (the dock's network item) hands its page to the running one and ends; Rift Home
    // brings that window in front.
    const QString serverName = QStringLiteral("rift-home-settings-%1").arg(getuid());
    QLocalServer server;
    if (!parser.isSet(screenshotOption)) {
        QLocalSocket running;
        running.connectToServer(serverName);
        if (running.waitForConnected(300)) {
            running.write(QByteArray::number(page) + '\n');
            running.waitForBytesWritten(300);
            return 0;
        }
        QLocalServer::removeServer(serverName); // left over from a Settings that crashed
        server.listen(serverName);
    }

    RiftHome::Settings::Network network;
    RiftHome::Settings::Bluetooth bluetooth;
    RiftHome::Settings::Sound sound;
    RiftHome::Settings::DateTime dateTime;
    RiftHome::Settings::RiftSettings riftSettings;
    RiftHome::Settings::System system;
    const bool agent = parser.isSet(agentOption);
    RiftHome::Settings::Authentication auth(agent);
    if (agent) {
        app.setQuitOnLastWindowClosed(false); // closed, the window only hides: the agent stays
    }

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("icon"), new IconProvider);
    engine.addImageProvider(QStringLiteral("room"), new RoomThumbnailProvider);
    QQmlContext *context = engine.rootContext();
    context->setContextProperty(QStringLiteral("network"), &network);
    context->setContextProperty(QStringLiteral("bluetooth"), &bluetooth);
    context->setContextProperty(QStringLiteral("sound"), &sound);
    context->setContextProperty(QStringLiteral("dateTime"), &dateTime);
    context->setContextProperty(QStringLiteral("riftSettings"), &riftSettings);
    context->setContextProperty(QStringLiteral("system"), &system);
    context->setContextProperty(QStringLiteral("auth"), &auth);
    context->setContextProperty(QStringLiteral("startPage"), std::max(page, 0));
    context->setContextProperty(QStringLiteral("startHidden"), agent);
    context->setContextProperty(QStringLiteral("previewSheet"), parser.value(previewSheetOption));
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    QObject::connect(&server, &QLocalServer::newConnection, &app, [&server, &engine] {
        QLocalSocket *client = server.nextPendingConnection();
        QObject::connect(client, &QLocalSocket::readyRead, client, [client, &engine] {
            const int wanted = client->readLine().trimmed().toInt();
            if (!engine.rootObjects().isEmpty()) {
                QObject *root = engine.rootObjects().constFirst();
                if (wanted >= 0) {
                    root->setProperty("page", wanted);
                }
                if (auto *window = qobject_cast<QQuickWindow *>(root)) {
                    window->show(); // the window of the background process, asked for by the dock
                }
            }
            client->deleteLater();
        });
    });
    if (agent) {
        // A prompt shows the window (its sheet opens by itself); when done, a window shown only for it hides again.
        auto shownForPrompt = std::make_shared<bool>(false);
        QObject::connect(&auth, &RiftHome::Settings::Authentication::changed, &app, [&auth, &engine, shownForPrompt] {
            auto *window = engine.rootObjects().isEmpty() ? nullptr : qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
            if (!window) {
                return;
            }
            if (auth.pending() && !window->isVisible()) {
                *shownForPrompt = true;
                window->show();
            } else if (!auth.pending() && *shownForPrompt) {
                *shownForPrompt = false;
                window->hide();
            }
        });
    }
    if (parser.isSet(screenshotOption)) {
        const QString file = parser.value(screenshotOption);
        const int height = parser.value(screenshotHeightOption).toInt();
        if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst()); window && height > 0) {
            window->resize(window->width(), height);
        }
        QTimer::singleShot(3000, &app, [&engine, file] {
            if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst())) {
                window->grabWindow().save(file);
            }
            QGuiApplication::quit();
        });
    }
    return app.exec();
}
