// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "spatial/Math.hpp"

#include <QImage>

#include <memory>
#include <vector>

namespace RiftHome::Xr::ViewShot
{

/*!
 * One picture of what the user sees: the composition layers Rift Home sends to the runtime, drawn the way the runtime
 * draws them, from the head. A screenshot of the headset's own output does not exist (the runtime owns it), so the
 * layers are ray-cast again here: every output pixel looks along its ray, and each layer, back to front, is
 * intersected, sampled and blended over the result. Pure (no Vulkan, no OpenXR): the geometry follows the OpenXR
 * layer definitions, the pixels are linear light where 1.0 is the reference white.
 */

//! Pixels in linear light, premultiplied alpha, row by row (the top row first), 4 floats a pixel.
struct Texture {
    int width = 0;
    int height = 0;
    std::vector<float> rgba;
};

enum class Kind {
    Quad, //!< XrCompositionLayerQuad: @c width x @c height metres, facing +Z of its pose, visible from the front only
    Cylinder, //!< XR_KHR_composition_layer_cylinder: seen from inside, around -Z of its pose, axis along its Y
    Equirect, //!< XR_KHR_composition_layer_equirect2: a sphere (or sky, radius 0) around its pose, centre at -Z
};

struct Layer {
    Kind kind = Kind::Quad;
    //! In the room; or in the camera's frame when @c headLocked (a layer in the head's own space).
    Spatial::Pose pose;
    bool headLocked = false;
    float width = 0.0f; //!< quad: metres
    float height = 0.0f;
    float radius = 0.0f; //!< cylinder, equirect: metres (equirect 0: infinitely far)
    float centralAngle = 0.0f; //!< cylinder, equirect: radians
    float aspectRatio = 1.0f; //!< cylinder: the arc's width over its height
    float upperAngle = 0.0f; //!< equirect: radians above the horizon at the picture's top
    float lowerAngle = 0.0f; //!< equirect: radians at its bottom (negative below)
    std::shared_ptr<const Texture> texture;
    //! The part of the texture shown, in texels (the layer's subImage.imageRect); width 0: all of it.
    int rectX = 0;
    int rectY = 0;
    int rectWidth = 0;
    int rectHeight = 0;
    bool blend = false; //!< the layer's alpha counts (premultiplied), else it is opaque
    float colorScale[4] = {1.0f, 1.0f, 1.0f, 1.0f}; //!< XrCompositionLayerColorScaleBiasKHR
    float colorBias[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    /*!
     * Frosted glass, as RiftLinux's compositor draws XR_MNDX_composition_layer_backdrop_blur (render/shaders/layer.comp,
     * backdrop_under): what is composed behind the layer is blurred (a Gaussian of @c blurRadians as a standard
     * deviation, at most 0.06), its saturation set by @c blurSaturation about its luma, the @c blurTint colour (linear,
     * alpha: how much) mixed over it, and that mixed into the picture by the layer's own alpha over @c blurMaskAlpha
     * (full from that alpha up, none where the layer is transparent), before the layer is blended over it. 0 radians:
     * clear.
     */
    float blurRadians = 0.0f;
    float blurSaturation = 1.0f;
    float blurTint[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float blurMaskAlpha = 1.0f;
};

struct Camera {
    Spatial::Pose pose; //!< in the room; looks along -Z, up is +Y
    float horizontalFovDegrees = 100.0f;
    int width = 2560;
    int height = 1600;
};

//! The picture in linear light (RGBA, alpha 1, black where no layer is), the layers drawn in the order given.
Texture render(const Camera &camera, const std::vector<Layer> &layers, int threads = 0);

/*!
 * Linear light to a displayable 8-bit sRGB picture. Values up to a knee just under white pass as they are; above it
 * the colour's brightest channel is rolled off smoothly towards white (no clipping cliff, hue kept), so a window lit
 * above the reference white keeps its shape. @p dither adds a fraction of a step of fixed noise against banding.
 */
QImage toSrgb8(const Texture &image, bool dither = true);
//! The display mapping of one value (the brightest channel); 1.0 stays under 1 (a little below white).
float rollOff(float value);

//! How a layer's read-back pixels are encoded.
enum class Encoding {
    Srgb8, //!< 8-bit, sRGB curve (premultiplied: the runtime decodes the encoded values as they are)
    Unorm8, //!< 8-bit, linear
    HalfLinear, //!< 16-bit floats, linear light (1.0: reference white)
    HalfPq, //!< 16-bit floats, PQ (SMPTE ST 2084) encoded
};

struct Decode {
    Encoding encoding = Encoding::Srgb8;
    float referenceWhiteNits = 0.0f; //!< PQ: nits that count as 1.0; 0: 203
    bool bt2020 = false; //!< the values' primaries, taken to sRGB's
};

//! The PQ curve: an encoded value 0..1 to nits (0..10000).
float pqToNits(float encoded);

/*!
 * @p data's RGBA pixels (8-bit bytes or 16-bit floats by @p decode, rows top down) as linear light. A picture of more
 * than @p maxPixels is box filtered down by a whole factor first (a room's panorama need not be kept at 8K).
 */
Texture decodePixels(const void *data, int width, int height, const Decode &decode, long long maxPixels = 9'000'000);

} // namespace RiftHome::Xr::ViewShot
