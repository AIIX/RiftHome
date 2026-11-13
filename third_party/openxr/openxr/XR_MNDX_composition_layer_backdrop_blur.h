// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: BSL-1.0
/*
 * RiftLinux's provisional backdrop blur extension (frosted glass), from RiftLinux's runtime
 * (https://github.com/AIIX/RiftLinux), src/external/openxr_includes/openxr/XR_MNDX_composition_layer_backdrop_blur.h.
 * Copied unchanged in its declarations; Monado's openxr_extension_helpers.h (XR_STRUCT_ENUM) replaced by the constant it
 * defines, as for XR_MNDX_composition_layer_hdr.h. Keep in step with the runtime.
 *
 * What the compositor shows behind a quad or cylinder layer (other applications' layers included) is blurred where the
 * layer covers it, then the layer is blended over it. blurRadians: the Gaussian's standard deviation as an angle (0 off,
 * capped at 0.06); saturation 0..2 (1 keeps the colours); tint (linear rgb) mixed over the blur by tint.a; maskAlpha: the
 * layer's alpha from which the blur is full (it fades to none at alpha 0, so transparent corners stay sharp). At most 4
 * blurred layers a view (the lowest ones); the layer needs the source-alpha blend flag.
 */
#ifndef XR_MNDX_COMPOSITION_LAYER_BACKDROP_BLUR_H
#define XR_MNDX_COMPOSITION_LAYER_BACKDROP_BLUR_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_MNDX_composition_layer_backdrop_blur 1
#define XR_MNDX_composition_layer_backdrop_blur_SPEC_VERSION 1
#define XR_MNDX_COMPOSITION_LAYER_BACKDROP_BLUR_EXTENSION_NAME "XR_MNDX_composition_layer_backdrop_blur"

static const XrStructureType XR_TYPE_COMPOSITION_LAYER_BACKDROP_BLUR_MNDX = (XrStructureType)1002001001;

typedef struct XrCompositionLayerBackdropBlurMNDX
{
	XrStructureType type;
	const void *XR_MAY_ALIAS next;
	float blurRadians;
	float saturation;
	XrColor4f tint;
	float maskAlpha;
} XrCompositionLayerBackdropBlurMNDX;

#ifdef __cplusplus
}
#endif

#endif
