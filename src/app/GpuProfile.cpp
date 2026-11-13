// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "app/GpuProfile.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

namespace RiftHome::App
{

namespace
{

QString readLine(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromLatin1(file.readAll()).trimmed() : QString();
}

//! A connected connector of @p card the kernel marks non-desktop (its list of headsets' displays, and DisplayID).
bool drivesHeadset(const QString &card)
{
    int fd = ::open(card.toLocal8Bit().constData(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fd = ::open(card.toLocal8Bit().constData(), O_RDONLY | O_CLOEXEC);
    }
    if (fd < 0) {
        return false;
    }
    bool found = false;
    if (drmModeRes *resources = drmModeGetResources(fd)) {
        for (int i = 0; i < resources->count_connectors && !found; ++i) {
            // "Current": no probe (a probe can make a display blink, and the headset may be leased to the runtime).
            drmModeConnector *connector = drmModeGetConnectorCurrent(fd, resources->connectors[i]);
            if (!connector) {
                continue;
            }
            if (connector->connection == DRM_MODE_CONNECTED) {
                if (drmModeObjectProperties *properties = drmModeObjectGetProperties(fd, connector->connector_id, DRM_MODE_OBJECT_CONNECTOR)) {
                    for (uint32_t p = 0; p < properties->count_props && !found; ++p) {
                        if (drmModePropertyRes *property = drmModeGetProperty(fd, properties->props[p])) {
                            found = QByteArray(property->name) == "non-desktop" && properties->prop_values[p] != 0;
                            drmModeFreeProperty(property);
                        }
                    }
                    drmModeFreeObjectProperties(properties);
                }
            }
            drmModeFreeConnector(connector);
        }
        drmModeFreeResources(resources);
    }
    ::close(fd);
    return found;
}

//! Which of @p devices are dedicated cards and which are the processor's own, as Vulkan's drivers say (every vendor's,
//! open or closed), matched by render node (VK_EXT_physical_device_drm).
void classifyWithVulkan(std::vector<GpuDevice> &devices)
{
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "rift-home (GPU)";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS) {
        return;
    }
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> physical(count);
    vkEnumeratePhysicalDevices(instance, &count, physical.data());
    for (VkPhysicalDevice device : physical) {
        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());
        if (std::none_of(extensions.begin(), extensions.end(), [](const VkExtensionProperties &e) {
                return std::strcmp(e.extensionName, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME) == 0;
            })) {
            continue;
        }
        VkPhysicalDeviceDrmPropertiesEXT drm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &drm;
        vkGetPhysicalDeviceProperties2(device, &properties);
        if (!drm.hasRender) {
            continue; // a software renderer
        }
        const dev_t node = makedev(uint32_t(drm.renderMajor), uint32_t(drm.renderMinor));
        for (GpuDevice &gpu : devices) {
            struct stat st{};
            if (!gpu.renderNode.isEmpty() && ::stat(gpu.renderNode.toLocal8Bit().constData(), &st) == 0 && st.st_rdev == node) {
                gpu.kind = properties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? GpuDevice::Kind::Discrete
                    : properties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? GpuDevice::Kind::Integrated
                                                                                                   : GpuDevice::Kind::Unknown;
            }
        }
    }
    vkDestroyInstance(instance, nullptr);
}

QStringList installedVaapiDrivers()
{
    QStringList dirs = qEnvironmentVariable("LIBVA_DRIVERS_PATH").split(QLatin1Char(':'), Qt::SkipEmptyParts);
    if (dirs.isEmpty()) {
        dirs = {QStringLiteral("/usr/lib/dri"), QStringLiteral("/usr/lib64/dri"), QStringLiteral("/usr/lib/x86_64-linux-gnu/dri"),
                QStringLiteral("/usr/lib/aarch64-linux-gnu/dri"), QStringLiteral("/usr/local/lib/dri"), QStringLiteral("/usr/lib/dri-nonfree")};
    }
    QStringList names;
    for (const QString &dir : std::as_const(dirs)) {
        for (const QString &file : QDir(dir).entryList({QStringLiteral("*_drv_video.so")}, QDir::Files)) {
            const QString name = file.chopped(int(sizeof("_drv_video.so") - 1));
            if (!names.contains(name)) {
                names << name;
            }
        }
    }
    return names;
}

QString vendorName(GpuVendor vendor)
{
    switch (vendor) {
    case GpuVendor::Nvidia:
        return QStringLiteral("NVIDIA");
    case GpuVendor::Amd:
        return QStringLiteral("AMD");
    case GpuVendor::Intel:
        return QStringLiteral("Intel");
    case GpuVendor::Other:
        break;
    }
    return QStringLiteral("other");
}

} // namespace

