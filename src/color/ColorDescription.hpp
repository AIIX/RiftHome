// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "color/Primaries.hpp"

#include <QString>
#include <QStringList>

#include <cstdint>

namespace RiftHome::Color
{

/*!
 * What a Wayland surface says about its colours through color-management-v1, or that it says nothing
 * (untagged: sRGB by the protocol's default). Values are the protocol's named enums, 0 when unset.
 */
struct SurfaceColorDescription {
    bool tagged = false;
    uint32_t transferNamed = 0; //!< enum wp_color_manager_v1_transfer_function
    uint32_t primariesNamed = 0; //!< enum wp_color_manager_v1_primaries
    float tfPower = 0; //!< set_tf_power's exponent (0: a named transfer function)
    bool hasPrimaries = false; //!< set_primaries' chromaticities instead of named ones
    Chromaticities primaries;
    bool hasLuminances = false; //!< set_luminances
    float minNits = 0;
    float maxNits = 0;
    float referenceNits = 0;
    bool hasMasteringPrimaries = false;
    bool hasMasteringLuminance = false;
    float masteringMinNits = 0;
    float masteringMaxNits = 0;
    float maxCllNits = 0; //!< 0: not given
    float maxFallNits = 0; //!< 0: not given

    bool operator==(const SurfaceColorDescription &) const = default;
    QString toString() const;
};

//! How a layer's values are encoded; the values of XrHdrTransferMNDX (XR_MNDX_composition_layer_hdr).
enum class LayerTransfer : uint32_t {
    Sdr = 0,
    Linear = 1,
    Pq = 2,
};

//! The primaries of a layer's values; the values of XrHdrPrimariesMNDX.
enum class LayerPrimaries : uint32_t {
    Bt709 = 0,
    Bt2020 = 1,
};

//! The description chained on a window's layer (XrCompositionLayerHdrMNDX without the per-frame scene values).
struct LayerHdrDescription {
    LayerTransfer transfer = LayerTransfer::Sdr;
    LayerPrimaries primaries = LayerPrimaries::Bt709;
    float referenceWhiteNits = 0; //!< 0: the runtime's default, 203 (ITU-R BT.2408)
    float masteringMinNits = 0;
    float masteringMaxNits = 0;
    float maxCllNits = 0;
    float maxFallNits = 0;

    bool operator==(const LayerHdrDescription &) const = default;
};

//! The sample type of a client's buffer.
enum class PixelKind {
    Unorm8,
    Unorm10,
    Float16,
    Unknown,
};

//! The format of the window's OpenXR swapchain.
enum class SwapchainFormat {
    Srgb8, //!< 8-bit, sampled as sRGB (the runtime decodes it)
    Unorm8, //!< 8-bit, values as they are (PQ or linear encoded)
    Unorm10, //!< A2B10G10R10
    Float16, //!< R16G16B16A16_SFLOAT
};

enum class LayerPath {
    Direct, //!< the buffer is copied as it is
    ReencodeSrgb, //!< 8-bit SDR decoded with its own curve and encoded again as sRGB (the runtime decodes sRGB)
    ConvertToLinearFloat, //!< decoded to linear light in a 16-bit float image first
    Unsupported, //!< no colour-correct path yet: shown only with a diagnostic
};

//! The curve the window pass decodes a buffer's values with; the values of window.comp's `curve`.
enum class DecodeCurve : uint32_t {
    None = 0, //!< linear already, or left to the runtime (an sRGB image, PQ)
    Srgb = 1, //!< IEC 61966-2-1's piece-wise curve (compound_power_2_4)
    Power = 2, //!< a pure power: gamma 2.2 and 2.8, BT.1886's 2.4 (black at 0, as on the CV1's OLED), set_tf_power
    Pq = 3, //!< SMPTE ST 2084: 1.0 = 10000 cd/m²
    Hlg = 4, //!< BT.2100 HLG with its OOTF for a 1000 cd/m² display: 1.0 = that display's peak
    St240 = 5, //!< SMPTE ST 240
    Log100 = 6, //!< logarithmic, 100:1
    Log316 = 7, //!< logarithmic, 100 x sqrt(10):1
};

//! What the window pass does to the values on the way; nothing for a direct copy.
struct LayerDecode {
    DecodeCurve curve = DecodeCurve::None;
    float exponent = 1.0f; //!< Power's
    bool convert = false; //!< matrix applied: onto the layer's primaries (clipped at 0)
    Matrix3 matrix = kIdentity3;
    std::array<float, 3> luma{0.2627f, 0.6780f, 0.0593f}; //!< HLG's OOTF: the source primaries' luminance weights
    float gain = 1.0f; //!< after decoding: to the layer's scale (PQ: 10000 / reference white; HLG: 1000 / reference white)
    bool encodeSrgb = false; //!< encoded again with sRGB's curve for an 8-bit sRGB swapchain

    bool operator==(const LayerDecode &) const = default;
};

//! How one window reaches the headset: the copy, the swapchain format and the description the runtime gets.
struct LayerPlan {
    LayerPath path = LayerPath::Unsupported;
    SwapchainFormat format = SwapchainFormat::Srgb8;
    LayerHdrDescription hdr;
    LayerDecode decode;
    //! Approximations and values not forwarded. Logged, never dropped silently.
    QStringList notes;

    QString toString() const;
};

/*!
 * The transfer function an untagged surface is decoded with: gamma 2.2, the protocol's rule since its version 2 ("use
 * gamma22 for video, motion picture and computer graphics"; IEC 61966-2-1's reference display), as wlroots and KWin
 * show untagged windows. RIFT_HOME_UNTAGGED=srgb: the piece-wise sRGB curve instead (Rift Home's earlier behaviour:
 * lighter shadows), for comparing.
 */
uint32_t untaggedTransfer();

/*!
 * The layer for a window with this description and buffer: the mapping to a layer. Pure
 * function, no OpenXR or wlroots involved. @p untagged: the transfer function of a surface without a description.
 */
LayerPlan planLayer(const SurfaceColorDescription &description, PixelKind pixels, uint32_t untagged = 2 /* gamma22 */);

QString transferName(uint32_t transferNamed);
QString primariesName(uint32_t primariesNamed);
QString describe(LayerPath path);
QString describe(SwapchainFormat format);
QString describe(LayerTransfer transfer);
QString describe(LayerPrimaries primaries);

} // namespace RiftHome::Color
