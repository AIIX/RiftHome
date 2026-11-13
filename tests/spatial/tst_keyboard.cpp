// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "spatial/VirtualKeyboard.hpp"

#include <QTest>

#include <linux/input-event-codes.h>

using namespace RiftHome::Spatial;

namespace
{

int indexOf(const VirtualKeyboard &kb, const QString &label)
{
    for (size_t i = 0; i < kb.keys().size(); ++i) {
        if (kb.keys()[i].label == label) {
            return int(i);
        }
    }
    return -1;
}

} // namespace

class TestKeyboard : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void layoutIsUsAndFitsItsRows()
    {
        const VirtualKeyboard kb;
        // 14.5 pitches wide, five rows; every row fills the width.
        QVERIFY(qAbs(kb.width() - (14.5f * VirtualKeyboard::kPitch + 2 * VirtualKeyboard::kPadding)) < 1e-5f);
        QVERIFY(qAbs(kb.height() - (5 * VirtualKeyboard::kPitch + 2 * VirtualKeyboard::kPadding)) < 1e-5f);
        QCOMPARE(kb.keys().size(), size_t(14 + 14 + 13 + 12 + 7));
        // Q is left of W, A below Q.
        const KeyRect q = kb.rects()[size_t(indexOf(kb, QStringLiteral("q")))];
        const KeyRect w = kb.rects()[size_t(indexOf(kb, QStringLiteral("w")))];
        const KeyRect a = kb.rects()[size_t(indexOf(kb, QStringLiteral("a")))];
        QVERIFY(q.x < w.x && a.y < q.y);
    }

    void hitFindsTheKeyAndTheGapsBelongToKeys()
    {
        const VirtualKeyboard kb;
        const int g = indexOf(kb, QStringLiteral("g"));
        const KeyRect r = kb.rects()[size_t(g)];
        QCOMPARE(kb.hit(r.x, r.y), g);
        // Just inside the gap to the right of G: still a key (G or H), never nothing.
        QVERIFY(kb.hit(r.x + r.width / 2.0f + 0.0015f, r.y) >= 0);
        // Off the keyboard: nothing; on its padding: solid but no key.
        QCOMPARE(kb.hit(0.0f, kb.height()), -1);
        QVERIFY(kb.contains(0.0f, kb.height() / 2.0f - 0.005f));
        QCOMPARE(kb.hit(0.0f, kb.height() / 2.0f - 0.005f), -1);
    }

    void lettersTypeAndShiftIsOneShot()
    {
        VirtualKeyboard kb;
        const int a = indexOf(kb, QStringLiteral("a"));
        QCOMPARE(kb.press(a), (std::vector<KeyEvent>{{KEY_A, true}}));
        QCOMPARE(kb.release(a), (std::vector<KeyEvent>{{KEY_A, false}}));
        // Shift, then A: shifted once (A, with Shift held around it); then a again.
        const int shift = indexOf(kb, QStringLiteral("Shift"));
        QVERIFY(kb.press(shift).empty() && kb.shift());
        QCOMPARE(kb.label(a), QStringLiteral("A"));
        QCOMPARE(kb.press(a), (std::vector<KeyEvent>{{KEY_LEFTSHIFT, true}, {KEY_A, true}}));
        QCOMPARE(kb.release(a), (std::vector<KeyEvent>{{KEY_A, false}, {KEY_LEFTSHIFT, false}}));
        QVERIFY(!kb.shift());
        QCOMPARE(kb.label(a), QStringLiteral("a"));
    }

    void shiftTwiceIsCapsLockForLettersOnly()
    {
        VirtualKeyboard kb;
        const int shift = indexOf(kb, QStringLiteral("Shift"));
        kb.press(shift);
        kb.press(shift);
        QVERIFY(kb.capsLock() && !kb.shift());
        const int b = indexOf(kb, QStringLiteral("b"));
        const int one = indexOf(kb, QStringLiteral("1"));
        QCOMPARE(kb.label(b), QStringLiteral("B"));
        QCOMPARE(kb.label(one), QStringLiteral("1"));
        QCOMPARE(kb.press(b).front(), (KeyEvent{KEY_LEFTSHIFT, true}));
        kb.release(b);
        QVERIFY(kb.capsLock()); // stays
        QCOMPARE(kb.press(one), (std::vector<KeyEvent>{{KEY_1, true}}));
        kb.release(one);
        kb.press(shift);
        QVERIFY(!kb.capsLock() && !kb.shift());
    }

    void ctrlCombinesAndHideHides()
    {
        VirtualKeyboard kb;
        const int ctrl = indexOf(kb, QStringLiteral("Ctrl"));
        const int c = indexOf(kb, QStringLiteral("c"));
        kb.press(ctrl);
        QCOMPARE(kb.press(c), (std::vector<KeyEvent>{{KEY_LEFTCTRL, true}, {KEY_C, true}}));
        QCOMPARE(kb.release(c), (std::vector<KeyEvent>{{KEY_C, false}, {KEY_LEFTCTRL, false}}));
        QVERIFY(!kb.ctrl());
        QVERIFY(!kb.takeHide());
        kb.press(indexOf(kb, QStringLiteral("Hide")));
        QVERIFY(kb.takeHide() && !kb.takeHide());
    }

    void textToKeys()
    {
        const VirtualKeyboard kb;
        QCOMPARE(kb.keyForChar(QLatin1Char('h')), (std::optional<std::pair<uint32_t, bool>>{{KEY_H, false}}));
        QCOMPARE(kb.keyForChar(QLatin1Char('H')), (std::optional<std::pair<uint32_t, bool>>{{KEY_H, true}}));
        QCOMPARE(kb.keyForChar(QLatin1Char('!')), (std::optional<std::pair<uint32_t, bool>>{{KEY_1, true}}));
        QCOMPARE(kb.keyForChar(QLatin1Char(' ')), (std::optional<std::pair<uint32_t, bool>>{{KEY_SPACE, false}}));
        QCOMPARE(kb.keyForChar(QLatin1Char(',')), (std::optional<std::pair<uint32_t, bool>>{{KEY_COMMA, false}}));
        QVERIFY(!kb.keyForChar(QChar(0x00e9))); // é: not on a US keyboard
    }
};

QTEST_GUILESS_MAIN(TestKeyboard)
#include "tst_keyboard.moc"