GpuProfile GpuProfile::detect()
{
    GpuProfile profile;
    const QDir drm(QStringLiteral("/sys/class/drm"));
    const QRegularExpression cardName(QStringLiteral("^card(\\d+)$"));
    QStringList cards = drm.entryList({QStringLiteral("card*")}, QDir::Dirs | QDir::System | QDir::NoDotAndDotDot);
    std::sort(cards.begin(), cards.end(), [](const QString &a, const QString &b) {
        return a.mid(4).toInt() < b.mid(4).toInt();
    });
    for (const QString &card : std::as_const(cards)) {
        if (!cardName.match(card).hasMatch()) {
            continue; // card0-HDMI-A-1 and the like: connectors
        }
        const QString device = drm.filePath(card) + QStringLiteral("/device");
        GpuDevice gpu;
        gpu.card = QStringLiteral("/dev/dri/") + card;
        gpu.vendorId = readLine(device + QStringLiteral("/vendor")).toUInt(nullptr, 16);
        gpu.kernelDriver = QFileInfo(QFileInfo(device + QStringLiteral("/driver")).symLinkTarget()).fileName();
        gpu.bootVga = readLine(device + QStringLiteral("/boot_vga")) == QLatin1String("1");
        const QStringList renders = QDir(device + QStringLiteral("/drm")).entryList({QStringLiteral("renderD*")}, QDir::Dirs | QDir::System);
        if (!renders.isEmpty()) {
            gpu.renderNode = QStringLiteral("/dev/dri/") + renders.first();
        }
        gpu.headset = drivesHeadset(gpu.card);
        profile.devices.push_back(gpu);
    }
    if (profile.devices.size() > 1) {
        classifyWithVulkan(profile.devices); // (one GPU: nothing to choose between)
    }
    profile.chosen = choose(profile.devices);
    profile.vaapiDrivers = installedVaapiDrivers();
    profile.nvidiaOpenModules = readLine(QStringLiteral("/proc/driver/nvidia/version")).contains(QLatin1String("Open Kernel Module"));
    profile.nvidiaModeset = readLine(QStringLiteral("/sys/module/nvidia_drm/parameters/modeset")) == QLatin1String("Y");
    return profile;
}

std::optional<size_t> GpuProfile::choose(const std::vector<GpuDevice> &devices)
{
    const auto first = [&](auto &&test) -> std::optional<size_t> {
        for (size_t i = 0; i < devices.size(); ++i) {
            if (!devices[i].renderNode.isEmpty() && test(devices[i])) {
                return i;
            }
        }
        return std::nullopt;
    };
    if (auto i = first([](const GpuDevice &gpu) { return gpu.headset; })) {
        return i;
    }
    // No headset seen (the CV1 shows its display only once a runtime wakes it): the dedicated GPU, where VR outputs
    // are (a laptop's HDMI port is on it; machines have one, with the processor's own beside it at most).
    if (auto i = first([](const GpuDevice &gpu) { return gpu.kind == GpuDevice::Kind::Discrete; })) {
        return i;
    }
    // Without Vulkan's word: NVIDIA's are dedicated; an AMD one that is not the boot GPU next to another is too.
    if (auto i = first([](const GpuDevice &gpu) {
            return gpu.kind == GpuDevice::Kind::Unknown && vendorOf(gpu.vendorId) == GpuVendor::Nvidia;
        })) {
        return i;
    }
    if (auto i = first([&](const GpuDevice &gpu) {
            return gpu.kind == GpuDevice::Kind::Unknown && vendorOf(gpu.vendorId) == GpuVendor::Amd && !gpu.bootVga && devices.size() > 1;
        })) {
        return i;
    }
    if (auto i = first([](const GpuDevice &gpu) { return gpu.bootVga; })) {
        return i;
    }
    return first([](const GpuDevice &) { return true; });
}

