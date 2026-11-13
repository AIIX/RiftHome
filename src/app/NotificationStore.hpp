// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QList>
#include <QPair>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <algorithm>

namespace RiftHome::App
{

/*!
 * Notifications as Rift Home keeps them (Rift Home is the freedesktop notification server on its own
 * session bus): newest first, a replaced one updated and moved to the top, the oldest dropped past kMax. How long the card
 * shows and whether a notification closes by itself follow the specification's expire_timeout and hints. Pure.
 */
struct NotificationEntry {
    uint id = 0;
    QString app; //!< app_name
    QString summary;
    QString body; //!< plain text (see plainText)
    QString iconName; //!< app_icon: a theme icon name, a path or a file:// URL
    QString desktopEntry; //!< hint "desktop-entry": the sender's .desktop id, without ".desktop"
    QStringList actions; //!< key, label, key, label, ...
    int urgency = 1; //!< 0 low, 1 normal, 2 critical
    bool transient = false; //!< hint "transient": not kept in the list once its card has gone
    bool resident = false; //!< hint "resident": kept when one of its actions is invoked
    int expireMs = -1; //!< as asked: -1 the server's choice, 0 never, else milliseconds
};

class NotificationStore
{
public:
    static constexpr int kMax = 30;
    static constexpr int kDefaultCardMs = 5000;
    static constexpr int kLongCardMs = 10000; //!< critical ones and those that never expire: their card stays longer

    /*!
     * Adds @p entry, or updates the one @p replacesId names (when it is still here: else a new one, as the
     * specification asks). Returns its id; @p dropped gets the ids pushed out past kMax.
     */
    uint add(NotificationEntry entry, uint replacesId, QList<uint> *dropped = nullptr)
    {
        const auto old = std::find_if(m_entries.begin(), m_entries.end(), [replacesId](const NotificationEntry &e) {
            return replacesId != 0 && e.id == replacesId;
        });
        if (old != m_entries.end()) {
            entry.id = replacesId;
            m_entries.erase(old);
        } else {
            entry.id = ++m_lastId == 0 ? ++m_lastId : m_lastId; // never 0 (it means "none")
        }
        m_entries.prepend(entry);
        while (m_entries.size() > kMax) {
            if (dropped) {
                dropped->append(m_entries.last().id);
            }
            m_entries.removeLast();
        }
        return entry.id;
    }
    bool remove(uint id)
    {
        return m_entries.removeIf([id](const NotificationEntry &e) {
            return e.id == id;
        }) > 0;
    }
    QList<uint> clear()
    {
        QList<uint> ids;
        for (const NotificationEntry &e : m_entries) {
            ids << e.id;
        }
        m_entries.clear();
        return ids;
    }
    const NotificationEntry *find(uint id) const
    {
        for (const NotificationEntry &e : m_entries) {
            if (e.id == id) {
                return &e;
            }
        }
        return nullptr;
    }
    const QList<NotificationEntry> &entries() const
    {
        return m_entries;
    }

    //! How long its card shows: what it asked (3 to 15 s), else 5 s, 10 s for critical ones and those that never expire.
    static int cardMs(const NotificationEntry &e)
    {
        if (e.expireMs > 0) {
            return std::clamp(e.expireMs, 3000, 15000);
        }
        return e.urgency >= 2 || e.expireMs == 0 ? kLongCardMs : kDefaultCardMs;
    }
    //! It closes itself (NotificationClosed, reason expired) after expireAfterMs: asked with a time, or transient.
    static bool closesItself(const NotificationEntry &e)
    {
        return e.urgency < 2 && (e.expireMs > 0 || e.transient);
    }
    static int expireAfterMs(const NotificationEntry &e)
    {
        return e.expireMs > 0 ? e.expireMs : cardMs(e);
    }

    static constexpr int kMaxButtons = 3;
    /*!
     * Its action buttons, as (key, label) left to right: the actions but "default" (a tap on the card) and
     * those without a label, at most kMaxButtons.
     */
    static QList<QPair<QString, QString>> buttons(const NotificationEntry &e)
    {
        QList<QPair<QString, QString>> list;
        for (qsizetype i = 0; i + 1 < e.actions.size() && list.size() < kMaxButtons; i += 2) {
            const QString &key = e.actions[i];
            const QString label = e.actions[i + 1].simplified();
            if (key != QLatin1String("default") && !key.isEmpty() && !label.isEmpty()) {
                list.append({key, label});
            }
        }
        return list;
    }

    //! A body as plain text: markup some apps send anyway taken out (the server does not offer body-markup), lines kept.
    static QString plainText(const QString &body)
    {
        static const QRegularExpression breaks(QStringLiteral("<br\\s*/?>"), QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression tags(QStringLiteral("<[^>]*>"));
        QString text = body;
        text.replace(breaks, QStringLiteral("\n"));
        text.remove(tags);
        text.replace(QLatin1String("&lt;"), QLatin1String("<"));
        text.replace(QLatin1String("&gt;"), QLatin1String(">"));
        text.replace(QLatin1String("&quot;"), QLatin1String("\""));
        text.replace(QLatin1String("&apos;"), QLatin1String("'"));
        text.replace(QLatin1String("&#39;"), QLatin1String("'"));
        text.replace(QLatin1String("&amp;"), QLatin1String("&"));
        return text.trimmed();
    }

private:
    QList<NotificationEntry> m_entries;
    uint m_lastId = 0;
};

} // namespace RiftHome::App
