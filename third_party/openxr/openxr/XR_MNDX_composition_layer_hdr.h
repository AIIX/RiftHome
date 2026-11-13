// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: BSL-1.0
/*
 * XR_MNDX_composition_layer_hdr (RiftLinux, provisional): the HDR description of a composition layer.
 *
 * Provenance: RiftLinux's runtime (https://github.com/AIIX/RiftLinux),
 * src/external/openxr_includes/openxr/XR_MNDX_composition_layer_hdr.h. Copied unchanged in its
 * declarations; Monado's openxr_extension_helpers.h (XR_STRUCT_ENUM) replaced by the constant it defines. Keep the
 * structure identical to the runtime's: it is read by the runtime as is.
 */
#ifndef XR_MNDX_COMPOSITION_LAYER_HDR_H
#define XR_MNDX_COMPOSITION_LAYER_HDR_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

// Extension number 2001 (2000 prefix), provisional.
#define XR_MNDX_composition_layer_hdr 1
#define XR_MNDX_composition_layer_hdr_SPEC_VERSION 1
#define XR_MNDX_COMPOSITION_LAYER_HDR_EXTENSION_NAME "XR_MNDX_composition_layer_hdr"

//! How a layer's values are encoded.
typedef enum XrHdrTransferMNDX
{
	//! As the swapchain's format says (sRGB formats decoded, others as they are), white at 1.0: SDR.
	XR_HDR_TRANSFER_SDR_MNDX = 0,
	//! Linear light, 1.0 being the reference white, values above it brighter (a float swapchain).
	XR_HDR_TRANSFER_LINEAR_MNDX = 1,
	//! SMPTE ST 2084 (PQ), 1.0 being 10 000 nits: HDR10, HDR10+, Dolby Vision after the player's reshaping.
	XR_HDR_TRANSFER_PQ_MNDX = 2,
	XR_HDR_TRANSFER_MAX_ENUM_MNDX = 0x7FFFFFFF
} XrHdrTransferMNDX;

//! The primaries of a layer's values.
typedef enum XrHdrPrimariesMNDX
{
	XR_HDR_PRIMARIES_BT709_MNDX = 0,
	XR_HDR_PRIMARIES_BT2020_MNDX = 1,
	XR_HDR_PRIMARIES_MAX_ENUM_MNDX = 0x7FFFFFFF
} XrHdrPrimariesMNDX;

static const XrStructureType XR_TYPE_COMPOSITION_LAYER_HDR_MNDX = (XrStructureType)1002000001;

/*!
 * XrCompositionLayerHdrMNDX extends any XrCompositionLayer* structure. Brightness values are in nits; 0 means not
 * given. The scene values are per frame: give them with each xrEndFrame as the content changes.
 */
typedef struct XrCompositionLayerHdrMNDX
{
	XrStructureType type;
	const void *XR_MAY_ALIAS next;
	XrHdrTransferMNDX transfer;
	XrHdrPrimariesMNDX primaries;
	//! The content's reference (diffuse) white; 0: 203 nits (ITU-R BT.2408).
	float referenceWhiteNits;
	//! SMPTE ST 2086 mastering display.
	float masteringMinNits;
	float masteringMaxNits;
	//! CTA-861.3 content light levels (MaxCLL, MaxFALL).
	float maxContentLightLevelNits;
	float maxFrameAverageLightLevelNits;
	//! This frame's scene (HDR10+ SMPTE ST 2094-40, Dolby Vision L1): its peak and average.
	float sceneMaxNits;
	float sceneAverageNits;
} XrCompositionLayerHdrMNDX;

#ifdef __cplusplus
}
#endif

#endif