GpuVendor GpuProfile::vendorOf(uint32_t pciVendor)
{
    switch (pciVendor) {
    case 0x10de:
        return GpuVendor::Nvidia;
    case 0x1002:
    case 0x1022:
        return GpuVendor::Amd;
    case 0x8086:
        return GpuVendor::Intel;
    default:
        return GpuVendor::Other;
    }
}

const GpuDevice *GpuProfile::gpu() const
{
    return chosen && *chosen < devices.size() ? &devices[*chosen] : nullptr;
}

GpuVendor GpuProfile::vendor() const
{
    return gpu() ? vendorOf(gpu()->vendorId) : GpuVendor::Other;
}

QString GpuProfile::vaapiDriver() const
{
    const GpuDevice *device = gpu();
    if (!device) {
        return {};
    }
    const QString &kernel = device->kernelDriver;
    const auto installed = [this](const QString &name) {
        return vaapiDrivers.contains(name) ? name : QString();
    };
    if (kernel == QLatin1String("nvidia")) {
        return nvidiaModeset ? installed(QStringLiteral("nvidia")) : QString(); // nvidia-vaapi-driver needs modeset
    }
    if (kernel == QLatin1String("amdgpu") || kernel == QLatin1String("radeon")) {
        return installed(QStringLiteral("radeonsi"));
    }
    if (kernel == QLatin1String("i915") || kernel == QLatin1String("xe")) {
        const QString media = installed(QStringLiteral("iHD")); // intel-media-driver: Broadwell on, and every xe GPU
        return !media.isEmpty() || kernel == QLatin1String("xe") ? media : installed(QStringLiteral("i965"));
    }
    if (kernel == QLatin1String("nouveau")) {
        return installed(QStringLiteral("nouveau"));
    }
    return {};
}

std::vector<std::pair<QString, QString>> GpuProfile::appEnvironment() const
{
    std::vector<std::pair<QString, QString>> env;
    const QString driver = vaapiDriver();
    if (driver.isEmpty()) {
        return env;
    }
    if (driver == QLatin1String("nvidia")) {
        // nvidia-vaapi-driver's documented setup: named (libva may not map nvidia-drm to it), its direct backend, and
        // Firefox's decoder process unsandboxed (the driver opens the GPU from there).
        env.emplace_back(QStringLiteral("LIBVA_DRIVER_NAME"), driver);
        env.emplace_back(QStringLiteral("NVD_BACKEND"), QStringLiteral("direct"));
        env.emplace_back(QStringLiteral("MOZ_DISABLE_RDD_SANDBOX"), QStringLiteral("1"));
        return env;
    }
    // Mesa and Intel's driver are found by libva from the GPU itself; named only where another GPU could be taken.
    if (devices.size() > 1) {
        env.emplace_back(QStringLiteral("LIBVA_DRIVER_NAME"), driver);
    }
    return env;
}

