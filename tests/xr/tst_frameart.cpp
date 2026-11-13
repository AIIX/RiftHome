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
};

QTEST_MAIN(TestFrameArt)
#include "tst_frameart.moc"
