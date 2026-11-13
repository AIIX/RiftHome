// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "app/NotificationServer.hpp"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QUrl>

Q_LOGGING_CATEGORY(lcNotify, "rifthome.notify")

namespace RiftHome::App
{

namespace
{
constexpr int kIconSize = 128;
}

NotificationServer::NotificationServer(std::function<QImage(const QString &, const QString &, const QString &)> iconFor, QObject *parent)
    : QObject(parent)
    , m_iconFor(std::move(iconFor))
{
    m_cardTimer.setSingleShot(true);
    connect(&m_cardTimer, &QTimer::timeout, this, [this] {
        const uint id = m_card;
        m_card = 0;
        Q_EMIT changed();
        qCInfo(lcNotify) << "card gone" << id;
    });
}

bool NotificationServer::registerOnBus()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCWarning(lcNotify) << "no session bus: notifications off";
        return false;
    }
    if (!bus.registerObject(QStringLiteral("/org/freedesktop/Notifications"), this,
                            QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals | QDBusConnection::ExportScriptableInvokables)) {
        qCWarning(lcNotify) << "could not export the notification server";
        return false;
    }
    // Not queued: on Rift Home's own bus nobody else should have it; if someone does, theirs stays.
    const auto reply = bus.interface()->registerService(QStringLiteral("org.freedesktop.Notifications"), QDBusConnectionInterface::DontQueueService,
                                                        QDBusConnectionInterface::DontAllowReplacement);
    if (!reply.isValid() || reply.value() != QDBusConnectionInterface::ServiceRegistered) {
        qCWarning(lcNotify) << "org.freedesktop.Notifications is taken on this bus: notifications go elsewhere";
        bus.unregisterObject(QStringLiteral("/org/freedesktop/Notifications"));
        return false;
    }
    qCInfo(lcNotify) << "notification server on" << qgetenv("DBUS_SESSION_BUS_ADDRESS").constData();
    return true;
}

QStringList NotificationServer::GetCapabilities() const
{
    // Plain bodies (markup some apps send anyway is taken out), the default action, kept in a list.
    return {QStringLiteral("body"), QStringLiteral("actions"), QStringLiteral("icon-static"), QStringLiteral("persistence")};
}

QString NotificationServer::GetServerInformation(QString &vendor, QString &version, QString &specVersion) const
{
    vendor = QStringLiteral("Rift Home");
    version = QStringLiteral(PROJECT_VERSION_STRING);
    specVersion = QStringLiteral("1.2");
    return QStringLiteral("Rift Home");
}

uint NotificationServer::Notify(const QString &appName, uint replacesId, const QString &appIcon, const QString &summary, const QString &body,
                                const QStringList &actions, const QVariantMap &hints, int expireTimeout)
{
    NotificationEntry entry;
    entry.app = appName;
    entry.summary = summary;
    entry.body = NotificationStore::plainText(body);
    entry.iconName = appIcon;
    entry.desktopEntry = hints.value(QStringLiteral("desktop-entry")).toString();
    entry.actions = actions;
    entry.urgency = hints.contains(QStringLiteral("urgency")) ? hints.value(QStringLiteral("urgency")).toInt() : 1;
    entry.transient = hints.value(QStringLiteral("transient")).toBool();
    entry.resident = hints.value(QStringLiteral("resident")).toBool();
    entry.expireMs = expireTimeout;
    QList<uint> dropped;
    const uint id = m_store.add(entry, replacesId, &dropped);
    for (uint gone : dropped) {
        m_icons.remove(gone);
        m_serial.remove(gone);
        Q_EMIT NotificationClosed(gone, Expired);
    }
    // The picture, by the specification's order: image-data, image-path, app_icon, then the sender's app.
    QImage icon = iconFromHints(hints);
    if (icon.isNull() && m_iconFor) {
        const QString path = hints.value(QStringLiteral("image-path"), hints.value(QStringLiteral("image_path"))).toString();
        icon = m_iconFor(path.isEmpty() ? appIcon : path, appName, entry.desktopEntry);
    }
    m_icons.insert(id, icon);
    const quint64 serial = ++m_nextSerial;
    m_serial.insert(id, serial);
    const NotificationEntry &kept = *m_store.find(id);
    m_card = id;
    m_cardTimer.start(NotificationStore::cardMs(kept));
    if (NotificationStore::closesItself(kept)) {
        QTimer::singleShot(NotificationStore::expireAfterMs(kept), this, [this, id, serial] {
            if (m_serial.value(id) == serial) {
                close(id, Expired);
            }
        });
    }
    qCInfo(lcNotify).noquote() << QStringLiteral("notification %1 from \"%2\": \"%3\"%4 (urgency %5, timeout %6, %7 actions)")
                                      .arg(id)
                                      .arg(appName, summary, replacesId ? QStringLiteral(" replacing %1").arg(replacesId) : QString())
                                      .arg(entry.urgency)
                                      .arg(expireTimeout)
                                      .arg(actions.size() / 2);
    Q_EMIT changed();
    return id;
}

