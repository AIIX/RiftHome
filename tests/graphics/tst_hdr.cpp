// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "graphics/RadianceHdr.hpp"

#include <QFloat16>
#include <QTest>

using namespace RiftHome::Graphics;

namespace
{

QByteArray header(int width, int height)
{
    return QByteArray("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y ") + QByteArray::number(height) + " +X " + QByteArray::number(width) + "\n";
}

} // namespace

class TestHdr : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void flatScanlines()
    {
        // 2x1, flat: (1, 0.5, 0.25) as RGBE 128,64,32,129 and black.
        QByteArray data = header(2, 1);
        data.append(char(128)).append(char(64)).append(char(32)).append(char(129));
        data.append(char(0)).append(char(0)).append(char(0)).append(char(0));
        const auto image = parseRadianceHdr(data);
        QVERIFY(image && image->width == 2 && image->height == 1);
        QVERIFY(std::fabs(image->rgb[0] - 1.0f) < 0.01f && std::fabs(image->rgb[1] - 0.5f) < 0.01f && std::fabs(image->rgb[2] - 0.25f) < 0.01f);
        QCOMPARE(image->rgb[3], 0.0f);
    }

    void runLengthScanlines()
    {
        // 8x1, new-style: R a run of 8 x 128, G literals, B a run of 0, E a run of 129.
        QByteArray data = header(8, 1);
        data.append(char(2)).append(char(2)).append(char(0)).append(char(8));
        data.append(char(128 + 8)).append(char(128));
        data.append(char(8));
        for (int i = 0; i < 8; ++i) {
            data.append(char(i * 16));
        }
        data.append(char(128 + 8)).append(char(0));
        data.append(char(128 + 8)).append(char(129));
        const auto image = parseRadianceHdr(data);
        QVERIFY(image && image->width == 8);
        QVERIFY(std::fabs(image->rgb[0] - 1.0f) < 0.01f);                // R
        QVERIFY(std::fabs(image->rgb[7 * 3 + 1] - 112.0f / 128.0f) < 0.01f); // G of the last pixel
        QVERIFY(image->rgb[2] < 0.01f);                                   // B
    }

    void notHdr()
    {
        QVERIFY(!parseRadianceHdr("P6\n2 2\n255\n"));
        QVERIFY(!parseRadianceHdr(header(4, 4))); // no pixels
    }

    void exposureAndHalves()
    {
        HdrImage image{4, 2, std::vector<float>(4 * 2 * 3, 2.0f)};
        QVERIFY(std::fabs(exposureFor(image, 0.18f) - 0.09f) < 0.001f);
        int w = 0, h = 0;
        const QByteArray halves = toHalfRgba(image, 0.5f, 2, w, h); // halved to 2x1
        QCOMPARE(w, 2);
        QCOMPARE(h, 1);
        const auto *px = reinterpret_cast<const qfloat16 *>(halves.constData());
        QCOMPARE(float(px[0]), 1.0f);
        QCOMPARE(float(px[3]), 1.0f); // alpha
    }
};

QTEST_GUILESS_MAIN(TestHdr)
#include "tst_hdr.moc"
