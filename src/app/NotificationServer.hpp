// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app/NotificationStore.hpp"

#include <QHash>
#include <QImage>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

#include <functional>

namespace RiftHome::App
{

/*!
 * Rift Home as the freedesktop notification server (org.freedesktop.Notifications 1.2) on its own
 * session bus, the one the apps started in Rift Home use (rift-home-run.sh: dbus-run-session), so it works nested in
 * Plasma too. The newest notification shows as a card for a few seconds (cardId), all of them in the dock's list until
 * dismissed. Tapping one invokes its default action. Main thread.
 */
class NotificationServer : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")

public:
    //! @p iconFor: an icon for (app_icon, app_name, desktop-entry); a null image when there is none.
    explicit NotificationServer(std::function<QImage(const QString &, const QString &, const QString &)> iconFor, QObject *parent = nullptr);

    //! Takes the name on the session bus; false (and a warning) when another server has it.
    bool registerOnBus();

    const QList<NotificationEntry> &entries() const
    {
        return m_store.entries();
    }
    QImage icon(uint id) const
    {
        return m_icons.value(id);
    }
    uint cardId() const //!< the notification whose card shows now; 0: none
    {
        return m_card;
    }
    //! Tapped: its default action (when it has one), then closed (reason dismissed) unless it is resident.
    void activate(uint id);
    //! Its @p index-th action button (NotificationStore::buttons), then closed unless it is resident.
    void invokeAction(uint id, int index);
    //! Dismissed from the list (0: all of them).
    void dismiss(uint id);

public Q_SLOTS:
    // D-Bus methods.
    Q_SCRIPTABLE QStringList GetCapabilities() const;
    Q_SCRIPTABLE uint Notify(const QString &appName, uint replacesId, const QString &appIcon, const QString &summary, const QString &body,
                             const QStringList &actions, const QVariantMap &hints, int expireTimeout);
    Q_SCRIPTABLE void CloseNotification(uint id);
    Q_SCRIPTABLE QString GetServerInformation(QString &vendor, QString &version, QString &specVersion) const;

Q_SIGNALS:
    Q_SCRIPTABLE void NotificationClosed(uint id, uint reason);
    Q_SCRIPTABLE void ActionInvoked(uint id, const QString &actionKey);
    //! The list or the card changed.
    void changed();

private:
    enum Reason : uint {
        Expired = 1,
        Dismissed = 2,
        Closed = 3, //!< by CloseNotification
    };
    void close(uint id, uint reason);
    QImage iconFromHints(const QVariantMap &hints) const;

    std::function<QImage(const QString &, const QString &, const QString &)> m_iconFor;
    NotificationStore m_store;
    QHash<uint, QImage> m_icons;
    QHash<uint, quint64> m_serial; //!< which Notify call a timer belongs to (a replaced notification gets a new one)
    quint64 m_nextSerial = 0;
    uint m_card = 0;
    QTimer m_cardTimer;
};

} // namespace RiftHome::App
