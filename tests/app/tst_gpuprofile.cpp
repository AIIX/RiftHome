// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "app/GpuProfile.hpp"

#include <QTest>

#include <algorithm>

using namespace RiftHome::App;

namespace
{
GpuDevice device(int index, uint32_t vendor, const char *driver, bool bootVga, bool headset)
{
    GpuDevice gpu;
    gpu.card = QStringLiteral("/dev/dri/card%1").arg(index);
    gpu.renderNode = QStringLiteral("/dev/dri/renderD%1").arg(128 + index);
    gpu.vendorId = vendor;
    gpu.kernelDriver = QString::fromLatin1(driver);
    gpu.bootVga = bootVga;
    gpu.headset = headset;
    return gpu;
}

bool has(const std::vector<std::pair<QString, QString>> &list, const char *name, const char *value = nullptr)
{
    return std::any_of(list.begin(), list.end(), [&](const auto &entry) {
        return entry.first == QLatin1String(name) && (!value || entry.second == QLatin1String(value));
    });
}
} // namespace

class TestGpuProfile : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void headsetGpuWins()
    {
        // A laptop: Intel boots, the headset is on the NVIDIA GPU's HDMI port.
        const std::vector<GpuDevice> gpus = {device(0, 0x8086, "i915", true, false), device(1, 0x10de, "nvidia", false, true)};
        QVERIFY(GpuProfile::choose(gpus) == std::optional<size_t>(1));
        // Even when the headset is on the integrated GPU.
        const std::vector<GpuDevice> other = {device(0, 0x8086, "i915", true, true), device(1, 0x10de, "nvidia", false, false)};
        QVERIFY(GpuProfile::choose(other) == std::optional<size_t>(0));
    }

    void withoutHeadsetDiscreteThenBoot()
    {
        const std::vector<GpuDevice> laptop = {device(0, 0x8086, "xe", true, false), device(1, 0x1002, "amdgpu", false, false)};
        QVERIFY(GpuProfile::choose(laptop) == std::optional<size_t>(1));
        const std::vector<GpuDevice> intelOnly = {device(0, 0x8086, "i915", true, false)};
        QVERIFY(GpuProfile::choose(intelOnly) == std::optional<size_t>(0));
        QVERIFY(GpuProfile::choose({}) == std::optional<size_t>());
    }

    void dedicatedByVulkansWord()
    {
        // A desktop: Ryzen's own graphics on, the monitor (boot GPU) on the Radeon card. Vulkan tells them apart.
        std::vector<GpuDevice> ryzen = {device(0, 0x1002, "amdgpu", false, false), device(1, 0x1002, "amdgpu", true, false)};
        ryzen[0].kind = GpuDevice::Kind::Integrated;
        ryzen[1].kind = GpuDevice::Kind::Discrete;
        QVERIFY(GpuProfile::choose(ryzen) == std::optional<size_t>(1));
        // Intel Arc beside Intel's integrated graphics.
        std::vector<GpuDevice> arc = {device(0, 0x8086, "i915", true, false), device(1, 0x8086, "xe", false, false)};
        arc[0].kind = GpuDevice::Kind::Integrated;
        arc[1].kind = GpuDevice::Kind::Discrete;
        QVERIFY(GpuProfile::choose(arc) == std::optional<size_t>(1));
        // A Ryzen laptop with an NVIDIA GPU, without Vulkan's word: NVIDIA's.
        const std::vector<GpuDevice> laptop = {device(0, 0x1002, "amdgpu", true, false), device(1, 0x10de, "nvidia", false, false)};
        QVERIFY(GpuProfile::choose(laptop) == std::optional<size_t>(1));
    }

    void nvidiaPath()
    {
        GpuProfile profile;
        profile.devices = {device(1, 0x10de, "nvidia", true, true)};
        profile.chosen = 0;
        profile.nvidiaModeset = true;
        profile.vaapiDrivers = {QStringLiteral("nvidia")};
        QCOMPARE(profile.vaapiDriver(), QStringLiteral("nvidia"));
        QVERIFY(has(profile.appEnvironment(), "LIBVA_DRIVER_NAME", "nvidia"));
        QVERIFY(has(profile.appEnvironment(), "NVD_BACKEND", "direct"));
        QVERIFY(has(profile.appEnvironment(), "MOZ_DISABLE_RDD_SANDBOX", "1"));
        // Firefox blocks hardware decoding on every NVIDIA GPU: forced, with the HDR preferences.
        QVERIFY(has(profile.firefoxPrefs(), "media.hardware-video-decoding.force-enabled", "true"));
        QVERIFY(has(profile.firefoxPrefs(), "widget.dmabuf.force-enabled", "true"));
        QVERIFY(has(profile.firefoxPrefs(), "gfx.wayland.hdr", "true"));
        QVERIFY(profile.missing().isEmpty());
    }

    void nvidiaWithoutItsVideoDriver()
    {
        GpuProfile profile;
        profile.devices = {device(1, 0x10de, "nvidia", true, true)};
        profile.chosen = 0;
        profile.nvidiaModeset = true;
        QVERIFY(profile.vaapiDriver().isEmpty());
        QVERIFY(profile.appEnvironment().empty());
        QVERIFY(!has(profile.firefoxPrefs(), "media.hardware-video-decoding.force-enabled")); // would only fail there
        QVERIFY(profile.missing().contains(QLatin1String("nvidia-vaapi-driver")));
        // Installed, but nvidia-drm without modeset: unusable, said so.
        profile.vaapiDrivers = {QStringLiteral("nvidia")};
        profile.nvidiaModeset = false;
        QVERIFY(profile.vaapiDriver().isEmpty());
        QVERIFY(profile.missing().contains(QLatin1String("modeset")));
    }

    void amdAndIntelLeftToFirefox()
    {
        GpuProfile amd;
        amd.devices = {device(0, 0x1002, "amdgpu", true, true)};
        amd.chosen = 0;
        amd.vaapiDrivers = {QStringLiteral("radeonsi")};
        QCOMPARE(amd.vaapiDriver(), QStringLiteral("radeonsi"));
        QVERIFY(amd.appEnvironment().empty()); // one GPU: libva finds radeonsi itself
        QVERIFY(!has(amd.firefoxPrefs(), "media.hardware-video-decoding.force-enabled"));
        QVERIFY(has(amd.firefoxPrefs(), "gfx.wayland.hdr", "true"));
        QVERIFY(amd.missing().isEmpty());

        GpuProfile intel;
        intel.devices = {device(0, 0x8086, "xe", true, true), device(1, 0x10de, "nvidia", false, false)};
        intel.chosen = 0;
        intel.vaapiDrivers = {QStringLiteral("i965"), QStringLiteral("nvidia")};
        QVERIFY(intel.vaapiDriver().isEmpty()); // xe takes intel-media-driver (iHD) only
        QVERIFY(intel.missing().contains(QLatin1String("intel-media-driver")));
        intel.vaapiDrivers << QStringLiteral("iHD");
        QCOMPARE(intel.vaapiDriver(), QStringLiteral("iHD"));
        QVERIFY(has(intel.appEnvironment(), "LIBVA_DRIVER_NAME", "iHD")); // two GPUs: named
    }
};

QTEST_GUILESS_MAIN(TestGpuProfile)
#include "tst_gpuprofile.moc"
