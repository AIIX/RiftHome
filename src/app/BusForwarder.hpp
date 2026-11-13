// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QDBusConnection>
#include <QDBusVirtualObject>
#include <QString>

namespace RiftHome::App
{

/*!
 * A D-Bus service of the desktop's session bus made available on Rift Home's own bus (rift-home-run.sh's
 * dbus-run-session), under the same name: every call forwarded and its reply sent back, the service's signals relayed.
 * For the Secret Service (org.freedesktop.secrets) while Rift Home runs nested in the desktop (Proton
 * VPN found no password store on Rift Home's bus and quit; the desktop's gnome-keyring holds its login). In its own
 * session Rift Home starts the store itself instead. Main thread.
 */
class BusForwarder : public QDBusVirtualObject
{
    Q_OBJECT

public:
    //! @p root: the service's objects are this path and below it; @p local: Rift Home's bus; @p desktop: the desktop's.
    BusForwarder(const QString &service, const QString &root, const QDBusConnection &local, const QDBusConnection &desktop, QObject *parent = nullptr);
    //! When the desktop has the service: takes its name on Rift Home's bus; false (logged) otherwise.
    bool start();

    QString introspect(const QString &path) const override;
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override;

private Q_SLOTS:
    void relaySignal(const QDBusMessage &signal);

private:
    QString m_service;
    QString m_root;
    QDBusConnection m_local;
    QDBusConnection m_desktop;
    int m_calls = 0;
};

} // namespace RiftHome::App