void NotificationServer::CloseNotification(uint id)
{
    close(id, Closed);
}

void NotificationServer::activate(uint id)
{
    const NotificationEntry *e = m_store.find(id);
    if (!e) {
        return;
    }
    const bool resident = e->resident;
    if (e->actions.contains(QStringLiteral("default"))) {
        qCInfo(lcNotify) << "notification" << id << "default action";
        Q_EMIT ActionInvoked(id, QStringLiteral("default"));
    }
    if (!resident) {
        close(id, Dismissed);
    }
}

void NotificationServer::invokeAction(uint id, int index)
{
    const NotificationEntry *e = m_store.find(id);
    if (!e) {
        return;
    }
    const auto buttons = NotificationStore::buttons(*e);
    if (index < 0 || index >= buttons.size()) {
        return;
    }
    const bool resident = e->resident;
    qCInfo(lcNotify).noquote() << QStringLiteral("notification %1 action \"%2\" (%3)").arg(id).arg(buttons[index].second, buttons[index].first);
    Q_EMIT ActionInvoked(id, buttons[index].first);
    if (!resident) {
        close(id, Dismissed);
    }
}

void NotificationServer::dismiss(uint id)
{
    if (id != 0) {
        close(id, Dismissed);
        return;
    }
    const QList<uint> ids = m_store.clear();
    for (uint gone : ids) {
        Q_EMIT NotificationClosed(gone, Dismissed);
    }
    m_icons.clear();
    m_serial.clear();
    m_card = 0;
    m_cardTimer.stop();
    qCInfo(lcNotify) << "all notifications dismissed:" << ids.size();
    Q_EMIT changed();
}

void NotificationServer::close(uint id, uint reason)
{
    if (!m_store.remove(id)) {
        return;
    }
    m_icons.remove(id);
    m_serial.remove(id);
    if (m_card == id) {
        m_card = 0;
        m_cardTimer.stop();
    }
    qCInfo(lcNotify) << "notification" << id << "closed, reason" << reason;
    Q_EMIT NotificationClosed(id, reason);
    Q_EMIT changed();
}

QImage NotificationServer::iconFromHints(const QVariantMap &hints) const
{
    // image-data (iiibiiay): width, height, rowstride, has alpha, bits per sample, channels, the pixels.
    for (const QString &key : {QStringLiteral("image-data"), QStringLiteral("image_data"), QStringLiteral("icon_data")}) {
        const QVariant value = hints.value(key);
        if (!value.canConvert<QDBusArgument>()) {
            continue;
        }
        const QDBusArgument argument = value.value<QDBusArgument>();
        int width = 0, height = 0, stride = 0, bits = 0, channels = 0;
        bool alpha = false;
        QByteArray pixels;
        argument.beginStructure();
        argument >> width >> height >> stride >> alpha >> bits >> channels >> pixels;
        argument.endStructure();
        if (width <= 0 || height <= 0 || bits != 8 || (channels != 3 && channels != 4) || stride < width * channels
            || pixels.size() < qsizetype(stride) * (height - 1) + qsizetype(width) * channels) {
            continue;
        }
        const QImage image(reinterpret_cast<const uchar *>(pixels.constData()), width, height, stride,
                           channels == 4 ? QImage::Format_RGBA8888 : QImage::Format_RGB888);
        return image.convertToFormat(QImage::Format_RGBA8888_Premultiplied).scaled(kIconSize, kIconSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return {};
}

} // namespace RiftHome::App
