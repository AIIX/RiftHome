// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "app/NotificationStore.hpp"

#include <QTest>

using namespace RiftHome::App;

namespace
{
NotificationEntry note(const QString &app, const QString &summary)
{
    NotificationEntry e;
    e.app = app;
    e.summary = summary;
    return e;
}
} // namespace

class TestNotifications : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void newestFirstAndReplaced()
    {
        NotificationStore store;
        const uint a = store.add(note(QStringLiteral("Firefox"), QStringLiteral("Download done")), 0);
        const uint b = store.add(note(QStringLiteral("Konsole"), QStringLiteral("Command finished")), 0);
        QVERIFY(a != 0 && b != 0 && a != b);
        QCOMPARE(store.entries().first().id, b);
        // Replacing keeps the id, updates it and moves it to the top.
        QCOMPARE(store.add(note(QStringLiteral("Firefox"), QStringLiteral("Download 2 done")), a), a);
        QCOMPARE(store.entries().size(), 2);
        QCOMPARE(store.entries().first().summary, QStringLiteral("Download 2 done"));
        // A replaces_id that is gone: a new notification (the specification).
        const uint c = store.add(note(QStringLiteral("x"), QStringLiteral("y")), 999);
        QVERIFY(c != 999 && store.find(c));
        QVERIFY(store.remove(b));
        QVERIFY(!store.remove(b));
        QCOMPARE(store.clear().size(), 2);
        QVERIFY(store.entries().isEmpty());
    }

    void oldestDroppedPastTheLimit()
    {
        NotificationStore store;
        const uint first = store.add(note(QStringLiteral("a"), QStringLiteral("first")), 0);
        QList<uint> dropped;
        for (int i = 0; i < NotificationStore::kMax; ++i) {
            store.add(note(QStringLiteral("a"), QString::number(i)), 0, &dropped);
        }
        QCOMPARE(store.entries().size(), NotificationStore::kMax);
        QCOMPARE(dropped, QList<uint>{first});
        QVERIFY(!store.find(first));
    }

    void howLongAndWhetherItCloses()
    {
        NotificationEntry e;
        QCOMPARE(NotificationStore::cardMs(e), NotificationStore::kDefaultCardMs); // -1: the server's choice
        QVERIFY(!NotificationStore::closesItself(e)); // kept in the list
        e.expireMs = 0; // never expires
        QCOMPARE(NotificationStore::cardMs(e), NotificationStore::kLongCardMs);
        QVERIFY(!NotificationStore::closesItself(e));
        e.expireMs = 1000; // asked: closes after its time, the card at least 3 s
        QCOMPARE(NotificationStore::cardMs(e), 3000);
        QVERIFY(NotificationStore::closesItself(e));
        QCOMPARE(NotificationStore::expireAfterMs(e), 1000);
        e.urgency = 2; // critical: never closes by itself
        QVERIFY(!NotificationStore::closesItself(e));
        NotificationEntry t;
        t.transient = true;
        QVERIFY(NotificationStore::closesItself(t));
        QCOMPARE(NotificationStore::expireAfterMs(t), NotificationStore::kDefaultCardMs);
    }

    void actionButtons()
    {
        // "default" is the tap on the card; unlabelled ones are not shown; at most three, in the order sent.
        NotificationEntry e;
        e.actions = {QStringLiteral("default"), QStringLiteral("Open"), QStringLiteral("open"), QStringLiteral("Open"),
                     QStringLiteral("folder"), QStringLiteral("Show in folder"), QStringLiteral("x"), QString(),
                     QStringLiteral("a"), QStringLiteral("A"), QStringLiteral("b"), QStringLiteral("B")};
        const auto buttons = NotificationStore::buttons(e);
        QCOMPARE(buttons.size(), 3);
        QCOMPARE(buttons[0].first, QStringLiteral("open"));
        QCOMPARE(buttons[1].second, QStringLiteral("Show in folder"));
        QCOMPARE(buttons[2].first, QStringLiteral("a"));
        // An odd list (a key without a label) is not read past its end.
        e.actions = {QStringLiteral("open")};
        QVERIFY(NotificationStore::buttons(e).isEmpty());
    }

    void markupTakenOut()
    {
        QCOMPARE(NotificationStore::plainText(QStringLiteral("<b>Bold</b> &amp; <a href=\"x\">link</a><br/>next")), QStringLiteral("Bold & link\nnext"));
        QCOMPARE(NotificationStore::plainText(QStringLiteral("  1 &lt; 2  ")), QStringLiteral("1 < 2"));
    }
};

QTEST_GUILESS_MAIN(TestNotifications)
#include "tst_notifications.moc"
