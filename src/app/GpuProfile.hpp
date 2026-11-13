// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace RiftHome::App
{

//! A GPU as Rift Home finds it (sysfs and the kernel's display connectors).
struct GpuDevice {
    QString card; //!< /dev/dri/cardN
    QString renderNode; //!< /dev/dri/renderDN; empty: none
    uint32_t vendorId = 0; //!< PCI vendor
    QString kernelDriver; //!< nvidia, amdgpu, radeon, i915, xe, nouveau, ...
    bool bootVga = false; //!< the firmware's display GPU
    bool headset = false; //!< drives a connected display the kernel marks non-desktop: a VR headset
    //! What Vulkan says it is (a dedicated card, or part of the processor); Unknown without Vulkan's answer.
    enum class Kind {
        Unknown,
        Discrete,
        Integrated,
    } kind = Kind::Unknown;
};

enum class GpuVendor {
    Nvidia,
    Amd,
    Intel,
    Other,
};

/*!
 * The system's GPUs and the path Rift Home takes on them, chosen by itself on any machine (NVIDIA, AMD, Intel, open or
 * closed drivers: the best path is detected, with no decisions for the user).
 *
 * - The GPU: the one the headset is plugged into (the runtime renders there; KWin's "primary GPU" is the desktop's, not
 *   the headset's), else a discrete one, else the boot GPU. Rift Home renders on it, so apps (Firefox's GL and VA-API
 *   decoding) work there too and their buffers need no copy between GPUs.
 * - Video decoding by Firefox's own rules (widget/gtk/GfxInfo.cpp): hardware decoding is blocked on all NVIDIA unless
 *   forced (and works there only through nvidia-vaapi-driver, with nvidia-drm.modeset=1); allowed on Intel and on AMD
 *   with Mesa 24.2 or newer (Firefox decides those itself, not forced here).
 */
struct GpuProfile {
    std::vector<GpuDevice> devices;
    std::optional<size_t> chosen; //!< index into devices
    QStringList vaapiDrivers; //!< libva drivers installed: nvidia, radeonsi, iHD, i965, nouveau, r600, ...
    bool nvidiaOpenModules = false; //!< NVIDIA's open kernel modules (the same user space as the closed ones)
    bool nvidiaModeset = false; //!< nvidia-drm.modeset=1

    //! The machine's.
    static GpuProfile detect();
    //! The GPU to use among @p devices: the headset's, else the dedicated one, else the boot GPU, else the first.
    static std::optional<size_t> choose(const std::vector<GpuDevice> &devices);
    static GpuVendor vendorOf(uint32_t pciVendor);

    const GpuDevice *gpu() const;
    GpuVendor vendor() const;
    //! The libva driver for the chosen GPU, if installed and usable: nvidia, radeonsi, iHD (then i965), nouveau; else "".
    QString vaapiDriver() const;
    //! Apps' environment for decoding video on the chosen GPU (only these names; the user's own values win).
    std::vector<std::pair<QString, QString>> appEnvironment() const;
    //! Firefox's preferences for HDR video on the chosen GPU (name, value in prefs.js syntax).
    std::vector<std::pair<QString, QString>> firefoxPrefs() const;
    //! What the best path lacks on this machine ("" nothing), for the user: a package or a kernel setting.
    QString missing() const;
    //! One line for the log.
    QString describe() const;
};

} // namespace RiftHome::App
