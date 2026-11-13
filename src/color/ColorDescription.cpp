// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "color/ColorDescription.hpp"

#include <wayland-protocols/color-management-v1-enum.h>

#include <QtGlobal>

namespace RiftHome::Color
{

namespace
{

//! scRGB and ext_linear with sRGB primaries: 1.0 is 80 cd/m² (the protocol's sRGB default luminances).
constexpr float kLinearReferenceWhiteNits = 80.0f;
//! PQ's and HLG's reference white by the protocol's default (BT.2408).
constexpr float kPqReferenceWhiteNits = 203.0f;
//! HLG's nominal display peak, the protocol's assumption for its absolute values.
constexpr float kHlgPeakNits = 1000.0f;

QString nits(float value)
{
    return QString::number(value, 'g', 6);
}

} // namespace

QString transferName(uint32_t tf)
{
    switch (tf) {
    case 0:
        return QStringLiteral("unset");
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886:
        return QStringLiteral("bt1886");
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22:
        return QStringLiteral("gamma22");
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA28:
        return QStringLiteral("gamma28");
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST240:
        return QStringLiteral("st240");
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR:
        return QStringLiteral("ext_linear");
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_LOG_100:
        return QStringLiteral("log_100");
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_LOG_316:
        return QStringLiteral("log_316");
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_XVYCC:
        return QStringLiteral("xvycc");
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB:
        return QStringLiteral("srgb");
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_SRGB:
        return QStringLiteral("ext_srgb");
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ:
        return QStringLiteral("pq");
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST428:
        return QStringLiteral("st428");
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG:
        return QStringLiteral("hlg");
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4:
        return QStringLiteral("compound_power_2_4");
    default:
        return QStringLiteral("tf%1").arg(tf);
    }
}

QString primariesName(uint32_t primaries)
{
    switch (primaries) {
    case 0:
        return QStringLiteral("unset");
    case WP_COLOR_MANAGER_V1_PRIMARIES_SRGB:
        return QStringLiteral("srgb");
    case WP_COLOR_MANAGER_V1_PRIMARIES_PAL_M:
        return QStringLiteral("pal_m");
    case WP_COLOR_MANAGER_V1_PRIMARIES_PAL:
        return QStringLiteral("pal");
    case WP_COLOR_MANAGER_V1_PRIMARIES_NTSC:
        return QStringLiteral("ntsc");
    case WP_COLOR_MANAGER_V1_PRIMARIES_GENERIC_FILM:
        return QStringLiteral("generic_film");
    case WP_COLOR_MANAGER_V1_PRIMARIES_BT2020:
        return QStringLiteral("bt2020");
    case WP_COLOR_MANAGER_V1_PRIMARIES_CIE1931_XYZ:
        return QStringLiteral("cie1931_xyz");
    case WP_COLOR_MANAGER_V1_PRIMARIES_DCI_P3:
        return QStringLiteral("dci_p3");
    case WP_COLOR_MANAGER_V1_PRIMARIES_DISPLAY_P3:
        return QStringLiteral("display_p3");
    case WP_COLOR_MANAGER_V1_PRIMARIES_ADOBE_RGB:
        return QStringLiteral("adobe_rgb");
    default:
        return QStringLiteral("primaries%1").arg(primaries);
    }
}

QString describe(LayerPath path)
{
    switch (path) {
    case LayerPath::Direct:
        return QStringLiteral("direct copy");
    case LayerPath::ReencodeSrgb:
        return QStringLiteral("re-encoded as sRGB");
    case LayerPath::ConvertToLinearFloat:
        return QStringLiteral("convert to linear float");
    case LayerPath::Unsupported:
        return QStringLiteral("UNSUPPORTED");
    }
    return {};
}

QString describe(SwapchainFormat format)
{
    switch (format) {
    case SwapchainFormat::Srgb8:
        return QStringLiteral("8-bit sRGB");
    case SwapchainFormat::Unorm8:
        return QStringLiteral("8-bit");
    case SwapchainFormat::Unorm10:
        return QStringLiteral("10-bit");
    case SwapchainFormat::Float16:
        return QStringLiteral("16-bit float");
    }
    return {};
}

QString describe(LayerTransfer transfer)
{
    switch (transfer) {
    case LayerTransfer::Sdr:
        return QStringLiteral("SDR");
    case LayerTransfer::Linear:
        return QStringLiteral("linear");
    case LayerTransfer::Pq:
        return QStringLiteral("PQ");
    }
    return {};
}

QString describe(LayerPrimaries primaries)
{
    return primaries == LayerPrimaries::Bt2020 ? QStringLiteral("BT.2020") : QStringLiteral("BT.709");
}

QString SurfaceColorDescription::toString() const
{
    if (!tagged) {
        return QStringLiteral("untagged");
    }
    QString s = QStringLiteral("%1 %2").arg(transferName(transferNamed), primariesName(primariesNamed));
    if (hasMasteringLuminance) {
        s += QStringLiteral(" mastering %1-%2 nits").arg(nits(masteringMinNits), nits(masteringMaxNits));
    }
    if (hasMasteringPrimaries) {
        s += QStringLiteral(" +mastering primaries");
    }
    if (maxCllNits > 0) {
        s += QStringLiteral(" MaxCLL %1").arg(nits(maxCllNits));
    }
    if (maxFallNits > 0) {
        s += QStringLiteral(" MaxFALL %1").arg(nits(maxFallNits));
    }
    return s;
}

QString LayerPlan::toString() const
{
    if (path == LayerPath::Unsupported) {
        return QStringLiteral("UNSUPPORTED (%1)").arg(notes.join(QStringLiteral("; ")));
    }
    QString s = QStringLiteral("%1 %2, %3, %4")
                    .arg(describe(hdr.transfer), describe(hdr.primaries), describe(format),
                         describe(path));
    switch (decode.curve) {
    case DecodeCurve::None:
        break;
    case DecodeCurve::Srgb:
        s += QStringLiteral(" (sRGB curve)");
        break;
    case DecodeCurve::Power:
        s += QStringLiteral(" (gamma %1)").arg(decode.exponent, 0, 'f', 2);
        break;
    case DecodeCurve::Pq:
        s += QStringLiteral(" (PQ decoded)");
        break;
    case DecodeCurve::Hlg:
        s += QStringLiteral(" (HLG, 1000 cd/m² display)");
        break;
    case DecodeCurve::St240:
        s += QStringLiteral(" (ST 240)");
        break;
    case DecodeCurve::Log100:
    case DecodeCurve::Log316:
        s += QStringLiteral(" (log)");
        break;
    }
    if (decode.convert) {
        s += QStringLiteral(" (primaries converted)");
    }
    if (hdr.referenceWhiteNits > 0) {
        s += QStringLiteral(", white %1").arg(nits(hdr.referenceWhiteNits));
    }
    if (hdr.masteringMaxNits > 0) {
        s += QStringLiteral(", mastering %1-%2").arg(nits(hdr.masteringMinNits), nits(hdr.masteringMaxNits));
    }
    if (hdr.maxCllNits > 0) {
        s += QStringLiteral(", MaxCLL %1").arg(nits(hdr.maxCllNits));
    }
    if (hdr.maxFallNits > 0) {
        s += QStringLiteral(", MaxFALL %1").arg(nits(hdr.maxFallNits));
    }
    if (!notes.isEmpty()) {
        s += QStringLiteral(" [%1]").arg(notes.join(QStringLiteral("; ")));
    }
    return s;
}

uint32_t untaggedTransfer()
{
    static const uint32_t tf = qgetenv("RIFT_HOME_UNTAGGED") == "srgb" ? uint32_t(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4)
                                                                        : uint32_t(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
    return tf;
}

LayerPlan planLayer(const SurfaceColorDescription &d, PixelKind pixels, uint32_t untagged)
{
    LayerPlan plan;
    if (pixels == PixelKind::Unknown) {
        plan.notes << QStringLiteral("pixel format not handled");
        return plan;
    }

    // The values' primaries: named, given as chromaticities, or sRGB's (untagged).
    Chromaticities source = bt709();
    if (d.hasPrimaries) {
        source = d.primaries;
    } else if (d.primariesNamed != 0) {
        const std::optional<Chromaticities> named = namedPrimaries(d.primariesNamed);
        if (!named) {
            plan.notes << QStringLiteral("primaries %1 not handled").arg(primariesName(d.primariesNamed));
            return plan;
        }
        source = *named;
    }
    const bool inBt709 = source == bt709();
    const bool inBt2020 = source == bt2020();

    const uint32_t tf = !d.tagged ? untagged : d.tfPower > 0.0f ? 0u : d.transferNamed;
    const auto takeMetadata = [&] {
        if (d.hasMasteringLuminance) {
            plan.hdr.masteringMinNits = d.masteringMinNits;
            plan.hdr.masteringMaxNits = d.masteringMaxNits;
        }
        plan.hdr.maxCllNits = d.maxCllNits;
        plan.hdr.maxFallNits = d.maxFallNits;
    };
    const auto directFormat = [pixels] {
        switch (pixels) {
        case PixelKind::Unorm8:
            return SwapchainFormat::Unorm8;
        case PixelKind::Unorm10:
            return SwapchainFormat::Unorm10;
        default:
            return SwapchainFormat::Float16;
        }
    };
    // SDR: shown as SDR (no HDR mapping in the runtime, as bright as every other window), decoded with its own curve,
    // in BT.709 (other primaries mapped onto it here, clipped: the runtime maps primaries of HDR layers only). 8-bit:
    // the runtime's sRGB decoding does sRGB's own curve; others are decoded here and encoded again as sRGB (only the
    // deepest codes of the steeper curves end up as black). More bits: decoded to linear float.
    const auto sdr = [&](DecodeCurve curve, float exponent) {
        plan.hdr.transfer = LayerTransfer::Sdr;
        plan.hdr.primaries = LayerPrimaries::Bt709;
        plan.decode.curve = curve;
        plan.decode.exponent = exponent;
        if (!inBt709) {
            plan.decode.convert = true;
            plan.decode.matrix = conversion(source, bt709());
            plan.notes << QStringLiteral("SDR in %1: onto BT.709 here").arg(d.hasPrimaries ? QStringLiteral("its own primaries") : primariesName(d.primariesNamed));
        }
        if (pixels == PixelKind::Unorm8) {
            plan.format = SwapchainFormat::Srgb8;
            if (curve == DecodeCurve::Srgb && !plan.decode.convert) {
                plan.path = LayerPath::Direct;
                plan.decode = {};
            } else {
                plan.path = LayerPath::ReencodeSrgb;
                plan.decode.encodeSrgb = true;
            }
        } else {
            plan.path = LayerPath::ConvertToLinearFloat;
            plan.format = SwapchainFormat::Float16;
            plan.notes << QStringLiteral("SDR in a 10-bit or float buffer: decoded to linear float first");
        }
    };
    // HDR decoded here (HLG; PQ or linear light in primaries the runtime does not take): linear float in BT.2020, 1.0 =
    // the reference white, as the runtime's linear HDR layers are.
    const auto hdrHere = [&](DecodeCurve curve, float gain, float referenceNits) {
        plan.path = LayerPath::ConvertToLinearFloat;
        plan.format = SwapchainFormat::Float16;
        plan.hdr.transfer = LayerTransfer::Linear;
        plan.hdr.primaries = LayerPrimaries::Bt2020;
        plan.hdr.referenceWhiteNits = referenceNits;
        plan.decode.curve = curve;
        plan.decode.gain = gain;
        plan.decode.luma = luminanceWeights(source);
        if (!inBt2020) {
            plan.decode.convert = true;
            plan.decode.matrix = conversion(source, bt2020());
        }
        takeMetadata();
    };
    const float reference = d.hasLuminances && d.referenceNits > 0.0f ? d.referenceNits : 0.0f;

    switch (tf) {
    case 0: // set_tf_power
        sdr(DecodeCurve::Power, d.tfPower);
        break;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4:
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB: // deprecated, not offered; read as the piece-wise curve
        sdr(DecodeCurve::Srgb, 2.4f);
        break;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22:
        sdr(DecodeCurve::Power, 2.2f);
        break;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA28:
        sdr(DecodeCurve::Power, 2.8f);
        break;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886:
        // BT.1886 with the black at 0 (the CV1's OLED): a pure 2.4 power.
        sdr(DecodeCurve::Power, 2.4f);
        break;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST240:
        sdr(DecodeCurve::St240, 1.0f);
        break;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_LOG_100:
        sdr(DecodeCurve::Log100, 1.0f);
        break;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_LOG_316:
        sdr(DecodeCurve::Log316, 1.0f);
        break;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ:
        if (inBt709 || inBt2020) {
            plan.path = LayerPath::Direct;
            plan.format = directFormat();
            plan.hdr.transfer = LayerTransfer::Pq;
            plan.hdr.primaries = inBt2020 ? LayerPrimaries::Bt2020 : LayerPrimaries::Bt709;
            plan.hdr.referenceWhiteNits = reference;
            takeMetadata();
        } else {
            const float white = reference > 0.0f ? reference : kPqReferenceWhiteNits;
            hdrHere(DecodeCurve::Pq, 10000.0f / white, white);
        }
        break;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG: {
        // BT.2100 HLG shown as on its 1000 cd/m² reference display (OOTF gamma 1.2), reference white 203 cd/m²
        // (BT.2408: 75 % HLG), unless the description says otherwise.
        const float white = reference > 0.0f ? reference : kPqReferenceWhiteNits;
        hdrHere(DecodeCurve::Hlg, kHlgPeakNits / white, white);
        if (plan.hdr.masteringMaxNits <= 0.0f && plan.hdr.maxCllNits <= 0.0f) {
            plan.hdr.maxCllNits = kHlgPeakNits;
        }
        break;
    }
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR:
        if (inBt709 || inBt2020) {
            plan.path = LayerPath::Direct;
            plan.format = directFormat();
            plan.hdr.transfer = LayerTransfer::Linear;
            plan.hdr.primaries = inBt2020 ? LayerPrimaries::Bt2020 : LayerPrimaries::Bt709;
            plan.hdr.referenceWhiteNits = reference > 0.0f ? reference : kLinearReferenceWhiteNits;
            takeMetadata();
        } else {
            hdrHere(DecodeCurve::None, 1.0f, reference > 0.0f ? reference : kLinearReferenceWhiteNits);
        }
        break;
    default:
        plan.notes << QStringLiteral("transfer %1 not handled").arg(transferName(tf));
        return plan;
    }

    if (d.hasMasteringPrimaries) {
        plan.notes << QStringLiteral("mastering primaries not forwarded (the runtime maps by luminance)");
    }
    return plan;
}

} // namespace RiftHome::Color
