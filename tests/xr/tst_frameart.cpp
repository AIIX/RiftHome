// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "xr/FrameArt.hpp"

#include <QtTest>

#include <cmath>

using namespace RiftHome::Xr;

class TestFrameArt : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    //! The lens test's cross, at the panel's density: on the middle pixel (the lens's axis), four pure green lines a
    //! pixel wide and 4 apart a bar about a dark middle, the horizontal ones stopping short of the vertical bar (no
    //! overlap), 81 pixels across as Oculus'; the rest black and opaque. Without it, the middle is black.
    void lensTestCross()
    {
        const float panel = 793.0f; // the CV1's 1:1 at the lens centre, as RiftLinux reports it
        const QImage image = FrameArt::lensTest(panel, 63, true);
        const int expected = 2 * int(std::lround(panel * std::tan(FrameArt::kLensTestDegrees / 2.0f * float(M_PI) / 180.0f))) + 1;
        QCOMPARE(image.width(), expected);
        QCOMPARE(image.height(), expected);
        const int c = image.width() / 2;
        const QRgb green = qRgba(0, 255, 0, 255);
        const QRgb black = qRgba(0, 0, 0, 255);
        // Every pixel of the 81 x 81 middle (and a pixel around): lit exactly where Oculus' layout has a line.
        const auto onLine = [](int v) {
            return v == -6 || v == -2 || v == 2 || v == 6;
        };
        for (int y = -41; y <= 41; ++y) {
            for (int x = -41; x <= 41; ++x) {
                const bool vertical = onLine(x) && std::abs(y) <= 39;
                const bool horizontal = onLine(y) && std::abs(x) >= 8 && std::abs(x) <= 39;
                QCOMPARE(image.pixel(c + x, c + y), vertical || horizontal ? green : black);
            }
        }
        QCOMPARE(image.pixel(c, c), black); // the middle is dark
        QCOMPARE(image.pixel(c + 7, c + 2), black); // the horizontal lines stop a pixel short of the vertical bar
        QCOMPARE(image.pixel(c + 8, c + 2), green);
        QCOMPARE(image.pixel(c + 40, c + 2), black); // 81 across
        QCOMPARE(qAlpha(image.pixel(0, 0)), 255);
        const QImage without = FrameArt::lensTest(panel, 63, false);
        for (int i = -40; i <= 40; ++i) {
            QCOMPARE(without.pixel(c + i, c + 2), black);
            QCOMPARE(without.pixel(c + 2, c + i), black);
        }
    }

    //! The texts are above and below the cross, never on it; the spacing's line only when known.
    void lensTestText()
    {
        const float density = 793.0f;
        const auto lit = [](const QImage &image, int top, int bottom) {
            int count = 0;
            for (int y = top; y < bottom; ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    const QRgb p = image.pixel(x, y);
                    count += qRed(p) > 60 && qBlue(p) > 60; // white text, not the green cross
                }
            }
            return count;
        };
        const auto row = [density](float degrees, const QImage &image) {
            return image.height() / 2 + int(std::lround(density * std::tan(degrees * float(M_PI) / 180.0f)));
        };
        const QImage known = FrameArt::lensTest(793.0f, 63, true);
        const QImage unknown = FrameArt::lensTest(793.0f, 0, true);
        QVERIFY(lit(known, row(-20.0f, known), row(-6.0f, known)) > 500); // the title and the instructions
        QCOMPARE(lit(known, row(-6.0f, known), row(4.0f, known)), 0); // nothing white around the cross
        QVERIFY(lit(known, row(4.5f, known), row(7.0f, known)) > 100); // "Lens spacing: 63 mm"
        QCOMPARE(lit(unknown, row(4.5f, unknown), row(7.0f, unknown)), 0);
        QVERIFY(lit(known, row(8.5f, known), row(11.6f, known)) > 100); // the button's frame and words
    }

    //! The window's numbers: shapes named as people name them (within 1 %), others as a ratio to one; the art two lines
    //! on the frame's glass at its size.
    void windowStats()
    {
        QCOMPARE(FrameArt::aspectRatio(3840, 2160), QStringLiteral("16:9"));
        QCOMPARE(FrameArt::aspectRatio(1920, 1080), QStringLiteral("16:9"));
        QCOMPARE(FrameArt::aspectRatio(3840, 1607), QStringLiteral("2.39:1"));
        QCOMPARE(FrameArt::aspectRatio(1440, 1080), QStringLiteral("4:3"));
        QCOMPARE(FrameArt::aspectRatio(2560, 1080), QStringLiteral("2.39:1")); // 2.37: within 1 % of 2.39, not of 21:9 (2.33)
        QCOMPARE(FrameArt::aspectRatio(5000, 2000), QStringLiteral("2.50:1"));
        QCOMPARE(FrameArt::aspectRatio(0, 1080), QString());
        const QStringList longest{QStringLiteral("Window, to 7680 \u00d7 4320 pixels \u00b7 2.39:1"), QStringLiteral("HDR (PQ, BT.2020), 10-bit YUV"),
                                  QStringLiteral("Sent to the runtime: 2880 \u00d7 1620, enlarged"),
                                  QStringLiteral("Each eye's picture: 2160 \u00d7 2400 (200 %)")};
        const QImage art = FrameArt::stats(longest, FrameArt::Theme::Dark);
        QVERIFY(std::abs(art.width() - int(std::lround(FrameArt::kStatsWidth * FrameArt::kPixelsPerUnit))) <= 1);
        QVERIFY(art.pixelColor(art.width() / 2, art.height() / 2).alpha() > 100);
        if (const QString dir = qEnvironmentVariable("RIFT_HOME_ART_DIR"); !dir.isEmpty()) {
            art.save(dir + QStringLiteral("/stats.png")); // to look at
        }
        // A line far too long is drawn smaller, all of it inside the text's room (never cut short or past the glass).
        const QImage blank = FrameArt::stats({QString(), QString(), QString(), QString()}, FrameArt::Theme::Dark);
        const QString tooLong = QStringLiteral("Headset gets 2880 \u00d7 1620, sharpened, and then a great deal more");
        const QImage shrunk = FrameArt::stats({QString(), QString(), tooLong, QString()}, FrameArt::Theme::Dark);
        const QImage plain = FrameArt::stats({QString(), QString(), QStringLiteral("Headset"), QString()}, FrameArt::Theme::Dark);
        const qreal row = (art.height() - 2) / qreal(FrameArt::kStatsLines);
        const int roomRight = int(1 + row * 0.45 + (art.width() - 2) - row * 0.9);
        const auto ink = [&](const QImage &image, int *left, int *right, int *top, int *bottom) {
            *left = image.width();
            *right = -1;
            *top = image.height();
            *bottom = -1;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    if (image.pixel(x, y) != blank.pixel(x, y)) {
                        *left = std::min(*left, x);
                        *right = std::max(*right, x);
                        *top = std::min(*top, y);
                        *bottom = std::max(*bottom, y);
                    }
                }
            }
        };
        int l = 0, r = 0, t = 0, b = 0, pl = 0, pr = 0, pt = 0, pb = 0;
        ink(shrunk, &l, &r, &t, &b);
        ink(plain, &pl, &pr, &pt, &pb);
        QVERIFY2(r >= 0 && r <= roomRight + 1, qPrintable(QStringLiteral("ink to %1, room to %2").arg(r).arg(roomRight)));
        QVERIFY2(r > roomRight - row, "it fills the line"); // all of it, not cut short
        QVERIFY2(b - t < pb - pt, "smaller letters than a line that fits");
    }

    //! Cinema mode's row of shapes: the frame's size, the selected shape lit (it differs from none selected).
    void aspectRow()
    {
        const QImage none = FrameArt::aspectBar(-1, -1, -1, QString(), 0.0f, FrameArt::Theme::Dark);
        const QImage film = FrameArt::aspectBar(0, -1, -1, QStringLiteral("2.39:1"), 2.39f, FrameArt::Theme::Dark);
        QVERIFY(std::abs(none.width() - int(std::lround((0.12f + 7 * 0.092f) * FrameArt::kPixelsPerUnit))) <= 8);
        QCOMPARE(film.size(), none.size());
        QVERIFY(film != none);
        // Hovered is an outline: it differs from not hovered, and from lit.
        const QImage hovered = FrameArt::aspectBar(-1, 7, -1, QStringLiteral("2.39:1"), 2.39f, FrameArt::Theme::Dark);
        const QImage lit = FrameArt::aspectBar(7, -1, -1, QStringLiteral("2.39:1"), 2.39f, FrameArt::Theme::Dark);
        QVERIFY(hovered != lit && hovered != FrameArt::aspectBar(-1, -1, -1, QStringLiteral("2.39:1"), 2.39f, FrameArt::Theme::Dark));
        // Inside the hovered button, just within its left edge (past the outline, before its icon and words): the glass,
        // not a fill (a fill means lit).
        const int scopeLeft = int(std::lround(0.12f * FrameArt::kPixelsPerUnit)) + 6 * int(std::lround(0.092f * FrameArt::kPixelsPerUnit));
        const QPoint inside(scopeLeft + 13, lit.height() / 2);
        QVERIFY(lit.pixelColor(inside).lightness() > hovered.pixelColor(inside).lightness() + 60);
        if (const QString dir = qEnvironmentVariable("RIFT_HOME_ART_DIR"); !dir.isEmpty()) {
            FrameArt::aspectBar(0, 3, -1, QStringLiteral("2.39:1"), 2.39f, FrameArt::Theme::Dark).save(dir + QStringLiteral("/aspects.png")); // to look at
        }
    }

    //! The padlock: the frame's size, a closed and lit lock when locked (the shackle reaches into the body), an open
    //! one when not (a gap between the shackle's left leg and the body), in both styles.
    void lockDot()
    {
        for (const FrameArt::Theme theme : {FrameArt::Theme::Dark, FrameArt::Theme::Light}) {
            const QImage locked = FrameArt::lockDot(true, FrameArt::State::Shown, theme);
            const QImage open = FrameArt::lockDot(false, FrameArt::State::Shown, theme);
            const int size = int(std::lround(0.036f * FrameArt::kPixelsPerUnit));
            QVERIFY(std::abs(locked.width() - size) <= 1 && locked.height() == locked.width());
            QCOMPARE(open.size(), locked.size());
            QVERIFY(locked != open);
            // Just above the body, where the shackle's left leg is: drawn when locked, a gap when open.
            const qreal s = locked.width();
            const QPoint leftLeg(int(s * (0.5 - 0.115)), int(s * 0.44));
            const QColor inkLocked = locked.pixelColor(leftLeg);
            const QColor backLocked = locked.pixelColor(QPoint(int(s * 0.2), int(s * 0.44)));
            const QColor inkOpen = open.pixelColor(leftLeg);
            const QColor backOpen = open.pixelColor(QPoint(int(s * 0.2), int(s * 0.44)));
            const auto apart = [](QColor a, QColor b) {
                return std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) + std::abs(a.blue() - b.blue()) > 60;
            };
            QVERIFY2(apart(inkLocked, backLocked), "the closed shackle's leg reaches the body");
            QVERIFY2(!apart(inkOpen, backOpen), "the open shackle's leg stops above the body");
        }
    }
};

QTEST_MAIN(TestFrameArt)
#include "tst_frameart.moc"