std::vector<std::pair<QString, QString>> GpuProfile::firefoxPrefs() const
{
    // HDR: Firefox tags HDR video (and sends it as 10-bit frames) only with these on.
    std::vector<std::pair<QString, QString>> prefs = {
        {QStringLiteral("gfx.wayland.hdr"), QStringLiteral("true")},
        {QStringLiteral("gfx.color_management.hdr"), QStringLiteral("true")},
        {QStringLiteral("gfx.color_management.hdr.force_enabled"), QStringLiteral("true")},
    };
    const QString driver = vaapiDriver();
    if (!driver.isEmpty()) {
        prefs.emplace_back(QStringLiteral("media.ffmpeg.vaapi.enabled"), QStringLiteral("true"));
    }
    if (driver == QLatin1String("nvidia")) {
        // Firefox blocks hardware decoding on every NVIDIA GPU; HDR video comes only from it (its software decoder's
        // 10-bit frames cannot be written into NVIDIA's buffers): forced, with buffers shared with the compositor.
        prefs.emplace_back(QStringLiteral("media.hardware-video-decoding.force-enabled"), QStringLiteral("true"));
        prefs.emplace_back(QStringLiteral("media.rdd-ffmpeg.enabled"), QStringLiteral("true"));
        prefs.emplace_back(QStringLiteral("widget.dmabuf.force-enabled"), QStringLiteral("true"));
    }
    return prefs;
}

QString GpuProfile::missing() const
{
    const GpuDevice *device = gpu();
    if (!device) {
        return QStringLiteral("no GPU found");
    }
    const QString &kernel = device->kernelDriver;
    if (kernel == QLatin1String("nvidia")) {
        if (!vaapiDrivers.contains(QLatin1String("nvidia"))) {
            return QStringLiteral("HDR video in Firefox needs NVIDIA's VA-API driver (package nvidia-vaapi-driver or libva-nvidia-driver)");
        }
        if (!nvidiaModeset) {
            return QStringLiteral("HDR video in Firefox needs the kernel setting nvidia-drm.modeset=1");
        }
    } else if ((kernel == QLatin1String("amdgpu") || kernel == QLatin1String("radeon")) && vaapiDriver().isEmpty()) {
        return QStringLiteral("hardware video decoding needs Mesa's VA-API driver (package libva-mesa-driver or mesa-va-drivers)");
    } else if ((kernel == QLatin1String("i915") || kernel == QLatin1String("xe")) && vaapiDriver().isEmpty()) {
        return QStringLiteral("hardware video decoding needs Intel's VA-API driver (package intel-media-driver)");
    }
    return {};
}

QString GpuProfile::describe() const
{
    QStringList parts;
    for (size_t i = 0; i < devices.size(); ++i) {
        const GpuDevice &gpu = devices[i];
        QString line = QStringLiteral("%1 %2 (%3) %4").arg(vendorName(vendorOf(gpu.vendorId)), gpu.card.section(QLatin1Char('/'), -1), gpu.kernelDriver,
                                                            gpu.renderNode.section(QLatin1Char('/'), -1));
        if (gpu.kernelDriver == QLatin1String("nvidia")) {
            line += nvidiaOpenModules ? QStringLiteral(" open modules") : QStringLiteral(" closed modules");
        }
        if (gpu.kind != GpuDevice::Kind::Unknown) {
            line += gpu.kind == GpuDevice::Kind::Discrete ? QStringLiteral(", dedicated") : QStringLiteral(", integrated");
        }
        if (gpu.headset) {
            line += QStringLiteral(", drives the headset");
        }
        if (gpu.bootVga) {
            line += QStringLiteral(", boot GPU");
        }
        if (chosen && *chosen == i) {
            line += QStringLiteral(" [Rift Home renders here]");
        }
        parts << line;
    }
    const QString driver = vaapiDriver();
    return QStringLiteral("GPUs: %1 | video decoding: %2").arg(parts.isEmpty() ? QStringLiteral("none") : parts.join(QStringLiteral("; ")),
                                                               driver.isEmpty() ? QStringLiteral("software") : QStringLiteral("VA-API ") + driver);
}

} // namespace RiftHome::App
