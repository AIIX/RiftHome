// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "app/BusForwarder.hpp"

#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcBus, "rifthome.bus")

namespace RiftHome::App
{

namespace
{
constexpr int kTimeoutMs = 120000; //!< long: a store may ask for a password before it answers
}

BusForwarder::BusForwarder(const QString &service, const QString &root, const QDBusConnection &local, const QDBusConnection &desktop, QObject *parent)
    : QDBusVirtualObject(parent)
    , m_service(service)
    , m_root(root)
    , m_local(local)
    , m_desktop(desktop)
{
}

bool BusForwarder::start()
{
    if (!m_local.isConnected() || !m_desktop.isConnected()) {
        qCInfo(lcBus).noquote() << m_service << "not forwarded: no desktop bus";
        return false;
    }
    if (!m_desktop.interface()->isServiceRegistered(m_service).value()) {
        qCInfo(lcBus).noquote() << m_service << "not forwarded: the desktop does not run it";
        return false;
    }
    // Its signals (a password prompt's Completed, collections and items changing), from any of its objects.
    m_desktop.connect(m_service, QString(), QString(), QString(), this, SLOT(relaySignal(QDBusMessage)));
    if (!m_local.registerVirtualObject(m_root, this, QDBusConnection::SubPath)) {
        qCWarning(lcBus).noquote() << m_service << "not forwarded: could not export its objects";
        return false;
    }
    const auto reply = m_local.interface()->registerService(m_service, QDBusConnectionInterface::DontQueueService,
                                                           QDBusConnectionInterface::DontAllowReplacement);
    if (!reply.isValid() || reply.value() != QDBusConnectionInterface::ServiceRegistered) {
        qCWarning(lcBus).noquote() << m_service << "not forwarded: taken on Rift Home's bus";
        m_local.unregisterObject(m_root, QDBusConnection::UnregisterTree);
        return false;
    }
    qCInfo(lcBus).noquote() << m_service << "forwarded from the desktop's session bus";
    return true;
}

QString BusForwarder::introspect(const QString &path) const
{
    // The service's own description of its objects (QtDBus answers Introspect for a virtual object with this).
    QDBusMessage call = QDBusMessage::createMethodCall(m_service, path.isEmpty() ? m_root : path,
                                                       QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect"));
    const QDBusMessage reply = m_desktop.call(call, QDBus::Block, 5000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        return {};
    }
    // Without its document header and root element: QtDBus wraps the interfaces in its own.
    QString xml = reply.arguments().first().toString();
    const qsizetype open = xml.indexOf(QStringLiteral("<node"));
    const qsizetype body = open < 0 ? -1 : xml.indexOf(QLatin1Char('>'), open);
    const qsizetype close = xml.lastIndexOf(QStringLiteral("</node>"));
    return body >= 0 && close > body ? xml.mid(body + 1, close - body - 1) : QString();
}

bool BusForwarder::handleMessage(const QDBusMessage &message, const QDBusConnection &connection)
{
    if (message.type() != QDBusMessage::MethodCallMessage) {
        return false;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(m_service, message.path(), message.interface(), message.member());
    call.setArguments(message.arguments()); // complex arguments pass through as they came (QtDBus cross-marshals them)
    call.setInteractiveAuthorizationAllowed(message.isInteractiveAuthorizationAllowed());
    if (++m_calls <= 10) {
        qCInfo(lcBus).noquote() << m_service << "call" << message.interface() + QLatin1Char('.') + message.member() << message.path();
    }
    const QDBusPendingCall pending = m_desktop.asyncCall(call, kTimeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(pending, this);
    const bool wantsReply = message.isReplyRequired();
    const int number = m_calls;
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, message, connection, wantsReply, number](QDBusPendingCallWatcher *w) {
        const QDBusMessage reply = w->reply();
        w->deleteLater();
        if (reply.type() == QDBusMessage::ErrorMessage && number <= 10) {
            qCWarning(lcBus).noquote() << m_service << message.interface() + QLatin1Char('.') + message.member() << "failed:" << reply.errorName() << reply.errorMessage();
        }
        if (!wantsReply) {
            return;
        }
        QDBusConnection(connection).send(reply.type() == QDBusMessage::ErrorMessage ? message.createErrorReply(reply.errorName(), reply.errorMessage())
                                                                                   : message.createReply(reply.arguments()));
    });
    return true;
}

void BusForwarder::relaySignal(const QDBusMessage &signal)
{
    // Only its own objects': one program may own several names (KDE's wallet answers as kwalletd6 and kwalletd5), each
    // forwarded on its own, and its signals would come twice.
    if (m_root != QLatin1String("/") && signal.path() != m_root && !signal.path().startsWith(m_root + QLatin1Char('/'))) {
        return;
    }
    QDBusMessage relayed = QDBusMessage::createSignal(signal.path(), signal.interface(), signal.member());
    relayed.setArguments(signal.arguments());
    m_local.send(relayed);
}

} // namespace RiftHome::App
