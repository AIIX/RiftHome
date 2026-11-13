// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "color/ColorDescription.hpp"

#include <QTest>

#include <cmath>

#include <wayland-protocols/color-management-v1-enum.h>

using namespace RiftHome::Color;

class TestLayerPlan : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void untagged8BitIsGamma22()
    {
        // Untagged: gamma 2.2 (the protocol's rule since version 2), decoded and encoded again as sRGB for the runtime.
        const LayerPlan plan = planLayer({}, PixelKind::Unorm8);
        QCOMPARE(plan.path, LayerPath::ReencodeSrgb);
        QCOMPARE(plan.format, SwapchainFormat::Srgb8);
        QCOMPARE(plan.hdr.transfer, LayerTransfer::Sdr);
        QCOMPARE(plan.hdr.primaries, LayerPrimaries::Bt709);
        QCOMPARE(plan.decode.curve, DecodeCurve::Power);
        QCOMPARE(plan.decode.exponent, 2.2f);
        QVERIFY(plan.decode.encodeSrgb);
        QVERIFY(!plan.decode.convert);
        QVERIFY(plan.notes.isEmpty());
    }

    void untaggedCanStayPiecewiseSrgb()
    {
        // RIFT_HOME_UNTAGGED=srgb: the earlier behaviour, a direct copy the runtime decodes as sRGB.
        const LayerPlan plan = planLayer({}, PixelKind::Unorm8, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4);
        QCOMPARE(plan.path, LayerPath::Direct);
        QCOMPARE(plan.format, SwapchainFormat::Srgb8);
        QCOMPARE(plan.decode, LayerDecode{});
    }

    void compoundPower24IsTheRuntimesOwnSrgb()
    {
        // Rift Home's own art and apps that ask for it: sRGB's piece-wise curve, decoded by the runtime as it is.
        SurfaceColorDescription d;
        d.tagged = true;
        d.transferNamed = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4;
        d.primariesNamed = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB;
        const LayerPlan plan = planLayer(d, PixelKind::Unorm8);
        QCOMPARE(plan.path, LayerPath::Direct);
        QCOMPARE(plan.decode, LayerDecode{});
        // In 10 bits: decoded here with the same curve, to linear float, still SDR.
        const LayerPlan deep = planLayer(d, PixelKind::Unorm10);
        QCOMPARE(deep.path, LayerPath::ConvertToLinearFloat);
        QCOMPARE(deep.hdr.transfer, LayerTransfer::Sdr);
        QCOMPARE(deep.decode.curve, DecodeCurve::Srgb);
        QVERIFY(!deep.decode.encodeSrgb);
    }

    void bt1886IsAPurePower24()
    {
        SurfaceColorDescription d;
        d.tagged = true;
        d.transferNamed = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886;
        d.primariesNamed = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB;
        const LayerPlan plan = planLayer(d, PixelKind::Unorm8);
        QCOMPARE(plan.path, LayerPath::ReencodeSrgb);
        QCOMPARE(plan.decode.curve, DecodeCurve::Power);
        QCOMPARE(plan.decode.exponent, 2.4f);
    }

    void sdrInBt2020IsMappedHere()
    {
        // The runtime maps primaries for HDR layers only: SDR in BT.2020 goes onto BT.709 in the window pass.
        SurfaceColorDescription d;
        d.tagged = true;
        d.transferNamed = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22;
        d.primariesNamed = WP_COLOR_MANAGER_V1_PRIMARIES_BT2020;
        const LayerPlan plan = planLayer(d, PixelKind::Unorm8);
        QCOMPARE(plan.path, LayerPath::ReencodeSrgb);
        QVERIFY(plan.decode.convert);
        QVERIFY(std::abs(plan.decode.matrix[0] - 1.660491f) < 1e-3f); // BT.2087's BT.2020 to BT.709
        QCOMPARE(plan.hdr.primaries, LayerPrimaries::Bt709);
        QCOMPARE(plan.notes.size(), 1);
        // sRGB's own curve in BT.2020 cannot be a direct copy either.
        d.transferNamed = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4;
        QCOMPARE(planLayer(d, PixelKind::Unorm8).path, LayerPath::ReencodeSrgb);
    }

    void hdr10ForwardsItsMetadata()
    {
        SurfaceColorDescription d;
        d.tagged = true;
        d.transferNamed = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ;
        d.primariesNamed = WP_COLOR_MANAGER_V1_PRIMARIES_BT2020;
        d.hasMasteringLuminance = true;
        d.masteringMinNits = 0.0001f;
        d.masteringMaxNits = 1000.0f;
        d.hasMasteringPrimaries = true;
        d.maxCllNits = 1000.0f;
        d.maxFallNits = 400.0f;

        const LayerPlan plan = planLayer(d, PixelKind::Unorm10);
        QCOMPARE(plan.path, LayerPath::Direct);
        QCOMPARE(plan.format, SwapchainFormat::Unorm10);
        QCOMPARE(plan.hdr.transfer, LayerTransfer::Pq);
        QCOMPARE(plan.hdr.primaries, LayerPrimaries::Bt2020);
        QCOMPARE(plan.hdr.masteringMinNits, 0.0001f);
        QCOMPARE(plan.hdr.masteringMaxNits, 1000.0f);
        QCOMPARE(plan.hdr.maxCllNits, 1000.0f);
        QCOMPARE(plan.hdr.maxFallNits, 400.0f);
        QCOMPARE(plan.hdr.referenceWhiteNits, 0.0f);
        // Mastering primaries are not in the layer extension: said, not dropped silently.
        QCOMPARE(plan.notes.size(), 1);
    }

    void scRgbIsLinearWithItsWhite()
    {
        SurfaceColorDescription d;
        d.tagged = true;
        d.transferNamed = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR;
        d.primariesNamed = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB;

        const LayerPlan plan = planLayer(d, PixelKind::Float16);
        QCOMPARE(plan.path, LayerPath::Direct);
        QCOMPARE(plan.format, SwapchainFormat::Float16);
        QCOMPARE(plan.hdr.transfer, LayerTransfer::Linear);
        QCOMPARE(plan.hdr.referenceWhiteNits, 80.0f);
    }

    void gamma22IsDecodedExactly()
    {
        SurfaceColorDescription d;
        d.tagged = true;
        d.transferNamed = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22;
        d.primariesNamed = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB;

        const LayerPlan plan = planLayer(d, PixelKind::Unorm8);
        QCOMPARE(plan.path, LayerPath::ReencodeSrgb);
        QCOMPARE(plan.format, SwapchainFormat::Srgb8);
        QCOMPARE(plan.decode.exponent, 2.2f);
        QVERIFY(plan.notes.isEmpty());
    }

    void sdrIn10BitIsDecodedFirst()
    {
        // Linear float, but SDR: no HDR mapping in the runtime (as bright as an 8-bit window).
        const LayerPlan plan = planLayer({}, PixelKind::Unorm10);
        QCOMPARE(plan.path, LayerPath::ConvertToLinearFloat);
        QCOMPARE(plan.format, SwapchainFormat::Float16);
        QCOMPARE(plan.hdr.transfer, LayerTransfer::Sdr);
        QCOMPARE(plan.decode.curve, DecodeCurve::Power);
        QVERIFY(!plan.decode.encodeSrgb);
    }

    void hlgIsDecodedToLinearHdr()
    {
        // HLG: decoded here (its OOTF for a 1000 cd/m² display), linear BT.2020 with 1.0 = 203 cd/m², peak 1000.
        SurfaceColorDescription d;
        d.tagged = true;
        d.transferNamed = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG;
        d.primariesNamed = WP_COLOR_MANAGER_V1_PRIMARIES_BT2020;
        const LayerPlan plan = planLayer(d, PixelKind::Unorm10);
        QCOMPARE(plan.path, LayerPath::ConvertToLinearFloat);
        QCOMPARE(plan.format, SwapchainFormat::Float16);
        QCOMPARE(plan.hdr.transfer, LayerTransfer::Linear);
        QCOMPARE(plan.hdr.primaries, LayerPrimaries::Bt2020);
        QCOMPARE(plan.hdr.referenceWhiteNits, 203.0f);
        QCOMPARE(plan.hdr.maxCllNits, 1000.0f);
        QCOMPARE(plan.decode.curve, DecodeCurve::Hlg);
        QVERIFY(std::abs(plan.decode.gain - 1000.0f / 203.0f) < 1e-4f);
        QVERIFY(!plan.decode.convert); // already BT.2020
        QVERIFY(std::abs(plan.decode.luma[1] - 0.678f) < 1e-3f); // BT.2020's green weight
    }

    void displayP3IsConverted()
    {
        // Display P3 with sRGB's curve (photos, web pages): SDR, onto BT.709 here.
        SurfaceColorDescription d;
        d.tagged = true;
        d.transferNamed = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4;
        d.primariesNamed = WP_COLOR_MANAGER_V1_PRIMARIES_DISPLAY_P3;
        const LayerPlan sdr = planLayer(d, PixelKind::Unorm8);
        QCOMPARE(sdr.path, LayerPath::ReencodeSrgb);
        QCOMPARE(sdr.hdr.transfer, LayerTransfer::Sdr);
        QVERIFY(sdr.decode.convert);
        QCOMPARE(sdr.decode.curve, DecodeCurve::Srgb);
        // P3's red is more saturated than BT.709's: its pure red has negative green and blue in BT.709.
        QVERIFY(sdr.decode.matrix[0] > 1.2f && sdr.decode.matrix[3] < 0.0f);
        // PQ in Display P3: decoded here to linear BT.2020 (the runtime takes PQ in BT.709 or BT.2020 only).
        d.transferNamed = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ;
        const LayerPlan hdr = planLayer(d, PixelKind::Unorm10);
        QCOMPARE(hdr.path, LayerPath::ConvertToLinearFloat);
        QCOMPARE(hdr.hdr.transfer, LayerTransfer::Linear);
        QCOMPARE(hdr.hdr.primaries, LayerPrimaries::Bt2020);
        QCOMPARE(hdr.decode.curve, DecodeCurve::Pq);
        QVERIFY(std::abs(hdr.decode.gain - 10000.0f / 203.0f) < 1e-3f);
    }

    void otherCurvesAndPowers()
    {
        SurfaceColorDescription d;
        d.tagged = true;
        d.primariesNamed = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB;
        d.transferNamed = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA28;
        QCOMPARE(planLayer(d, PixelKind::Unorm8).decode.exponent, 2.8f);
        d.transferNamed = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST240;
        QCOMPARE(planLayer(d, PixelKind::Unorm8).decode.curve, DecodeCurve::St240);
        d.transferNamed = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_LOG_316;
        QCOMPARE(planLayer(d, PixelKind::Unorm8).decode.curve, DecodeCurve::Log316);
        // set_tf_power: its own exponent.
        d.transferNamed = 0;
        d.tfPower = 2.6f;
        const LayerPlan power = planLayer(d, PixelKind::Unorm8);
        QCOMPARE(power.decode.curve, DecodeCurve::Power);
        QCOMPARE(power.decode.exponent, 2.6f);
        // A reference white of its own (set_luminances) for PQ.
        d.tfPower = 0;
        d.transferNamed = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ;
        d.hasLuminances = true;
        d.referenceNits = 100.0f;
        QCOMPARE(planLayer(d, PixelKind::Unorm10).hdr.referenceWhiteNits, 100.0f);
    }

    void primariesMath()
    {
        // BT.2020 to BT.709 as ITU-R BT.2087 gives it; whites to white; DCI-P3's white adapted to D65.
        const Matrix3 m = conversion(bt2020(), bt709());
        const float bt2087[9] = {1.660491f, -0.587641f, -0.072850f, -0.124550f, 1.132900f, -0.008349f, -0.018151f, -0.100579f, 1.118730f};
        for (int i = 0; i < 9; ++i) {
            QVERIFY2(std::abs(m[size_t(i)] - bt2087[i]) < 2e-3f, qPrintable(QString::number(i)));
        }
        for (uint32_t named : {1u, 2u, 3u, 4u, 5u, 6u, 8u, 9u, 10u}) {
            const Matrix3 c = conversion(*namedPrimaries(named), bt709());
            for (int row = 0; row < 3; ++row) {
                const float white = c[size_t(row * 3)] + c[size_t(row * 3 + 1)] + c[size_t(row * 3 + 2)];
                QVERIFY2(std::abs(white - 1.0f) < 2e-3f, qPrintable(QStringLiteral("primaries %1 row %2: %3").arg(named).arg(row).arg(white)));
            }
        }
        QVERIFY(!namedPrimaries(77));
        const auto weights = luminanceWeights(bt709());
        QVERIFY(std::abs(weights[0] - 0.2126f) < 1e-3f && std::abs(weights[1] - 0.7152f) < 1e-3f);
    }

    void unknownPixelsHaveNoPath()
    {
        QCOMPARE(planLayer({}, PixelKind::Unknown).path, LayerPath::Unsupported);
    }
};

QTEST_GUILESS_MAIN(TestLayerPlan)
#include "tst_layerplan.moc"
