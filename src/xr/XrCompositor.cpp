// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "xr/XrCompositor.hpp"

#include "graphics/PixelFormat.hpp"
#include "graphics/RadianceHdr.hpp"
#include "spatial/PointerFilter.hpp"
#include "spatial/Recline.hpp"
#include "spatial/Sharpness.hpp"
#include "spatial/Dock.hpp"
#include "spatial/Gamepad.hpp"
#include "xr/ControllerRenderer.hpp"
#include "xr/RuntimeApps.hpp"
#include "xr/ViewShot.hpp"
#include "xr/VirtualGamepad.hpp"
#include "spatial/NotificationPanel.hpp"
#include "spatial/LauncherGrid.hpp"
#include "spatial/SpatialLayout.hpp"
#include "spatial/VirtualKeyboard.hpp"
#include "spatial/WindowFrame.hpp"
#include "xr/FrameArt.hpp"
#include "xr/LayerBridge.hpp"
#include "xr/XrInput.hpp"

#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <openxr/XR_MNDX_composition_layer_hdr.h>
#include <openxr/XR_MNDX_composition_layer_backdrop_blur.h>
#include <wayland-protocols/color-management-v1-enum.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>
#include <QThreadPool>
#include <QTime>

#include <cstddef>
#include <unordered_map>

#include <drm_fourcc.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <QFloat16>

#include <algorithm>
#include <array>
#include <chrono>
#include <numeric>
#include <tuple>
#include <cmath>
#include <cstring>
#include <map>
#include <limits>
#include <condition_variable>
#include <deque>
#include <string>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <unordered_set>
#include <vector>

Q_LOGGING_CATEGORY(lcXr, "rifthome.xr")

namespace RiftHome::Xr
{

namespace
{

using Clock = std::chrono::steady_clock;

VkFormat vkFormatFromDrm(uint32_t fourcc)
{
    switch (fourcc) {
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XRGB8888:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_XBGR8888:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case DRM_FORMAT_P010: // its luma plane; the chroma plane (R16G16, half size) is imported beside it
        return VK_FORMAT_R16_UNORM;
    case DRM_FORMAT_ABGR2101010:
    case DRM_FORMAT_XBGR2101010:
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case DRM_FORMAT_ARGB2101010:
    case DRM_FORMAT_XRGB2101010:
        return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    case DRM_FORMAT_ABGR16161616F:
    case DRM_FORMAT_XBGR16161616F:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

uint32_t bytesPerPixel(VkFormat format)
{
    return format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
}

constexpr uint32_t kOpaque = 1; //!< the window pass's flags (window.comp)
constexpr uint32_t kYuvBt2020 = 4; //!< P010: luma and chroma planes, BT.2020 non-constant luminance, limited range

//! How a buffer reaches its swapchain through the window pass.
struct Target {
    VkFormat source; //!< the client's buffer, sampled
    VkFormat swapchain; //!< what the runtime gets
    VkFormat storage; //!< the pass's image, copied bit for bit into the swapchain image (UNORM for an sRGB swapchain)
    bool floatOutput; //!< the rgba16f pipeline, else rgba8
    uint32_t flags;
    Color::LayerDecode decode; //!< the curve and conversions on the way
};

std::optional<Target> chooseTarget(const Color::LayerPlan &plan, uint32_t drmFormat, const std::set<int64_t> &available)
{
    const VkFormat source = vkFormatFromDrm(drmFormat);
    if (plan.path == Color::LayerPath::Unsupported || source == VK_FORMAT_UNDEFINED) {
        return std::nullopt;
    }
    const uint32_t opaque = (Graphics::hasAlpha(drmFormat) ? 0u : kOpaque) | (drmFormat == DRM_FORMAT_P010 ? kYuvBt2020 : 0u);
    Target t{};
    if (plan.path == Color::LayerPath::ConvertToLinearFloat) {
        t = {source, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, true, opaque, plan.decode};
    } else if (plan.path == Color::LayerPath::ReencodeSrgb) {
        t = {source, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, false, opaque, plan.decode};
    } else {
        switch (plan.format) {
        case Color::SwapchainFormat::Srgb8:
            t = {source, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, false, opaque};
            break;
        case Color::SwapchainFormat::Unorm8:
            t = {source, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, false, opaque};
            break;
        case Color::SwapchainFormat::Unorm10: // no 10-bit swapchains in the runtime: float keeps every value
        case Color::SwapchainFormat::Float16:
            t = {source, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, true, opaque};
            break;
        }
    }
    if (!available.contains(t.swapchain)) {
        return std::nullopt;
    }
    return t;
}

//! The window pass's push constants (window.comp).
struct PassParams {
    int32_t size[2];
    int32_t offset[2];
    int32_t windowSize[2];
    float radius;
    uint32_t flags;
    uint32_t curve; //!< Color::DecodeCurve
    float exponent;
    float gain;
    uint32_t convert; //!< 1: the matrix, 2: encoded again as sRGB
    float m[12]; //!< the matrix's rows as vec4s, their w the source's luminance weights (HLG)
    float scale[2]; //!< window pixels a buffer pixel (a video surface shown at another size than its buffer's)
    float yuvMax = 1.0f; //!< P010: the largest encoded value the converted colour may reach (the content's peak in PQ)
    float pad = 0.0f;
};
static_assert(sizeof(PassParams) == 112 && offsetof(PassParams, m) == 48 && offsetof(PassParams, scale) == 96,
              "window.comp's push constants (std430)");

//! Where a surface sits in its window's visible geometry, for the corners.
struct SurfaceGeometry {
    int x = 0;
    int y = 0;
    int windowWidth = 0;
    int windowHeight = 0;
    int width = 0; //!< the surface itself in window pixels (a video's buffer is often shown bigger or smaller: viewporter)
    int height = 0;
};

const uint32_t kWindowPassRgba8[] =
#include "window_rgba8.spv.h"
    ;
const uint32_t kWindowPassRgba16f[] =
#include "window_rgba16f.spv.h"
    ;

//! The resample pass's push constants (resample.comp): sharp windows near and far.
struct DownsampleParams {
    int32_t masterSize[2];
    int32_t shownSize[2];
    uint32_t encoding; //!< 0 linear values, 1 sRGB, 2 PQ
    uint32_t mode; //!< 0 smaller (area filter), 1 twice as big (Lanczos-2)
};

const uint32_t kDownsampleRgba8[] =
#include "resample_rgba8.spv.h"
    ;
const uint32_t kDownsampleRgba16f[] =
#include "resample_rgba16f.spv.h"
    ;

bool fenceSignalled(int fd)
{
    pollfd p{fd, POLLIN, 0};
    return ::poll(&p, 1, 0) > 0;
}

/*!
 * Where each window sits around the user (its centre's angle, positive to the right): as visionOS lays windows out,
 * side by side at the same distance, each turned to face the user. The lowest slot is in front; odd slots go on the
 * right and even ones on the left, each next to the previous one on its side.
 */
std::map<int, float> layoutAngles(std::vector<WindowPlacement> windows, float pixelsPerMeter, float distance, float gapDegrees)
{
    std::sort(windows.begin(), windows.end(), [](const WindowPlacement &a, const WindowPlacement &b) {
        return a.slot < b.slot;
    });
    const float gap = gapDegrees * float(M_PI) / 180.0f;
    std::map<int, float> angles;
    float right = 0.0f;
    float left = 0.0f;
    bool first = true;
    for (const WindowPlacement &window : windows) {
        const float half = std::atan((window.width / pixelsPerMeter / 2.0f) / distance);
        float angle = 0.0f;
        if (first) {
            right = half;
            left = -half;
            first = false;
        } else if (window.slot % 2 == 1) {
            angle = right + gap + half;
            right = angle + half;
        } else {
            angle = left - gap - half;
            left = angle - half;
        }
        angles[window.windowId] = angle;
    }
    return angles;
}

const char *stateName(XrSessionState state)
{
    switch (state) {
    case XR_SESSION_STATE_IDLE:
        return "idle";
    case XR_SESSION_STATE_READY:
        return "ready";
    case XR_SESSION_STATE_SYNCHRONIZED:
        return "synchronized";
    case XR_SESSION_STATE_VISIBLE:
        return "visible";
    case XR_SESSION_STATE_FOCUSED:
        return "focused";
    case XR_SESSION_STATE_STOPPING:
        return "stopping";
    case XR_SESSION_STATE_LOSS_PENDING:
        return "loss pending";
    case XR_SESSION_STATE_EXITING:
        return "exiting";
    default:
        return "unknown";
    }
}

Spatial::Pose toPose(const XrPosef &p)
{
    return {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w}, {p.position.x, p.position.y, p.position.z}};
}

XrPosef toXr(const Spatial::Pose &p)
{
    XrPosef x;
    x.orientation = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
    x.position = {p.position.x, p.position.y, p.position.z};
    return x;
}

//! An orientation whose forward (-Z) is @p direction, upright.
Spatial::Quat lookAlong(Spatial::Vec3 direction)
{
    const Spatial::Vec3 z = (direction * -1.0f).normalized();
    Spatial::Vec3 x = Spatial::Vec3{0.0f, 1.0f, 0.0f}.cross(z).normalized();
    if (x.length() < 0.5f) {
        x = {1.0f, 0.0f, 0.0f};
    }
    return Spatial::Quat::fromBasis(x, z.cross(x), z);
}

float deadzone(float v)
{
    return std::fabs(v) < 0.15f ? 0.0f : v;
}

//! The cursor: a soft white dot with a dark rim (visible on white too), premultiplied RGBA8.
QByteArray cursorPixels(int size, const QColor &colour)
{
    QByteArray px(size * size * 4, '\0');
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float r = std::hypot(x + 0.5f - size / 2.0f, y + 0.5f - size / 2.0f) / (size / 2.0f);
            const float core = std::clamp((0.55f - r) / 0.08f, 0.0f, 1.0f);
            const float rim = std::clamp((0.78f - r) / 0.08f, 0.0f, 1.0f) * 0.55f;
            const float alpha = std::max(core * 0.95f, rim);
            const float lit = core * 0.95f; // the pointer's colour (premultiplied), its rim dark
            uchar *p = reinterpret_cast<uchar *>(px.data()) + (y * size + x) * 4;
            p[0] = uchar(std::lround(lit * colour.redF() * 255.0f));
            p[1] = uchar(std::lround(lit * colour.greenF() * 255.0f));
            p[2] = uchar(std::lround(lit * colour.blueF() * 255.0f));
            p[3] = uchar(std::lround(alpha * 255.0f));
        }
    }
    return px;
}

//! The beam: in its colour (light blue-white unless Settings say), brightest at the controller (bottom row), fading
//! along its length but visible up to the window.
QByteArray beamPixels(int width, int height, const QColor &colour)
{
    QByteArray px(width * height * 4, '\0');
    for (int y = 0; y < height; ++y) {
        const float along = 1.0f - (y + 0.5f) / height; // 0 at the far end (top), 1 at the controller
        for (int x = 0; x < width; ++x) {
            const float across = (x + 0.5f) / width - 0.5f;
            const float alpha = (0.3f + 0.5f * std::pow(along, 1.2f)) * std::exp(-(across * across) / (2.0f * 0.18f * 0.18f));
            uchar *p = reinterpret_cast<uchar *>(px.data()) + (y * width + x) * 4;
            p[0] = uchar(std::lround(alpha * colour.redF() * 255.0f));
            p[1] = uchar(std::lround(alpha * colour.greenF() * 255.0f));
            p[2] = uchar(std::lround(alpha * colour.blueF() * 255.0f));
            p[3] = uchar(std::lround(alpha * 255.0f));
        }
    }
    return px;
}

constexpr uint64_t kCursorLayer = ~0ull - 1;
constexpr uint64_t kBeamLayer = ~0ull - 2;
constexpr uint64_t kEnvironmentLayer = ~0ull - 3;
constexpr uint64_t kKeyboardLayer = ~0ull - 4;
constexpr uint64_t kKeyHoverLayer = ~0ull - 5;
constexpr uint64_t kKeyPressLayer = ~0ull - 6;
constexpr uint64_t kLauncherLayer = ~0ull - 7;
constexpr uint64_t kKeyboardBarLayer = ~0ull - 8;
constexpr uint64_t kDockLayer = ~0ull - 9;
constexpr uint64_t kHomePlateLayer = ~0ull - 10;
constexpr uint64_t kFocusDotLayer = ~0ull - 11;
constexpr uint64_t kDockBarLayer = ~0ull - 12;
constexpr uint64_t kNoteCardLayer = ~0ull - 13; //!< the newest notification's card
constexpr uint64_t kNoteListLayer = ~0ull - 14; //!< the dock's list of notifications
constexpr uint64_t kDragLayer = ~0ull - 15; //!< what a drag and drop carries, beside the cursor
constexpr size_t kMaxBlurredLayers = 4; //!< the runtime's backdrop blur, per view
constexpr uint64_t kHintLayer = ~0ull - 16; //!< a short message in front of the user (game mode's way out)
constexpr uint64_t kLensTestLayer = ~0ull - 17; //!< the lens test's picture (Settings), shown to each eye on its axis
constexpr float kLensTestDistance = 1.0f; //!< metres: the lens test's picture from each eye (its size is its angle)
constexpr auto kLensTestLongest = std::chrono::minutes(2); //!< the lens test ends by itself (controllers off: no button)
constexpr float kNoteCardDistance = 0.75f; //!< metres: a new notification's card, from the eyes
constexpr float kNoteCardBelowGaze = 0.21f; //!< radians (12 degrees) below where the user looks
constexpr float kDockBarWidth = 0.16f; //!< metres: the dock's grab bar, under it
constexpr float kDockBarGap = 0.02f;
// Where the dock may be put (it should always open near the user; moved far by mistake it must not become useless):
// its distance, height and angle from the gaze stay within these.
constexpr float kDockMinDistance = 0.45f;
constexpr float kDockMaxDistance = 1.1f;
constexpr float kDockLowest = -0.75f; //!< metres below the eyes
constexpr float kDockTiltFromDegrees = 40.0f; //!< gaze above the horizon from which the dock opens below the gaze tilted with it (lying back)
constexpr float kDockHighest = 0.05f;
constexpr float kDockMaxAzimuth = 0.7f; //!< radians (40 degrees) either side of the gaze
constexpr float kKeyboardBarWidth = 0.18f; //!< metres: the keyboard's grab bar, under it
constexpr float kKeyboardBarGap = 0.022f;
//! The window frame's layers: kChromeBase - windowId * 16 - part.
constexpr uint64_t kChromeBase = ~0ull - 1024;
enum ChromePart {
    kMenuPart,
    kLabelPart,
    kTiltPart,
    kGrabPart,
    kScalePart,
    kResetPart,
    kCurvePart, //!< the curvature slider's track
    kKnobPart, //!< its knob
    kFlatPart, //!< the dot at its right end
    kResizePart, //!< the resize button, top-right
    kChromeParts,
};

uint64_t chromeLayer(int windowId, int part)
{
    return kChromeBase - uint64_t(windowId) * 16 - uint64_t(part);
}

/*!
 * A photo made into a room around the user (the photo on a curved screen felt like a distant background, not
 * encompassing): an equirectangular panorama, 360 x 180 degrees, with the photo spanning
 * @p spanDegrees in front (its middle a little below the horizon, edges feathered) and the rest the same room as a soft,
 * dimmed glow (the photo and its mirror image, blurred), so nothing is black and there is no edge to see.
 */
QImage panoramaFromPhoto(const QImage &photo, float spanDegrees, int width)
{
    const int height = width / 2;
    QImage pano(width, height, QImage::Format_RGBA8888_Premultiplied);
    QPainter p(&pano);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    // The glow: photo and mirror side by side (no seam behind), blurred by shrinking and growing, dimmed.
    QImage strip(photo.width() * 2, photo.height(), QImage::Format_RGBA8888_Premultiplied);
    {
        QPainter s(&strip);
        s.drawImage(0, 0, photo);
        s.drawImage(photo.width(), 0, photo.flipped(Qt::Horizontal));
    }
    // Grown back in steps: one bilinear step from so few pixels shows blocks.
    QImage glow = strip.scaled(48, 24, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    for (int w = 192; w < width; w *= 4) {
        glow = glow.scaled(w, w / 2, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    glow = glow.scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    // The photo's half centred in front (the panorama's middle), its mirror behind, wrapping around.
    p.drawImage(QRect(width / 4, 0, width, height), glow);
    p.drawImage(QRect(width / 4 - width, 0, width, height), glow);
    p.fillRect(pano.rect(), QColor(0, 0, 0, 120));
    // The photo in front, as many degrees tall as its shape gives (equirectangular pixels are square in degrees).
    const int w = int(width * spanDegrees / 360.0f);
    const int h = w * photo.height() / photo.width();
    QImage front = photo.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    {
        // Feathered edges: alpha ramps over 9 % of each side.
        QPainter f(&front);
        f.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        QLinearGradient across(0, 0, w, 0);
        across.setColorAt(0.0, QColor(0, 0, 0, 0));
        across.setColorAt(0.09, QColor(0, 0, 0, 255));
        across.setColorAt(0.91, QColor(0, 0, 0, 255));
        across.setColorAt(1.0, QColor(0, 0, 0, 0));
        f.fillRect(front.rect(), across);
        QLinearGradient down(0, 0, 0, h);
        down.setColorAt(0.0, QColor(0, 0, 0, 0));
        down.setColorAt(0.09, QColor(0, 0, 0, 255));
        down.setColorAt(0.91, QColor(0, 0, 0, 255));
        down.setColorAt(1.0, QColor(0, 0, 0, 0));
        f.fillRect(front.rect(), down);
    }
    const int below = int(width / 360.0f * 4.0f); // its middle 4 degrees below the horizon
    p.drawImage((width - w) / 2, (height - h) / 2 + below, front);
    p.end();
    return pano;
}

//! Pixels Rift Home draws itself (premultiplied RGBA8888, sRGB values) as a buffer for a layer.
std::unique_ptr<BufferHandoff> rgbaBuffer(QByteArray pixels, int width, int height, int stride)
{
    auto buffer = std::make_unique<BufferHandoff>();
    buffer->kind = BufferHandoff::Kind::Shm;
    buffer->drmFormat = DRM_FORMAT_ABGR8888;
    buffer->width = width;
    buffer->height = height;
    buffer->stride = stride;
    buffer->pixels = std::move(pixels);
    // Drawn with sRGB's own curve (Qt's): shown exactly so, not as an untagged app's window (gamma 2.2).
    Color::SurfaceColorDescription srgb;
    srgb.tagged = true;
    srgb.transferNamed = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4;
    srgb.primariesNamed = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB;
    buffer->plan = Color::planLayer(srgb, Color::PixelKind::Unorm8);
    buffer->alpha = true;
    return buffer;
}

//! Rift Home's OpenXR application name, as the runtime's app list (the Rift panel, libmonado) shows it;
//! RIFT_HOME_XR_APP_NAME: another (a second Rift Home as "another VR app" in checks).
std::string appName()
{
    const QByteArray chosen = qgetenv("RIFT_HOME_XR_APP_NAME");
    return chosen.isEmpty() ? std::string("Rift Home VR") : chosen.toStdString();
}

std::unique_ptr<BufferHandoff> imageBuffer(const QImage &image, bool opaque = false)
{
    auto buffer = rgbaBuffer(QByteArray(reinterpret_cast<const char *>(image.constBits()), image.sizeInBytes()), image.width(),
                             image.height(), int(image.bytesPerLine()));
    if (opaque) {
        buffer->drmFormat = DRM_FORMAT_XBGR8888; // the same bytes, alpha ignored: an opaque layer
        buffer->alpha = false;
    }
    return buffer;
}

const char *controlName(Spatial::FrameControl control)
{
    switch (control) {
    case Spatial::FrameControl::Follow:
        return "follow";
    case Spatial::FrameControl::Main:
        return "make main";
    case Spatial::FrameControl::Pin:
        return "pin";
    case Spatial::FrameControl::Size:
        return "size";
    case Spatial::FrameControl::Close:
        return "close";
    case Spatial::FrameControl::Tilt:
        return "tilt";
    case Spatial::FrameControl::ResetTilt:
        return "reset tilt";
    case Spatial::FrameControl::Curve:
        return "curve";
    case Spatial::FrameControl::ResetCurve:
        return "flat";
    case Spatial::FrameControl::Minimize:
        return "minimize";
    case Spatial::FrameControl::Resize:
        return "resize";
    case Spatial::FrameControl::Keyboard:
        return "keyboard";
    case Spatial::FrameControl::Cinema:
        return "cinema";
    case Spatial::FrameControl::Grab:
        return "grab bar";
    case Spatial::FrameControl::Scale:
        return "scale";
    case Spatial::FrameControl::None:
        break;
    }
    return "none";
}

int menuIndex(Spatial::FrameControl control)
{
    for (int i = 0; i < Spatial::kMenuButtons; ++i) {
        if (Spatial::kMenuOrder[i] == control) {
            return i;
        }
    }
    return -1;
}
constexpr int kCursorSize = 64;
constexpr int kBeamWidth = 16;
constexpr int kBeamHeight = 256;

struct SurfaceLayer {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkFormat storage = VK_FORMAT_UNDEFINED; //!< the pass image's format
    int width = 0;
    int height = 0;
    std::vector<VkImage> images;
    bool hasImage = false;
    Color::LayerHdrDescription hdr;
    bool alpha = false;
    // Shared-memory buffers go through this host-visible buffer into a device image the pass samples.
    VkImage sourceImage = VK_NULL_HANDLE;
    VkDeviceMemory sourceMemory = VK_NULL_HANDLE;
    VkFormat sourceFormat = VK_FORMAT_UNDEFINED;
    int sourceWidth = 0;
    int sourceHeight = 0;
    // The window pass writes here (storage), then it is copied into the swapchain image. Same size as the swapchain.
    VkImage passImage = VK_NULL_HANDLE;
    VkDeviceMemory passMemory = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkDeviceSize stagingSize = 0;
    void *stagingMapped = nullptr;
    // Sharp at a distance: the pass image keeps the full-size picture; a layer shown smaller than its
    // pixels shows an area-filtered copy (made in scaledImage) in the swapchain image's top-left corner instead.
    int level = 0; //!< Spatial::Sharpness level shown (0: full size)
    float wantLevel = 0.0f; //!< the level wanted where it was shown last frame (the nearest of its places)
    int shownWidth = 0; //!< the part of the swapchain image the layer shows
    int shownHeight = 0;
    int swapWidth = 0; //!< the swapchain's size: the buffer's, twice it for the enlarged copy (Sharpness::kUpLevel)
    int swapHeight = 0;
    VkImage scaledImage = VK_NULL_HANDLE; //!< the smaller or enlarged copy
    VkDeviceMemory scaledMemory = VK_NULL_HANDLE;
    int scaledWidth = 0;
    int scaledHeight = 0;
};

//! A layer of the frame as sent, kept for a screenshot: where it is, and which of Rift Home's images it shows.
struct ShotLayer {
    const SurfaceLayer *layer = nullptr;
    Xr::ViewShot::Kind kind = Xr::ViewShot::Kind::Quad;
    Spatial::Pose pose; //!< as sent (the recline applied), in the room or, head locked, in the head's space
    bool headLocked = false;
    float width = 0.0f; //!< quad size, metres
    float height = 0.0f;
    float radius = 0.0f; //!< cylinder and equirect
    float centralAngle = 0.0f;
    float aspectRatio = 1.0f;
    float upperAngle = 0.0f; //!< equirect
    float lowerAngle = 0.0f;
    float scale[4] = {1.0f, 1.0f, 1.0f, 1.0f}; //!< colour scale (the fade, the room's brightness)
    float blurRadians = 0.0f; //!< frosted glass: the runtime's backdrop blur behind it (0: none)
    float blurSaturation = 1.0f; //!< and the rest of what chain() put in its XrCompositionLayerBackdropBlurMNDX
    float blurTint[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float blurMaskAlpha = 1.0f;
};

/*!
 * A client dmabuf imported once and kept while the client keeps the buffer, as wlroots and KWin keep theirs: importing
 * each 4K video frame anew (and freeing it) cost 1-13 ms a frame. Dropped when the client destroys the
 * buffer, and when unused for a while: a client may keep buffers it never shows again (Firefox keeps every video frame
 * buffer it made), and an import made again later costs only that import.
 */
struct ImportedBuffer {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImage chroma = VK_NULL_HANDLE; //!< P010's second plane
    VkDeviceMemory chromaMemory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED; //!< the first image's
    std::chrono::steady_clock::time_point used;
};

/*!
 * What a client dmabuf's import is kept under: the memory it shows (each plane's dma-buf, by the kernel's identity of
 * it, and where in it), not the client's buffer object. Firefox makes new buffers for the same decoded frames at every
 * seek: imported once per frame memory, a seek costs no import, and NVIDIA's driver, which kept a little
 * more for each import, keeps nothing more. Safe to reuse: while an import holds a dma-buf, its identity is not reused.
 * Empty: not keyed (a plane that cannot be looked at).
 */
std::string importKey(const BufferHandoff &buffer, VkFormat first, bool yuv)
{
    std::string key = std::to_string(buffer.drmFormat) + ':' + std::to_string(buffer.width) + 'x' + std::to_string(buffer.height) + ':'
        + std::to_string(int(first)) + (yuv ? "y" : "");
    for (const DmabufPlane &plane : buffer.planes) {
        struct stat st{};
        if (plane.fd < 0 || ::fstat(plane.fd, &st) != 0) {
            return {};
        }
        key += '|' + std::to_string(st.st_dev) + '.' + std::to_string(st.st_ino) + '@' + std::to_string(plane.offset) + '/'
            + std::to_string(plane.stride) + '/' + std::to_string(plane.modifier);
    }
    return key;
}

/*!
 * New client buffers imported on a thread of their own, asked for as the app attaches them (the Wayland thread asks,
 * through LayerBridge): a first import of a video frame took 5-17 ms on NVIDIA (once 163), too long for a frame of
 * 11. The frame thread collects what is done into its importCache (by importKey); a picture whose memory
 * is not imported yet waits a frame. Bounded: at most @p kept results wait to be collected (older ones are freed).
 */
class Importer
{
public:
    //! The import itself (Impl's, thread-safe: Vulkan objects of their own) and its undoing.
    using ImportFn = std::function<std::optional<ImportedBuffer>(const BufferHandoff &buffer, VkFormat first, bool yuv)>;
    using DropFn = std::function<void(const ImportedBuffer &imported)>;
    using Result = std::pair<std::string, std::optional<ImportedBuffer>>; //!< by importKey; nullopt: it failed

    Importer(std::set<int64_t> swapchainFormats, ImportFn import, DropFn drop, size_t kept)
        : m_formats(std::move(swapchainFormats))
        , m_import(std::move(import))
        , m_drop(std::move(drop))
        , m_kept(kept)
    {
        m_thread = std::thread([this] {
            work();
        });
    }
    ~Importer()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
        }
        m_wake.notify_all();
        m_thread.join();
        for (const std::function<void()> &free : m_frees) {
            free(); // every free asked for runs, before the device goes
        }
        for (const auto &[key, result] : m_done) {
            if (result) {
                m_drop(*result);
            }
        }
    }

    //! Any thread: @p free (Vulkan objects nothing uses any more) run on the worker thread, in order; at once if stopping.
    void post(std::function<void()> free)
    {
        {
            std::lock_guard lock(m_mutex);
            if (!m_stop) {
                m_frees.push_back(std::move(free));
                m_wake.notify_one();
                return;
            }
        }
        free();
    }
    Importer(const Importer &) = delete;
    Importer &operator=(const Importer &) = delete;

    //! Any thread: @p buffer's memory (a dmabuf) imported, unless the frame thread holds it already (held()), or it was
    //! asked for and not collected yet.
    void request(const BufferHandoff &buffer)
    {
        if (buffer.kind != BufferHandoff::Kind::Dmabuf || buffer.planes.empty()) {
            return;
        }
        const std::optional<Target> target = chooseTarget(buffer.plan, buffer.drmFormat, m_formats);
        if (!target) {
            return;
        }
        const bool yuv = (target->flags & kYuvBt2020) != 0 && buffer.planes.size() >= 2;
        const VkFormat first = yuv ? VK_FORMAT_R16_UNORM : target->source;
        std::string key = importKey(buffer, first, yuv);
        if (key.empty()) {
            return;
        }
        {
            std::lock_guard lock(m_mutex);
            if (m_stop || m_asked.contains(key) || m_held.contains(key)) {
                return;
            }
        }
        // Its own descriptors: the client's buffer may be released (and its handoff closed) before the import runs.
        auto copy = std::make_unique<BufferHandoff>();
        copy->kind = BufferHandoff::Kind::Dmabuf;
        copy->drmFormat = buffer.drmFormat;
        copy->modifier = buffer.modifier;
        copy->width = buffer.width;
        copy->height = buffer.height;
        copy->bufferId = buffer.bufferId;
        for (const DmabufPlane &plane : buffer.planes) {
            DmabufPlane own = plane;
            own.fd = fcntl(plane.fd, F_DUPFD_CLOEXEC, 0);
            if (own.fd < 0) {
                return; // (copy closes what it has)
            }
            copy->planes.push_back(own);
        }
        {
            std::lock_guard lock(m_mutex);
            if (m_stop || m_held.contains(key) || !m_asked.insert(key).second) {
                return;
            }
            m_jobs.push_back({std::move(copy), first, yuv, std::move(key)});
        }
        m_wake.notify_one();
    }

    //! Frame thread: the imports done since the last call. Their memory may be asked for again.
    std::vector<Result> collect()
    {
        std::lock_guard lock(m_mutex);
        std::vector<Result> done;
        done.swap(m_done);
        for (const auto &[key, result] : done) {
            m_asked.erase(key);
            if (result) {
                m_held.insert(key); // the frame thread keeps it (importCache) until it says otherwise (released())
            }
        }
        return done;
    }

    //! Frame thread: it holds @p key's import (made there), or has let it go: asked for again after that only.
    void held(const std::string &key)
    {
        std::lock_guard lock(m_mutex);
        m_held.insert(key);
    }
    void released(const std::string &key)
    {
        std::lock_guard lock(m_mutex);
        m_held.erase(key);
    }

    //! Stats since the last call: imports done, their total and longest time in ms.
    std::tuple<int, double, double> takeStats()
    {
        std::lock_guard lock(m_mutex);
        return {std::exchange(m_count, 0), std::exchange(m_totalMs, 0.0), std::exchange(m_mostMs, 0.0)};
    }

private:
    struct Job {
        std::unique_ptr<BufferHandoff> buffer;
        VkFormat first = VK_FORMAT_UNDEFINED;
        bool yuv = false;
        std::string key;
    };

    void work()
    {
        std::unique_lock lock(m_mutex);
        while (true) {
            m_wake.wait(lock, [this] {
                return m_stop || !m_jobs.empty() || !m_frees.empty();
            });
            if (m_stop) {
                return; // queued jobs' descriptors close with them; frees run in the destructor
            }
            if (!m_frees.empty()) {
                std::vector<std::function<void()>> frees;
                frees.swap(m_frees);
                lock.unlock();
                for (const std::function<void()> &free : frees) {
                    free();
                }
                lock.lock();
                continue;
            }
            Job job = std::move(m_jobs.front());
            m_jobs.pop_front();
            lock.unlock();
            const auto start = std::chrono::steady_clock::now();
            std::optional<ImportedBuffer> result = m_import(*job.buffer, job.first, job.yuv);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            job.buffer.reset(); // its descriptors closed (Vulkan has its own)
            lock.lock();
            ++m_count;
            m_totalMs += ms;
            m_mostMs = std::max(m_mostMs, ms);
            m_done.emplace_back(std::move(job.key), std::move(result));
            // Not collected (the frame thread is not taking frames): the oldest results go, so this stays bounded.
            while (m_done.size() > m_kept) {
                if (m_done.front().second) {
                    m_drop(*m_done.front().second);
                }
                m_asked.erase(m_done.front().first);
                m_done.erase(m_done.begin());
            }
        }
    }

    const std::set<int64_t> m_formats;
    const ImportFn m_import;
    const DropFn m_drop;
    const size_t m_kept;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Job> m_jobs;
    std::vector<std::function<void()>> m_frees;
    std::unordered_set<std::string> m_asked; //!< queued, importing, or done and not collected
    std::unordered_set<std::string> m_held; //!< imported and kept by the frame thread (importCache)
    std::vector<Result> m_done;
    int m_count = 0;
    double m_totalMs = 0.0;
    double m_mostMs = 0.0;
    bool m_stop = false;
    std::thread m_thread; //!< last: started once everything above exists
};

/*!
 * Writes a file on a thread of its own, the newest content winning: QSaveFile waits for the disk (fsync), which took 91
 * ms on a btrfs disk in a frame (a window closing; and saves come once a second while a window is moved).
 * Destroyed: what waits is written before it goes.
 */
class FileWriter
{
public:
    FileWriter()
    {
        m_thread = std::thread([this] {
            work();
        });
    }
    ~FileWriter()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
        }
        m_wake.notify_all();
        m_thread.join();
        if (m_pending) {
            write(m_pending->first, m_pending->second);
        }
    }
    FileWriter(const FileWriter &) = delete;
    FileWriter &operator=(const FileWriter &) = delete;

    void post(const QString &path, QByteArray bytes)
    {
        {
            std::lock_guard lock(m_mutex);
            m_pending = std::make_pair(path, std::move(bytes));
        }
        m_wake.notify_one();
    }

    static void write(const QString &path, const QByteArray &bytes)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QSaveFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(bytes);
            file.commit();
        }
    }

private:
    void work()
    {
        std::unique_lock lock(m_mutex);
        while (true) {
            m_wake.wait(lock, [this] {
                return m_stop || m_pending.has_value();
            });
            if (m_stop) {
                return; // the destructor writes what waits
            }
            const std::pair<QString, QByteArray> job = std::move(*m_pending);
            m_pending.reset();
            lock.unlock();
            write(job.first, job.second);
            lock.lock();
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::optional<std::pair<QString, QByteArray>> m_pending;
    bool m_stop = false;
    std::thread m_thread; //!< last: started once the rest exists
};

//! The part of a layer's swapchain image it shows (the whole of it until its first copy says otherwise).
XrRect2Di shownRect(const SurfaceLayer &layer)
{
    return {{0, 0}, {layer.shownWidth > 0 ? layer.shownWidth : layer.swapWidth, layer.shownHeight > 0 ? layer.shownHeight : layer.swapHeight}};
}

} // namespace

/*!
 * The room's image: a photo or panorama (8-bit sRGB), or a Radiance HDR panorama (16-bit float, linear light: real
 * light, its windows and lamps brighter than white).
 */
struct RoomImage {
    QImage sdr;
    QByteArray halves; //!< RGBA 16-bit floats (ABGR16161616F)
    float peak = 0.0f; //!< an HDR room's peak relative to white (roomPeak); 0 for a picture in SDR
    int width = 0;
    int height = 0;
    bool isNull() const
    {
        return sdr.isNull() && halves.isEmpty();
    }
};

namespace
{
std::unique_ptr<BufferHandoff> roomBuffer(const RoomImage &room); // below, with the room's loading
float roomPeak(const QByteArray &halves); // below
//! A panorama room's width: about 14 pixels a degree, just above the CV1's sharpest (about 13 at the lens centre), so
//! the room is not magnified; wider would shimmer, as the runtime samples a layer without mip levels.
constexpr int kRoomPanoramaWidth = 5120;
} // namespace

struct XrCompositor::Impl {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;
    bool running = false;
    bool hdrExtension = false;
    bool blurLayers = false; //!< XR_MNDX_composition_layer_backdrop_blur (RiftLinux's frosted glass)
    bool cylinderLayers = false; //!< XR_KHR_composition_layer_cylinder (photo environments)
    bool equirectLayers = false; //!< XR_KHR_composition_layer_equirect2 (panorama environments)
    bool colorScaleLayers = false; //!< XR_KHR_composition_layer_color_scale_bias (fading windows, dimming the room)
    bool environmentPanorama = false;
    RoomImage environmentImage; //!< loaded at start, uploaded with the first frame
    uint32_t maxLayers = 16;

    VkInstance vkInstance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    bool dmabufImport = false;
    bool queueFamilyForeign = false;
    PFN_vkGetMemoryFdPropertiesKHR getMemoryFdProperties = nullptr;
    // The window pass (window.comp): rounded corners, opaque formats, SDR decoding.
    VkSampler sampler = VK_NULL_HANDLE;
    VkSampler chromaSampler = VK_NULL_HANDLE; //!< bilinear: P010's half-size chroma, interpolated at its sample positions
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipelineRgba8 = VK_NULL_HANDLE;
    VkPipeline pipelineRgba16f = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    std::vector<VkImageView> frameViews; //!< destroyed once the frame's copies are done
    // Sharp windows near and far: the resample pass (resample.comp) and the density it aims at.
    VkDescriptorSetLayout downsampleSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout downsamplePipelineLayout = VK_NULL_HANDLE;
    VkPipeline downsampleRgba8 = VK_NULL_HANDLE;
    VkPipeline downsampleRgba16f = VK_NULL_HANDLE;
    uint32_t eyeWidth = 0; //!< the runtime's recommended view width: its 1:1 size at the lens centre
    float sharpDensity = 0.0f; //!< pixels a radian a window may show before it gets a filtered copy; 0: not yet/off
    bool sharpOff = false; //!< RIFT_HOME_SHARP=0 (before/after checks)
    float sharpFactor = Spatial::Sharpness::kDensityFactor; //!< RIFT_HOME_SHARP_FACTOR

    std::set<int64_t> swapchainFormats;
    std::map<uint64_t, SurfaceLayer> layers;
    std::set<uint64_t> warned;
    std::unordered_map<std::string, ImportedBuffer> importCache; //!< by importKey (the memory, not the client's buffer)
    static constexpr double kImportBudgetMs = 2.0; //!< without the Importer: new imports a frame, past this the rest wait
    std::shared_ptr<Importer> importer; //!< made with the device (init), gone before it (shutdown)
    //! Buffers whose import failed, and when: not shown, asked again after kImportRetry (a passing shortage).
    std::unordered_map<std::string, Clock::time_point> failedImports; //!< by importKey
    static constexpr size_t kFailedImportsKept = 256;
    static constexpr std::chrono::seconds kImportRetry{2};
    /*!
     * Swapchains of layers just gone (or resized), kept a moment for new layers: Firefox makes a new video surface at
     * every seek, new video and fullscreen switch, and destroying a swapchain and making another cost 5-10 ms each
     * (late frames). And RiftLinux's client library keeps memory for each swapchain made (its command pool,
     * never trimmed: about 100 KB each): windows opening
     * and closing made a dozen each time. At most kSpareSwapchains or kSpareMegabytes; small ones (a window frame's art)
     * kept kSmallSpareKept, big ones (video) kSpareKept; destroyed one a frame.
     */
    struct SpareSwapchain {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        std::vector<VkImage> images;
        VkFormat format = VK_FORMAT_UNDEFINED;
        int width = 0;
        int height = 0;
        Clock::time_point since;
        double megabytes() const
        {
            return (format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8.0 : 4.0) * width * height * double(images.size()) / 1e6;
        }
        std::chrono::seconds kept() const
        {
            return megabytes() < kSmallSpareMegabytes ? kSmallSpareKept : kSpareKept;
        }
    };
    std::vector<SpareSwapchain> spareSwapchains; //!< oldest first
    static constexpr size_t kSpareSwapchains = 24;
    static constexpr double kSpareMegabytes = 768.0;
    static constexpr std::chrono::seconds kSpareKept{60};
    static constexpr double kSmallSpareMegabytes = 32.0;
    static constexpr std::chrono::seconds kSmallSpareKept{600};
    int swapchainsReused = 0; //!< in the stats period
    //! @p layer's swapchain to the spares (none left on the layer).
    void spareSwapchain(SurfaceLayer &layer);
    //! At a frame's start: one spare past its time or the limits destroyed (each costs the runtime a few ms).
    void pruneSpareSwapchains(bool all);
    //! How long an unused import is kept (RIFT_HOME_IMPORT_KEEP_MS: a test's setting).
    std::chrono::milliseconds importKeptUnused{qEnvironmentVariableIsSet("RIFT_HOME_IMPORT_KEEP_MS")
                                                         ? qEnvironmentVariableIntValue("RIFT_HOME_IMPORT_KEEP_MS")
                                                         : 3000};
    static constexpr size_t kImportsKept = 128; //!< windows' buffers (2-4 each) and videos' frame pools (Firefox: about 11)

    // Windows in the room and the controllers.
    Options options;
    Spatial::SpatialLayout layout;
    XrInput input;
    bool inputReady = false;
    bool focused = false;
    std::optional<Clock::time_point> firstFocusAt; //!< new windows open in front of the user from a while after this
    XrTime lastDisplayTime = 0;
    //! The head in the arrangement's space (what the layout, the dock and Home work with): roomHead through the recline.
    Spatial::Pose head;
    Spatial::Pose roomHead; //!< the head in the room (LOCAL)
    // Recline (Spatial/Recline.hpp): the arrangement turned to where the user looks. recline maps the layout into the
    // room; what is shown goes out through it, the head and the rays come in through its inverse.
    Spatial::Pose recline;
    Spatial::Pose reclineFrom; //!< where the turn started
    Spatial::Pose reclineTarget;
    float reclineProgress = 1.0f; //!< of the turn, 0..1 over Spatial::kReclineSeconds
    bool reclined = false;
    struct HandState {
        bool trigger = false;
        bool squeeze = false;
        bool primary = false;
        bool stickClick = false;
        bool scrolling = false;
        bool rightClick = false; //!< the right button is down (a button given it: by default A, the thumbstick presses)
        bool middleClick = false; //!< the middle button is down (a button given it)
        Clock::time_point lastDeflect; //!< the thumbstick was last pushed (scrolling): its click is no right click then
        int hoverWindow = 0;
        // Where the ray was when the finger started pulling the trigger: pulling tips the controller down a little
        // before the click registers (YouTube's fullscreen button was pressed as its neighbour).
        float lastTrigger = 0.0f;
        struct Armed {
            int windowId;
            double x;
            double y;
            Spatial::Vec3 direction;
            Clock::time_point at;
        };
        std::optional<Armed> armed;
    };
    HandState handState[2];
    int activeHand = 1;
    int grabHand = -1;
    int pointerWindow = 0; //!< the window that has the Wayland pointer
    int pressedWindow = 0; //!< the window a click started on: it keeps the pointer until release
    double lastX = -1.0;
    double lastY = -1.0;
    XrInput::Hand hands[2];
    //! The controllers in the room (Rift Home's own models): a projection layer of Rift Home's own, one
    //! swapchain an eye, 16-bit float with an HDR description (linear light: highlights past white).
    struct ControllerLayer {
        XrSwapchain swapchains[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
        std::array<std::vector<VkImage>, 2> images;
        uint32_t width = 0;
        uint32_t height = 0;
        bool hdr = false;
        ControllerRenderer renderer;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool submitted = false; //!< its fence is waited for before the next frame records again (never in this frame)
    };
    std::shared_ptr<ControllerLayer> controllerLayer;
    QString controllerState; //!< logged when it changes: how many are drawn, or why none
    bool testControllers = false; //!< RIFT_HOME_TEST_CONTROLLERS=1: both shown in front of the view (desk checks)
    void createControllerLayer();
    void destroyControllerLayer();
    std::optional<Spatial::Hit> hits[2];
    Spatial::RayFilter rayFilters[2]; //!< hand tremor out of the rays
    Spatial::ClickLock clickLock; //!< a click stays where it was pressed
    //! The last press, for double clicks: a second press soon after and close by goes where the first went (up to
    //! 50 px apart: Dolphin saw two clicks on a folder and renamed it instead of opening it).
    struct LastPress {
        int windowId = 0;
        double x = 0.0;
        double y = 0.0;
        Spatial::Vec3 direction;
        Clock::time_point at;
    };
    std::optional<LastPress> lastPress;
    /*!
     * Apps tell a double click by the events' times (Qt and GTK: 400 ms unless set otherwise; trigger pulls can come
     * 500-850 ms apart, and with the user's own app settings Rift Home's 800 ms do not apply): a snapped second
     * press is stamped at most 250 ms after the first, and every later event as much earlier (pointer times only ever
     * lag).
     */
    uint32_t clickTimeShiftMs = 0;
    uint32_t lastPressMs = 0; //!< the last press's time as sent (shifted)
    uint32_t lastReleaseMs = 0;
    bool grabByTrigger = false; //!< the grab started on the grab bar (ends with the trigger, not the grip)
    // The window frame: per window, its controls this frame, whether it shows, what its art shows.
    struct Chrome {
        Spatial::FrameLayout frame;
        Spatial::Pose surface; //!< the window's plane
        Clock::time_point lastNear{};
        bool visible = false;
        Spatial::FrameControl hovered = Spatial::FrameControl::None;
        std::optional<FrameArt::Menu> menuArt;
        QString labelArt;
        std::optional<std::pair<int, int>> tiltArt, grabArt, scaleArt, resetArt, curveArt, knobArt, flatArt, resizeArt; //!< (quantised size, state)
    };
    std::map<int, Chrome> chrome;
    struct ChromeHit {
        int windowId = 0;
        Spatial::FrameControl control = Spatial::FrameControl::None;
        float distance = 0.0f;
        Spatial::Vec3 point;
        float localX = 0.0f; //!< window-local metres along its surface (Spatial::Hit's)
        float localY = 0.0f;
    };
    std::optional<ChromeHit> chromeHits[2]; //!< the ray on a control (or on the plane of the one it drags)
    std::optional<ChromeHit> chromeUnder[2]; //!< the control really under the ray (a menu button acts on release there)
    struct ChromeDrag {
        int hand;
        int windowId;
        Spatial::FrameControl control;
    };
    std::optional<ChromeDrag> chromeDrag;
    //! The resize handle held: the size last asked of the app, and when.
    struct ResizeDrag {
        int width = 0; //!< the size last asked
        int height = 0;
        Clock::time_point sent{};
        int startWidth = 0; //!< the window's size when the handle was taken
        int startHeight = 0;
        float startX = 0.0f; //!< where the ray was on it then (window-local metres)
        float startY = 0.0f;
        bool moved = false; //!< asked anything yet (grabbing alone changes nothing)
        int pendingWidth = 0; //!< the latest size the ray asks for (sent at most every 120 ms; on release if newer)
        int pendingHeight = 0;
    };
    std::optional<ResizeDrag> resizeDrag;
    /*!
     * The size the resize handle asks for: the window's size when taken, grown by how far the ray has moved since, twice
     * (it grows about its centre). Session m3-desk-1: from the ray's place alone the first request jumped (the handle
     * sits outside the window's corner).
     */
    std::pair<int, int> resizeTarget(int windowId, const ChromeHit &onPlane) const
    {
        const auto placed = layout.windows().find(windowId);
        if (!resizeDrag || placed == layout.windows().end() || placed->second.scale <= 0.0f) {
            return {0, 0};
        }
        const float perMetre = layout.pixelsPerMeter() / placed->second.scale;
        const float dx = std::fabs(onPlane.localX) - std::fabs(resizeDrag->startX);
        const float dy = std::fabs(onPlane.localY) - std::fabs(resizeDrag->startY);
        // Within a quarter and four times the size it had (a ray far off the window once asked 92757 px).
        const auto within = [](float value, int start) {
            return int(std::lround(std::clamp(value, std::max(320.0f, start * 0.25f), std::min(3840.0f, start * 4.0f))));
        };
        return {within(resizeDrag->startWidth + 2.0f * dx * perMetre, resizeDrag->startWidth),
                within(resizeDrag->startHeight + 2.0f * dy * perMetre, resizeDrag->startHeight)};
    }
    Spatial::FrameControl hapticControl[2] = {Spatial::FrameControl::None, Spatial::FrameControl::None};
    std::map<int, std::pair<bool, bool>> sizeStates; //!< window: (fullscreen, maximized)
    std::map<uint64_t, std::unique_ptr<BufferHandoff>> pendingArt; //!< uploaded at the next frame's copies
    /*!
     * Home's art painted away from the frame (Home's grid, the keyboard or a menu painted in the frame
     * took up to 49 ms, and frames dropped): FrameArt runs on one worker only, with copies of what it draws; done art is
     * uploaded from the next frame on, and a newer request for a layer wins over an older one still being painted.
     */
    struct ArtPainter {
        ArtPainter()
        {
            pool.setMaxThreadCount(1);
        }
        std::mutex mutex;
        std::map<uint64_t, uint64_t> latest; //!< layer: its newest request still being painted (none once done)
        std::map<uint64_t, QImage> done;
        uint64_t requests = 0;
        QThreadPool pool; //!< last: destroyed first, waiting for the work still running
    };
    std::unique_ptr<ArtPainter> artPainter = std::make_unique<ArtPainter>();
    //! Screenshots are drawn here, off the frame thread; shutdown waits for one still being drawn (it maps the read-backs'
    //! memory and hands their freeing back to this compositor).
    std::unique_ptr<QThreadPool> screenshotPool = std::make_unique<QThreadPool>();
    //! A screenshot just written, for the frame loop to say so (set on the screenshot pool).
    struct SavedShot {
        std::mutex mutex;
        QString path;
    };
    std::unique_ptr<SavedShot> savedShot = std::make_unique<SavedShot>();
    //! A screenshot written (any thread): the frame loop shows "Screenshot saved".
    void screenshotSaved(const QString &path);
    template<typename... Params, typename... Args>
    void paintArt(uint64_t layer, QImage (*paint)(Params...), Args &&...args)
    {
        ArtPainter *painter = artPainter.get();
        uint64_t request = 0;
        {
            std::lock_guard lock(painter->mutex);
            request = ++painter->requests;
            painter->latest[layer] = request;
        }
        painter->pool.start([painter, layer, request, paint, copies = std::make_tuple(std::decay_t<Args>(std::forward<Args>(args))...)] {
            QImage image = std::apply(paint, copies);
            std::lock_guard lock(painter->mutex);
            // Only the newest request of a layer still wanted is shown (a newer one, or the layer gone, drops this).
            if (const auto newest = painter->latest.find(layer); newest != painter->latest.end() && newest->second == request) {
                painter->done[layer] = std::move(image);
                painter->latest.erase(newest);
            }
        });
    }
    //! A layer gone: its art still being painted is never shown.
    void cancelArt(uint64_t layer)
    {
        std::lock_guard lock(artPainter->mutex);
        artPainter->latest.erase(layer);
        artPainter->done.erase(layer);
    }
    //! The art painted since the last frame, to the copies.
    void takePaintedArt()
    {
        std::map<uint64_t, QImage> done;
        {
            std::lock_guard lock(artPainter->mutex);
            done.swap(artPainter->done);
        }
        for (auto &[layer, image] : done) {
            pendingArt[layer] = imageBuffer(image);
        }
    }
    std::vector<Spatial::Pose> headHistory; //!< the last half second of head poses (a recentre's jump is in there)
    int recentreFrames = -1; //!< frames until the pending recentre is worked out (-1: none pending)
    // The virtual keyboard: in the room below the gaze while open; either hand types on it.
    Spatial::VirtualKeyboard keyboard;
    bool keyboardOpen = false;
    bool keyboardAuto = false; //!< opened because a text field was focused: closes when it is left
    bool keyboardWanted = false; //!< last frame, the window last clicked had a text field focused
    //! When its text field was left: the keyboard closes a moment later unless a field is focused again (clicks
    //! in Dolphin focused and left a field several times a second, the keyboard flashed open and shut).
    std::optional<Clock::time_point> textFieldLeftAt;
    int lastClickedWindow = 0; //!< by the user: a text field focused there (not by an app starting) opens the keyboard
    Spatial::Pose keyboardPose;
    struct KeyHit {
        int key = -1; //!< -1: on the keyboard, between keys
        float distance = 0.0f;
        Spatial::Vec3 point;
        bool bar = false; //!< on its grab bar
    };
    // Moving the keyboard (its grab bar with the trigger, or the grip anywhere on it); where it opens, relative to
    // the gaze, follows where the user left it.
    struct KeyboardGrab {
        int hand;
        bool byGrip;
        float distance;
        Spatial::Vec3 offsetLocal; //!< from the grabbed point to the keyboard's centre, keyboard-local
    };
    std::optional<KeyboardGrab> keyboardGrab;
    float keyboardAzimuth = 0.0f; //!< from the gaze, radians
    float keyboardDistance = 0.62f; //!< horizontal, metres
    float keyboardHeight = -0.42f; //!< below the eyes
    std::optional<int> keyboardBarArt; //!< its state as drawn
    std::optional<KeyHit> keyHits[2];
    int keyHeld[2] = {-1, -1};
    std::optional<std::array<bool, 4>> keyboardArt; //!< shift, caps lock, ctrl, alt as drawn
    int keysTyped = 0; //!< counted, never logged one by one (passwords)
    // The launcher (left menu button): a page of apps in front of the user.
    bool launcherOpen = false;
    Spatial::Pose launcherPose;
    int launcherPage = 0;
    int launcherArtPage = -1; //!< the page its image shows (-1: none yet)
    struct CellHit {
        int cell = -1; //!< on the page; -1: on the panel, not on an app
        float distance = 0.0f;
        Spatial::Vec3 point;
        int card = -1; //!< a running app's card (index in running)
        bool close = false; //!< on that card's close button
        bool search = false; //!< on the search field
    };
    // Home's search: the virtual keyboard types into it; the apps shown are those matching.
    QString homeQuery;
    bool homeSearching = false;
    bool keyboardForSearch = false; //!< the keyboard was opened for the search (closes with it)
    //! The apps Home shows (indices in options.apps): all, or those whose name holds the query.
    std::vector<int> homeApps() const
    {
        std::vector<int> shown;
        for (size_t i = 0; i < options.apps.size(); ++i) {
            if (homeQuery.isEmpty() || options.apps[i].name.contains(homeQuery, Qt::CaseInsensitive)) {
                shown.push_back(int(i));
            }
        }
        return shown;
    }
    //! A key typed into Home's search (the keyboard's events go there instead of to a window).
    void typeIntoSearch(LayerBridge &bridge, int key, bool upper);
    //! The open windows, for Home's running apps, in the order they opened.
    struct RunningWindow {
        int windowId = 0;
        QString title;
        std::string appId;
        bool minimized = false;
        uint32_t vrApp = 0; //!< another VR app (the runtime's client id), not a window
        bool vr = false;
    };
    std::vector<RunningWindow> runningWindows;
    std::map<int, int> minimizeSeen; //!< per window: its app's minimize requests handled so far
    QString launcherArtRunning; //!< the running apps as Home's image shows them
    int cardPressed[2] = {-1, -1};
    bool cardClosePressed[2] = {false, false};
    //! An app's icon for its window (by app id), or none.
    QImage iconForApp(const std::string &appId) const
    {
        const QString id = QString::fromStdString(appId).toLower();
        for (const auto &app : options.apps) {
            const QString candidate = app.id.toLower();
            if (!id.isEmpty() && (candidate == id || candidate.endsWith(QLatin1Char('.') + id))) {
                return app.icon;
            }
        }
        return {};
    }
    std::optional<CellHit> cellHits[2];
    int homeFramesOnHome = 0; //!< while home is open: frames a ray was on it, and on an app
    int homeFramesOnApp = 0;
    int homeFrames = 0;
    int cellPressed[2] = {-1, -1};
    bool lastMenu = false; //!< the menu button last frame (its presses are logged)
    Clock::time_point menuPressedAt{};
    Clock::time_point lastToggle{}; //!< home and dock toggles: one a press, not two (a press seen twice)
    float lastStickX[2] = {0.0f, 0.0f};
    // The dock (X button): Home, keyboard, Follow all, time, volume.
    bool dockOpen = false;
    Spatial::Pose dockPose;
    struct DockHit {
        Spatial::DockItem item = Spatial::DockItem::None;
        float distance = 0.0f;
        Spatial::Vec3 point;
        bool bar = false; //!< on its grab bar
    };
    // Moving the dock (it sat in the middle of the view; a grab lets it sit lower or
    // higher): its grab bar with the trigger or the grip anywhere on it, as the keyboard; it opens where it was left,
    // relative to the gaze (kept with the window placements).
    struct DockGrab {
        int hand;
        bool byGrip;
        float distance;
        Spatial::Vec3 offsetLocal; //!< from the grabbed point to the dock's centre, dock-local
    };
    std::optional<DockGrab> dockGrab;
    float dockAzimuth = 0.0f; //!< from the gaze, radians
    float dockDistance = 0.76f; //!< horizontal, metres
    float dockHeight = -0.38f; //!< below the eyes (was 0.30, too high)
    Spatial::Pose dockOpenedHead; //!< the head in the room when the dock opened: where the dock's Recline turns to
    std::optional<int> dockBarArt;
    void beginDockGrab(int hand, bool byGrip, const DockHit &hit);
    void updateDockGrab(float dt);
    void endDockGrab();
    std::optional<DockHit> dockHits[2];
    Spatial::DockItem dockPressed[2] = {Spatial::DockItem::None, Spatial::DockItem::None};
    std::optional<FrameArt::Dock> dockArt;
    // Game mode: the controllers feed Rift Home's virtual gamepad instead of pointing.
    bool gameMode = false;
    VirtualGamepad gamepad;
    //! The other VR apps connected to the runtime (via libmonado): Home's cards switch to them.
    std::shared_ptr<RuntimeApps> runtimeApps;
    std::optional<Clock::time_point> gameMenuDownAt; //!< the left menu button held: a tap is Start, a second leaves
    int gameStartFrames = 0; //!< Start held this many more frames (a tap)
    bool gameLeaving = false; //!< left by holding the menu button: ignore it until it is let go
    void setGameMode(LayerBridge &bridge, bool on);
    bool gameHoldHinted = false; //!< "keep holding" said for this hold of the menu button
    //! A short message where the user looks (game mode left the user "stuck", no way back shown).
    std::optional<Clock::time_point> hintUntil;
    Spatial::Pose hintPose;
    void showHint(const QString &title, const QString &text, int seconds);
    /*!
     * Screenshot: one picture of what the user sees, saved as `~/Pictures/RiftHome/screenshots/<time>.png`. The
     * frame's layers (as sent to the runtime: where they are, and their pixels read back at full size) are drawn again
     * from the head by Xr::ViewShot on a worker thread; the controllers are left out. With
     * RIFT_HOME_SCREENSHOT_LAYERS=1 every layer is also dumped as its own file into a folder of the same name (HDR
     * and float layers as .rlof half floats, tools/rlof.py; 8-bit ones as PNG) with layers.txt, for debugging.
     */
    void saveScreenshot();
    //! The layers of the frame being built, for the screenshot (cleared with each frame's layers).
    std::vector<ShotLayer> shotLayers;
    //! A layer's pass image in host memory: the buffer and its memory are the caller's to free.
    struct Readback {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        int width = 0;
        int height = 0;
        bool half = false;
    };
    bool readBack(const SurfaceLayer &layer, Readback &out);
    //! The environment variable's debugging dump: every layer as its own file (saveScreenshot).
    void saveLayerDump(const QString &dir);
    //! The controllers' inputs this frame (grips by their squeeze) and the actions their bindings set off.
    Spatial::InputMask inputsDown = 0;
    Spatial::BindingTracker bindingTracker;
    Spatial::BindingTracker::Result bindingResult;
    std::array<bool, 2> gripDown{};
    //! options.buttons as it takes effect (the menu button opens Home when nothing else leads there), and what that was made of.
    Spatial::ButtonMap effectiveButtons;
    Spatial::ButtonMap effectiveFrom;
    bool effectiveReady = false;
    //! Recording a binding for Settings (RecordBinding over D-Bus): the next inputs pressed, until all are let go.
    bool recordingBinding = false;
    Clock::time_point recordingDeadline;
    Spatial::BindingRecorder bindingRecorder;
    //! Nothing fires from the inputs held when a recording ended until they are let go.
    bool inputsQuiet = false;
    int recordCommand = 0; //!< 1: start recording, 2: cancel (from XrCompositor::startBindingRecording, taken on the next frame)
    /*!
     * The lens test (Settings, Display & Room; XrCompositor::showLensTest): only it is shown, a picture for each eye
     * fixed on its lens's axis, until any button is pressed (Arming: shown, waiting for what was held when it opened to
     * be let go) or kLensTestLongest has passed.
     */
    enum class LensTest {
        Off,
        Arming,
        Shown,
    };
    LensTest lensTest = LensTest::Off;
    bool lensTestCommand = false; //!< asked for (taken on the next frame)
    bool lensTestQuiet = false; //!< since it ended: the controllers press nothing until all is let go
    Clock::time_point lensTestUntil;
    Spatial::Pose lensEyes[2]; //!< the eyes in VIEW space, this frame
    bool lensEyesValid = false;
    float lensDensity = 0.0f; //!< the panel's pixels a radian at the lens centre: the picture's
    int lensSpacing = -1; //!< the lens spacing painted (mm; 0: not known; -1: not painted)
    bool lensRuntimeCross = false; //!< the runtime draws the cross onto the panel (the picture leaves the middle black)
    //! Each frame: the test asked for started, the eyes located, the picture painted again when the spacing changed.
    void updateLensTest(XrTime displayTime);
    void endLensTest(const char *why);
    std::function<void(const QString &)> bindingRecorded; //!< the binding's name, empty when none (XR thread)
    //! Whether a button of @p hand given @p action is held; @p stickQuiet: a thumbstick's press counts (not just scrolled).
    bool actionHeld(int hand, Spatial::ButtonAction action, bool stickQuiet) const;
    //! What a button action does (the dock's items that have a binding, and the keyboard, screenshot and recline).
    void performAction(LayerBridge &bridge, Spatial::ButtonAction action);
    void endRecording(const QString &binding);
    int screenshotInFrames = 0; //!< from the dock: taken once the dock has gone
    bool screenshotPending = false;
    bool dragArt = false; //!< the drag badge is drawn
    //! A hand at rest: its ray hides after a while (Settings), until it moves or a button is pressed.
    struct HandRest {
        Spatial::Vec3 direction;
        Spatial::Vec3 position;
        Clock::time_point activeAt{};
        bool hidden = false;
        bool swallowTrigger = false; //!< the pull that woke the ray: not a click
    };
    HandRest handRest[2];
    bool wasDragging = false; //!< a drag and drop was in progress last frame (a tap when it starts and when it drops)
    // Notifications (Rift Home's notification server on the main thread): the newest as a card above
    // the dock's place for a few seconds, all of them in a list above the dock (its bell).
    uint64_t noteRevision = ~0ull;
    std::vector<Notification> notes; //!< newest first
    uint32_t noteCard = 0; //!< whose card shows (0: none)
    Spatial::Pose noteCardPose;
    bool notesOpen = false; //!< the dock's list
    struct NoteHit {
        bool card = false; //!< on the card (else on the list)
        Spatial::NotificationHit hit;
        float distance = 0.0f;
        Spatial::Vec3 point;
    };
    std::optional<NoteHit> noteHits[2];
    std::optional<NoteHit> notePressed[2];
    std::optional<std::tuple<uint64_t, int, int>> noteCardArt, noteListArt; //!< (revision, hovered part, hovered row)
    void takeNotifications(LayerBridge &bridge);
    bool noteCardShown() const;
    //! The action buttons of the card's notification, and of the list's shown rows.
    int noteCardActions() const;
    std::vector<int> noteListActions() const;
    Spatial::Pose noteListPose() const;
    std::optional<NoteHit> noteHitTest(Spatial::Vec3 origin, Spatial::Vec3 direction) const;
    void performNote(LayerBridge &bridge, const NoteHit &hit);
    // Home fades the windows out, cinema mode the other windows and the room's light: 0..1, eased.
    float homeFade = 0.0f;
    float cinemaFade = 0.0f;
    int homePlateArt = -1; //!< the app whose enlarged plate is drawn (the one under the ray)
    struct CinemaBefore {
        int windowId = 0;
        bool fullscreen = false;
        bool maximized = false;
    } cinemaBefore;
    // Self-test steps the frame code acts on (they need the bridge).
    std::optional<Spatial::FrameControl> selfTestMenu; //!< a frame menu action the self-test does as its button would
    std::optional<Spatial::DockItem> selfTestDock; //!< a dock item the self-test presses
    bool settingsToFront = false; //!< Settings was started from the dock: its window goes in front once it appears
    int settingsSeen = 0; //!< the Settings window last seen: a new one (a password prompt, the dock) comes in front
    LayerBridge *bridgeForStatus = nullptr; //!< the volume for the dock
    bool visualsUploaded = false;
    bool savePending = false;
    Clock::time_point lastSave = Clock::now();
    std::shared_ptr<FileWriter> layoutWriter; //!< the placements written off the frame thread
    // The scripted self-test (--xr-selftest): a synthetic right hand.
    bool selfTestSeenFocus = false;
    Clock::time_point focusedAt;
    int selfTestStep = -1;
    int selfTestWindow = 0;
    Spatial::Vec3 selfTestGrabDirection;
    Spatial::Vec3 selfTestLocal; //!< a frame control's point, window-local, when its drag began
    bool selfTestReported = false;

    // Counted per 10 s.
    int frames = 0;
    int copies = 0;
    int deferred = 0;
    int notShown = 0;
    int skipped = 0; //!< buffers of windows out of view kept for later, not copied (copies cost frames)
    int rescaled = 0; //!< filtered copies made again from the full-size image (a window nearer or farther, no new buffer)
    //! Where a frame's time goes (86-88 frames a second with the controllers): milliseconds from
    //! xrBeginFrame to xrEndFrame's return, the copies (with their wait), the controllers, xrEndFrame; sum and most.
    struct StageTime {
        double sum = 0.0;
        double most = 0.0;
        double last = 0.0;
        void add(double ms)
        {
            sum += ms;
            most = std::max(most, ms);
            last = ms;
        }
    };
    StageTime workTime, copyTime, copyWaitTime, controllerTime, endTime, otherTime;
    // Inside the copies, a frame's total of each: layers destroyed (their swapchains), waits for swapchain images, dmabuf
    // imports, and recording the passes (imports included).
    StageTime destroyTime, imageWaitTime, importTime, recordTime, finishTime; // finish: images released, imports freed
    StageTime spareTime; //!< spare swapchains destroyed (one a frame at most)
    double frameImportMs = 0.0;
    StageTime swapchainTime; //!< each xrCreateSwapchain (a layer new or of another size): sum and most, and how many
    int swapchainCount = 0;
    int slowFrames = 0; //!< frames whose own work took more than 9 ms (of the 11.1 a frame has)
    size_t lastLayerCount = 0;
    Clock::time_point statsSince = Clock::now();
    XrResult lastEndFrame = XR_SUCCESS;

    bool check(XrResult result, const char *what) const
    {
        if (XR_SUCCEEDED(result)) {
            return true;
        }
        char name[XR_MAX_RESULT_STRING_SIZE] = {};
        if (instance == XR_NULL_HANDLE || XR_FAILED(xrResultToString(instance, result, name))) {
            std::snprintf(name, sizeof(name), "%d", int(result));
        }
        qCWarning(lcXr) << what << "failed:" << name;
        return false;
    }

    int32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const
    {
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & flags) == flags) {
                return int32_t(i);
            }
        }
        return -1;
    }

    template<typename F>
    F proc(const char *name) const
    {
        PFN_xrVoidFunction f = nullptr;
        xrGetInstanceProcAddr(instance, name, &f);
        return reinterpret_cast<F>(f);
    }

    bool init();
    void shutdown();
    void pollEvents(bool &exit);
    void frame(LayerBridge &bridge, const Options &options);
    bool ensureSwapchain(SurfaceLayer &layer, const Target &target, int width, int height);
    //! @p layer's swapchain takes a picture of @p width x @p height (at its level) as it is: no new one needed.
    bool swapchainFits(const SurfaceLayer &layer, VkFormat format, int width, int height) const;
    void destroyLayer(uint64_t id);
    bool createWindowPass();
    bool createImage(VkFormat format, int width, int height, VkImageUsageFlags usage, VkImage &image, VkDeviceMemory &memory);
    //! @p planes from @p firstPlane (all: -1) as one image of @p width x @p height (0: the buffer's).
    bool importDmabuf(const BufferHandoff &buffer, VkFormat format, int firstPlane, int planes, int width, int height, VkImage &image,
                      VkDeviceMemory &memory);
    //! A client dmabuf's images, P010 as two (@p yuv); thread-safe (the Importer's thread runs it too).
    std::optional<ImportedBuffer> importBuffer(const BufferHandoff &buffer, VkFormat first, bool yuv);
    /*!
     * A client dmabuf's images for a copy: its kept import (importCache), or a new one, kept when the buffer has an id
     * (else freed with the frame's @p imports). @p chroma: P010's second plane (@p yuv).
     */
    bool dmabufSources(const BufferHandoff &buffer, VkFormat format, bool yuv, std::vector<std::pair<VkImage, VkDeviceMemory>> &imports,
                       VkImage &source, VkImage &chroma);
    //! Client images change hands for each use: taken from the client (foreign queue) before, handed back after.
    void takeFromClient(VkImage image);
    void handBackToClient(VkImage image);
    //! Frees @p imported: with the frame's other imports (@p later) once the frame's copies are done, or else off the
    //! frame thread (freeLater).
    void dropImport(const ImportedBuffer &imported, std::vector<std::pair<VkImage, VkDeviceMemory>> *later);
    //! Any thread: @p imported's objects destroyed now.
    static void destroyImportNow(VkDevice device, const ImportedBuffer &imported);
    //! Vulkan objects nothing uses any more, freed on the Importer's thread (freeing big images and imports cost the
    //! frame up to 14 ms), or here without it.
    void freeLater(std::function<void()> free);
    //! @p image and @p memory (either may be null) freed later (freeLater).
    void freeImageLater(VkImage image, VkDeviceMemory memory);
    //! At a frame's start (no copy in flight): imports of buffers gone, unused a while, or past the most kept.
    void pruneImports(const LayerBridge::Frame &taken);
    VkImage uploadShm(SurfaceLayer &layer, const BufferHandoff &buffer, VkFormat format);
    VkImageView view(VkImage image, VkFormat format, VkImageUsageFlags usage);
    bool recordCopy(SurfaceLayer &layer, const BufferHandoff &buffer, VkImage swapchainImage, const Target &target,
                    const SurfaceGeometry &geometry, float radius, std::vector<std::pair<VkImage, VkDeviceMemory>> &imports);
    bool recordShow(SurfaceLayer &layer, VkImage swapchainImage);
    void barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                 uint32_t srcFamily = VK_QUEUE_FAMILY_IGNORED, uint32_t dstFamily = VK_QUEUE_FAMILY_IGNORED);
    void logStats();
    void interact(LayerBridge &bridge, XrTime displayTime, float dt);
    void layoutChrome();
    std::optional<ChromeHit> chromeHitTest(Spatial::Vec3 origin, Spatial::Vec3 direction);
    void beginChromeDrag(int hand, const ChromeHit &hit);
    void endChromeDrag(LayerBridge &bridge, int hand);
    void performMenu(LayerBridge &bridge, int hand, int windowId, Spatial::FrameControl control);
    //! layout.makeMain() with what it did logged: where the window went and which windows moved aside for it (Settings
    //! once opened over Chromium instead of moving it aside).
    void makeMain(int windowId, const char *why);
    bool releasedNear(int hand, const ChromeDrag &drag) const;
    void updateChromeArt();
    void openKeyboard(bool automatic, float azimuth);
    Spatial::Quat facingEye(Spatial::Vec3 position) const;
    void beginKeyboardGrab(int hand, bool byGrip, const KeyHit &hit);
    void updateKeyboardGrab(float dt);
    void endKeyboardGrab();
    void closeKeyboard(LayerBridge &bridge);
    std::optional<KeyHit> keyboardHitTest(Spatial::Vec3 origin, Spatial::Vec3 direction) const;
    void sendKeys(LayerBridge &bridge, const std::vector<Spatial::KeyEvent> &events);
    void openLauncher(bool open);
    int homeHovered() const; //!< the app (index in options.apps) under a ray in home, -1 if none
    void openDock(bool open);
    //! Recline on (the arrangement turns to @p view, a head in the room) or off (back upright), over half a second.
    void setRecline(bool on, const Spatial::Pose &view);
    std::optional<DockHit> dockHitTest(Spatial::Vec3 origin, Spatial::Vec3 direction) const;
    void performDock(LayerBridge &bridge, int hand, Spatial::DockItem item);
    //! The Settings app's window (app id org.rifthome.settings), or 0.
    //! Shown while a film plays in cinema mode: the film's window, and Settings over it (Settings
    //! did not open in cinema mode; the room's light during a film is set there).
    bool shownInCinema(int windowId) const
    {
        return windowId == layout.cinemaWindow() || (windowId != 0 && windowId == settingsWindow());
    }
    int settingsWindow() const
    {
        for (const auto &[id, window] : layout.windows()) {
            if (window.appId == "org.rifthome.settings") {
                return id;
            }
        }
        return 0;
    }
    std::optional<CellHit> launcherHitTest(Spatial::Vec3 origin, Spatial::Vec3 direction) const;
    QString labelFor(int windowId, Spatial::FrameControl control) const;
    bool selfTestHand(XrInput::Hand &hand);
    //! Settings changed (XR thread): new art in the new style, the room rebuilt off the frame loop when its photo changed.
    void applySettings(const XrCompositor::LiveSettings &settings);
    //! A new room being built from a photo on its own thread (shared: Impl is reset by assignment at shutdown).
    struct EnvironmentJob {
        std::mutex mutex;
        bool done = false;
        bool finished = false; //!< its thread has ended: joining it is instant
        RoomImage room;
        bool panorama = false;
        std::thread thread;
        ~EnvironmentJob()
        {
            if (thread.joinable()) {
                thread.join();
            }
        }
    };
    std::shared_ptr<EnvironmentJob> environmentJob;
    //! Rooms still loading when another was chosen (the room chooser invites trying a few): dropped once their thread
    //! has ended, never joined on this thread while a large picture is still being read.
    std::vector<std::shared_ptr<EnvironmentJob>> retiredEnvironmentJobs;
    void retireEnvironmentJob()
    {
        if (environmentJob) {
            retiredEnvironmentJobs.push_back(std::move(environmentJob));
            environmentJob.reset();
        }
    }
    void saveLayout(bool force);
    void uploadVisuals(LayerBridge::Frame &frame);
    void loadEnvironment();
};

bool XrCompositor::Impl::init()
{
    uint32_t count = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    std::vector<XrExtensionProperties> available(count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, available.data());
    bool vulkan2 = false;
    for (const XrExtensionProperties &p : available) {
        vulkan2 |= std::strcmp(p.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0;
        hdrExtension |= std::strcmp(p.extensionName, XR_MNDX_COMPOSITION_LAYER_HDR_EXTENSION_NAME) == 0;
        cylinderLayers |= std::strcmp(p.extensionName, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME) == 0;
        equirectLayers |= std::strcmp(p.extensionName, XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME) == 0;
        colorScaleLayers |= std::strcmp(p.extensionName, XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME) == 0;
        blurLayers |= std::strcmp(p.extensionName, XR_MNDX_COMPOSITION_LAYER_BACKDROP_BLUR_EXTENSION_NAME) == 0;
    }
    if (!vulkan2) {
        qCWarning(lcXr) << "the OpenXR runtime has no XR_KHR_vulkan_enable2 (is XR_RUNTIME_JSON RiftLinux's runtime?)";
        return false;
    }
    std::vector<const char *> extensions{XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    if (hdrExtension) {
        extensions.push_back(XR_MNDX_COMPOSITION_LAYER_HDR_EXTENSION_NAME);
    }
    if (cylinderLayers) {
        extensions.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
    }
    if (equirectLayers) {
        extensions.push_back(XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME);
    }
    if (colorScaleLayers) {
        extensions.push_back(XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME);
    }
    if (blurLayers) {
        extensions.push_back(XR_MNDX_COMPOSITION_LAYER_BACKDROP_BLUR_EXTENSION_NAME);
        qCInfo(lcXr) << "frosted glass: the runtime blurs behind Rift Home's glass (XR_MNDX_composition_layer_backdrop_blur)";
        FrameArt::setFrostedGlass(true);
    }

    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(ici.applicationInfo.applicationName, appName().c_str(), XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(ici.applicationInfo.engineName, "Rift Home", XR_MAX_ENGINE_NAME_SIZE - 1);
    ici.applicationInfo.applicationVersion = 1;
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = uint32_t(extensions.size());
    ici.enabledExtensionNames = extensions.data();
    if (!check(xrCreateInstance(&ici, &instance), "xrCreateInstance")) {
        return false;
    }
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(instance, &ip);

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!check(xrGetSystem(instance, &sgi, &system), "xrGetSystem")) {
        return false;
    }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(instance, system, &sp);
    maxLayers = sp.graphicsProperties.maxLayerCount;
    qCInfo(lcXr).noquote() << "runtime" << ip.runtimeName << "| system" << sp.systemName << "| max layers" << maxLayers
                           << "| HDR layer descriptions (XR_MNDX_composition_layer_hdr):" << (hdrExtension ? "yes" : "NO");

    auto getRequirements = proc<PFN_xrGetVulkanGraphicsRequirements2KHR>("xrGetVulkanGraphicsRequirements2KHR");
    auto createVkInstance = proc<PFN_xrCreateVulkanInstanceKHR>("xrCreateVulkanInstanceKHR");
    auto getVkDevice = proc<PFN_xrGetVulkanGraphicsDevice2KHR>("xrGetVulkanGraphicsDevice2KHR");
    auto createVkDevice = proc<PFN_xrCreateVulkanDeviceKHR>("xrCreateVulkanDeviceKHR");
    if (!getRequirements || !createVkInstance || !getVkDevice || !createVkDevice) {
        qCWarning(lcXr) << "XR_KHR_vulkan_enable2 functions missing";
        return false;
    }
    XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    if (!check(getRequirements(instance, system, &requirements), "xrGetVulkanGraphicsRequirements2KHR")) {
        return false;
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Rift Home VR";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo vici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    vici.pApplicationInfo = &app;
    XrVulkanInstanceCreateInfoKHR xvici{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xvici.systemId = system;
    xvici.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    xvici.vulkanCreateInfo = &vici;
    VkResult vr = VK_SUCCESS;
    if (!check(createVkInstance(instance, &xvici, &vkInstance, &vr), "xrCreateVulkanInstanceKHR") || vr != VK_SUCCESS) {
        qCWarning(lcXr) << "Vulkan instance:" << vr;
        return false;
    }
    XrVulkanGraphicsDeviceGetInfoKHR gdi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    gdi.systemId = system;
    gdi.vulkanInstance = vkInstance;
    if (!check(getVkDevice(instance, &gdi, &physical), "xrGetVulkanGraphicsDevice2KHR")) {
        return false;
    }
    VkPhysicalDeviceProperties pp{};
    vkGetPhysicalDeviceProperties(physical, &pp);
    vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
    bool found = false;
    for (uint32_t i = 0; i < familyCount && !found; ++i) {
        if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            queueFamily = i;
            found = true;
        }
    }
    if (!found) {
        qCWarning(lcXr) << "no graphics+compute queue";
        return false;
    }

    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> deviceExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, deviceExtensions.data());
    const auto has = [&](const char *name) {
        for (const VkExtensionProperties &e : deviceExtensions) {
            if (std::strcmp(e.extensionName, name) == 0) {
                return true;
            }
        }
        return false;
    };
    std::vector<const char *> enabled;
    for (const char *name : {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
                             VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME}) {
        if (has(name)) {
            enabled.push_back(name);
        }
    }
    dmabufImport = enabled.size() == 3;
    if (has(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)) {
        enabled.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
        queueFamilyForeign = true;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(enabled.size());
    dci.ppEnabledExtensionNames = enabled.data();
    XrVulkanDeviceCreateInfoKHR xdci{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xdci.systemId = system;
    xdci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    xdci.vulkanPhysicalDevice = physical;
    xdci.vulkanCreateInfo = &dci;
    if (!check(createVkDevice(instance, &xdci, &device, &vr), "xrCreateVulkanDeviceKHR") || vr != VK_SUCCESS) {
        qCWarning(lcXr) << "Vulkan device:" << vr;
        return false;
    }
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    getMemoryFdProperties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR"));
    dmabufImport = dmabufImport && getMemoryFdProperties;

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = queueFamily;
    vkCreateCommandPool(device, &cpci, nullptr, &pool);
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    vkAllocateCommandBuffers(device, &cbai, &cmd);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(device, &fci, nullptr, &fence);
    if (!createWindowPass()) {
        return false;
    }

    XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    binding.instance = vkInstance;
    binding.physicalDevice = physical;
    binding.device = device;
    binding.queueFamilyIndex = queueFamily;
    binding.queueIndex = 0;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = system;
    if (!check(xrCreateSession(instance, &sci, &session), "xrCreateSession")) {
        return false;
    }
    runtimeApps = std::make_shared<RuntimeApps>(appName());
    runtimeApps->start();
    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    if (!check(xrCreateReferenceSpace(session, &rsci, &space), "xrCreateReferenceSpace")) {
        return false;
    }
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!check(xrCreateReferenceSpace(session, &rsci, &viewSpace), "xrCreateReferenceSpace (view)")) {
        return false;
    }
    inputReady = input.init(instance, session);
    loadEnvironment();
    if (!options.stateFile.isEmpty()) {
        QFile file(options.stateFile);
        if (file.open(QIODevice::ReadOnly)) {
            const QJsonObject state = QJsonDocument::fromJson(file.readAll()).object();
            layout.load(state);
            const auto place = [&](const char *name, float &azimuth, float &distance, float &height, float maxAzimuth, float minDistance,
                                   float maxDistance, float lowest, float highest) {
                const QJsonObject o = state.value(QLatin1String(name)).toObject();
                if (!o.isEmpty()) {
                    azimuth = std::clamp(float(o.value(QStringLiteral("azimuth")).toDouble(azimuth)), -maxAzimuth, maxAzimuth);
                    distance = std::clamp(float(o.value(QStringLiteral("distance")).toDouble(distance)), minDistance, maxDistance);
                    height = std::clamp(float(o.value(QStringLiteral("height")).toDouble(height)), lowest, highest);
                }
            };
            place("dock", dockAzimuth, dockDistance, dockHeight, kDockMaxAzimuth, kDockMinDistance, kDockMaxDistance, kDockLowest, kDockHighest);
            place("keyboard", keyboardAzimuth, keyboardDistance, keyboardHeight, 0.8f, 0.3f, 1.2f, -0.9f, 0.1f);
            qCInfo(lcXr).noquote() << "window placements from" << options.stateFile;
        }
    }

    // The runtime's 1:1 size at the lens centre (RiftLinux asks applications for it): sharp windows' density.
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
    std::vector<XrViewConfigurationView> configViews(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    if (viewCount > 0
        && XR_SUCCEEDED(xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount,
                                                          &viewCount, configViews.data()))) {
        eyeWidth = configViews[0].recommendedImageRectWidth;
    }

    uint32_t formatCount = 0;
    xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr);
    std::vector<int64_t> formats(formatCount);
    xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data());
    swapchainFormats.insert(formats.begin(), formats.end());
    QStringList formatNames;
    for (int64_t f : formats) {
        formatNames << QString::number(f);
    }

    qCInfo(lcXr).noquote() << "Vulkan" << pp.deviceName << "| dmabuf import:" << (dmabufImport ? "yes" : "NO")
                           << "| foreign queue family:" << (queueFamilyForeign ? "yes" : "no")
                           << "| swapchain formats (VkFormat):" << formatNames.join(QLatin1Char(','));
    createControllerLayer(); // knowing the swapchain formats (HDR: 16-bit float)
    if (dmabufImport) {
        importer = std::make_shared<Importer>(
            swapchainFormats,
            [this](const BufferHandoff &buffer, VkFormat first, bool yuv) {
                return importBuffer(buffer, first, yuv);
            },
            [device = device](const ImportedBuffer &imported) {
                destroyImportNow(device, imported);
            },
            kImportsKept);
    }
    return true;
}

void XrCompositor::Impl::destroyLayer(uint64_t id)
{
    auto it = layers.find(id);
    if (it == layers.end()) {
        return;
    }
    SurfaceLayer &layer = it->second;
    spareSwapchain(layer);
    // Off the frame thread (a 4K picture's images are hundreds of MB): nothing uses them any more (the copies are done).
    if (layer.staging != VK_NULL_HANDLE) {
        freeLater([device = device, staging = layer.staging, memory = layer.stagingMemory] {
            vkDestroyBuffer(device, staging, nullptr);
            vkFreeMemory(device, memory, nullptr);
        });
    }
    freeImageLater(layer.sourceImage, layer.sourceMemory);
    freeImageLater(layer.passImage, layer.passMemory);
    freeImageLater(layer.scaledImage, layer.scaledMemory);
    layers.erase(it);
    warned.erase(id);
}

void XrCompositor::Impl::spareSwapchain(SurfaceLayer &layer)
{
    if (layer.swapchain == XR_NULL_HANDLE) {
        return;
    }
    spareSwapchains.push_back({layer.swapchain, std::move(layer.images), layer.format, layer.swapWidth, layer.swapHeight, Clock::now()});
    layer.swapchain = XR_NULL_HANDLE;
    layer.images.clear();
    // A burst (a window with many surfaces closed): beyond twice the limit, the oldest go at once (bounded).
    while (spareSwapchains.size() > 2 * kSpareSwapchains) {
        xrDestroySwapchain(spareSwapchains.front().swapchain);
        spareSwapchains.erase(spareSwapchains.begin());
    }
}

void XrCompositor::Impl::pruneSpareSwapchains(bool all)
{
    if (all) {
        for (const SpareSwapchain &spare : spareSwapchains) {
            xrDestroySwapchain(spare.swapchain);
        }
        spareSwapchains.clear();
        return;
    }
    if (spareSwapchains.empty()) {
        return;
    }
    double megabytes = 0.0;
    for (const SpareSwapchain &spare : spareSwapchains) {
        megabytes += spare.megabytes();
    }
    // Over the limits: the oldest; else the first past its time (small ones are kept much longer).
    auto gone = spareSwapchains.end();
    if (spareSwapchains.size() > kSpareSwapchains || megabytes > kSpareMegabytes) {
        gone = spareSwapchains.begin();
    } else {
        const Clock::time_point now = Clock::now();
        gone = std::find_if(spareSwapchains.begin(), spareSwapchains.end(), [now](const SpareSwapchain &spare) {
            return now - spare.since >= spare.kept();
        });
    }
    if (gone != spareSwapchains.end()) {
        xrDestroySwapchain(gone->swapchain);
        spareSwapchains.erase(gone);
    }
}

void XrCompositor::Impl::shutdown()
{
    screenshotPool->waitForDone();
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
    }
    while (!layers.empty()) {
        destroyLayer(layers.begin()->first);
    }
    importer.reset(); // its thread ends (the bridge no longer asks: XrCompositor::run); its uncollected imports freed
    for (const auto &[id, imported] : importCache) {
        dropImport(imported, nullptr);
    }
    importCache.clear();
    failedImports.clear();
    pruneSpareSwapchains(true);
    saveLayout(true);
    input.destroy();
    if (space != XR_NULL_HANDLE) {
        xrDestroySpace(space);
    }
    if (viewSpace != XR_NULL_HANDLE) {
        xrDestroySpace(viewSpace);
    }
    runtimeApps.reset(); // its thread ends first
    destroyControllerLayer();
    if (session != XR_NULL_HANDLE) {
        xrDestroySession(session);
    }
    if (device != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        vkDestroyPipeline(device, pipelineRgba8, nullptr);
        vkDestroyPipeline(device, pipelineRgba16f, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        vkDestroyPipeline(device, downsampleRgba8, nullptr);
        vkDestroyPipeline(device, downsampleRgba16f, nullptr);
        vkDestroyPipelineLayout(device, downsamplePipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(device, downsampleSetLayout, nullptr);
        vkDestroySampler(device, sampler, nullptr);
        vkDestroySampler(device, chromaSampler, nullptr);
        vkDestroyFence(device, fence, nullptr);
        vkDestroyCommandPool(device, pool, nullptr);
        vkDestroyDevice(device, nullptr);
    }
    if (vkInstance != VK_NULL_HANDLE) {
        vkDestroyInstance(vkInstance, nullptr);
    }
    if (instance != XR_NULL_HANDLE) {
        xrDestroyInstance(instance);
    }
    const Options kept = options;
    *this = Impl();
    options = kept;
}

void XrCompositor::Impl::pollEvents(bool &exit)
{
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto *changed = reinterpret_cast<const XrEventDataSessionStateChanged *>(&event);
            qCInfo(lcXr) << "session" << stateName(changed->state) << "(runtime time" << changed->time / 1000000 << "ms)";
            focused = changed->state == XR_SESSION_STATE_FOCUSED;
            switch (changed->state) {
            case XR_SESSION_STATE_READY: {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                running = check(xrBeginSession(session, &begin), "xrBeginSession");
                break;
            }
            case XR_SESSION_STATE_STOPPING:
                xrEndSession(session);
                running = false;
                break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                exit = true;
                break;
            default:
                break;
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            exit = true;
        } else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            // Recentering moves LOCAL to the user: the arrangement is re-expressed around the new origin, straight in
            // front (visionOS's recentre). RiftLinux's runtime sends this without the pose (pose_valid false).
            // Pinned windows stay in the room: the recentre's movement comes from the jump in the located head
            // poses around it (the runtime moves LOCAL before it sends this), worked out a few frames later.
            const auto *change = reinterpret_cast<const XrEventDataReferenceSpaceChangePending *>(&event);
            if (change->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                recentreFrames = 3;
            }
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

bool XrCompositor::Impl::swapchainFits(const SurfaceLayer &layer, VkFormat format, int width, int height) const
{
    const int factor = layer.level == Spatial::Sharpness::kUpLevel ? 2 : 1;
    const int needWidth = width * factor;
    const int needHeight = height * factor;
    return layer.swapchain != XR_NULL_HANDLE && layer.format == format && layer.swapWidth >= needWidth && layer.swapHeight >= needHeight
        && double(layer.swapWidth) * layer.swapHeight <= 2.25 * double(needWidth) * needHeight;
}

bool XrCompositor::Impl::ensureSwapchain(SurfaceLayer &layer, const Target &target, int width, int height)
{
    // The pass image: the full-size picture, the buffer's size. Kept while only the swapchain changes.
    const bool passOk = layer.passImage != VK_NULL_HANDLE && layer.storage == target.storage && layer.width == width && layer.height == height;
    if (!passOk) {
        // (Not in this frame's copies yet; the last frame's are done.)
        freeImageLater(layer.passImage, layer.passMemory);
        layer.passImage = VK_NULL_HANDLE;
        layer.passMemory = VK_NULL_HANDLE;
        freeImageLater(layer.scaledImage, layer.scaledMemory);
        layer.scaledImage = VK_NULL_HANDLE;
        layer.scaledMemory = VK_NULL_HANDLE;
        layer.hasImage = false;
        layer.shownWidth = layer.shownHeight = 0;
        if (!createImage(target.storage, width, height, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         layer.passImage, layer.passMemory)) {
            qCWarning(lcXr) << "window pass image not created:" << int(target.storage) << width << "x" << height;
            return false;
        }
        layer.storage = target.storage;
        layer.width = width;
        layer.height = height;
    }
    // The swapchain: the buffer's size, twice it while the layer shows its enlarged copy. Made a little bigger than that
    // and kept while the picture still fits (its top-left part shown, shownRect): a new swapchain costs the runtime 1-11
    // ms, and frames with several (a window scaled, Home's art changing size) dropped frames.
    const int factor = layer.level == Spatial::Sharpness::kUpLevel ? 2 : 1;
    const VkFormat format = target.swapchain;
    const int needWidth = width * factor;
    const int needHeight = height * factor;
    if (swapchainFits(layer, format, width, height)) {
        return true;
    }
    const auto roomy = [](int pixels) {
        return (pixels + pixels / 8 + 15) / 16 * 16; // an eighth more, in steps of 16
    };
    spareSwapchain(layer); // a size it may take again (a window resized back), or another layer's
    // A spare that takes the picture as swapchainFits() would: no new swapchain (newest first).
    for (auto spare = spareSwapchains.rbegin(); spare != spareSwapchains.rend(); ++spare) {
        if (spare->format == format && spare->width >= needWidth && spare->height >= needHeight
            && double(spare->width) * spare->height <= 2.25 * double(needWidth) * needHeight) {
            layer.swapchain = spare->swapchain;
            layer.images = std::move(spare->images);
            layer.format = spare->format;
            layer.swapWidth = spare->width;
            layer.swapHeight = spare->height;
            spareSwapchains.erase(std::next(spare).base());
            ++swapchainsReused;
            return true;
        }
    }
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = format;
    ci.sampleCount = 1;
    ci.width = uint32_t(roomy(needWidth));
    ci.height = uint32_t(roomy(needHeight));
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    const Clock::time_point creating = Clock::now();
    const bool created = check(xrCreateSwapchain(session, &ci, &layer.swapchain), "xrCreateSwapchain");
    swapchainTime.add(std::chrono::duration<double, std::milli>(Clock::now() - creating).count());
    ++swapchainCount;
    if (!created) {
        layer.swapchain = XR_NULL_HANDLE;
        return false;
    }
    uint32_t count = 0;
    xrEnumerateSwapchainImages(layer.swapchain, 0, &count, nullptr);
    std::vector<XrSwapchainImageVulkan2KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    xrEnumerateSwapchainImages(layer.swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data()));
    for (const XrSwapchainImageVulkan2KHR &image : images) {
        layer.images.push_back(image.image);
    }
    layer.format = format;
    layer.swapWidth = int(ci.width);
    layer.swapHeight = int(ci.height);
    return true;
}

void XrCompositor::Impl::createControllerLayer()
{
    // Rift Home's own models, one a hand (their buttons and grips differ): the first folder that has both.
    testControllers = qEnvironmentVariableIntValue("RIFT_HOME_TEST_CONTROLLERS") == 1;
    if (qEnvironmentVariable("RIFT_HOME_CONTROLLERS") == QLatin1String("0")) {
        return;
    }
    std::optional<ControllerMesh> right, left;
    QString from;
    for (const QString &dir : options.controllerModelDirs) {
        const QString r = QDir(dir).filePath(QStringLiteral("ControllerRight.glb"));
        const QString l = QDir(dir).filePath(QStringLiteral("ControllerLeft.glb"));
        if (QFileInfo::exists(r) && QFileInfo::exists(l)) {
            right = loadControllerModel(r);
            left = loadControllerModel(l);
            from = dir;
            break;
        }
    }
    if (!right || !left) {
        qCInfo(lcXr).noquote() << "controllers: no models (ControllerLeft.glb and ControllerRight.glb) in"
                               << options.controllerModelDirs.join(QStringLiteral(", ")) << "- only the rays are drawn";
        return;
    }
    qCInfo(lcXr).noquote() << "controllers: models from" << from << "| right:" << right->summary << "| left:" << left->summary;
    uint32_t count = 0;
    xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr);
    std::vector<XrViewConfigurationView> views(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, views.data());
    if (count < 2) {
        qCWarning(lcXr) << "controllers: not drawn (no stereo view configuration)";
        return;
    }
    auto layer = std::make_shared<ControllerLayer>();
    // Most of the eye's resolution: the models are small and 4x multisampled.
    layer->width = std::max(256u, uint32_t(float(views[0].recommendedImageRectWidth) * 0.8f));
    layer->height = std::max(256u, uint32_t(float(views[0].recommendedImageRectHeight) * 0.8f));
    // 8-bit sRGB like every other layer of Rift Home's (with the controllers in an HDR layer the whole
    // picture looked pixelated, its screen door far stronger and it bounced: the runtime maps a frame with an HDR layer
    // differently). RIFT_HOME_CONTROLLERS_HDR=1: linear float with highlights past white, for trying.
    layer->hdr = qEnvironmentVariableIntValue("RIFT_HOME_CONTROLLERS_HDR") == 1 && hdrExtension
        && swapchainFormats.contains(VK_FORMAT_R16G16B16A16_SFLOAT);
    const VkFormat format = layer->hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_SRGB;
    for (int eye = 0; eye < 2; ++eye) {
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        ci.format = format;
        ci.sampleCount = 1;
        ci.width = layer->width;
        ci.height = layer->height;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        if (!check(xrCreateSwapchain(session, &ci, &layer->swapchains[eye]), "xrCreateSwapchain (controllers)")) {
            layer->swapchains[eye] = XR_NULL_HANDLE;
            controllerLayer = layer;
            destroyControllerLayer();
            return;
        }
        uint32_t images = 0;
        xrEnumerateSwapchainImages(layer->swapchains[eye], 0, &images, nullptr);
        std::vector<XrSwapchainImageVulkan2KHR> list(images, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
        xrEnumerateSwapchainImages(layer->swapchains[eye], images, &images, reinterpret_cast<XrSwapchainImageBaseHeader *>(list.data()));
        for (const XrSwapchainImageVulkan2KHR &image : list) {
            layer->images[eye].push_back(image.image);
        }
    }
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    vkAllocateCommandBuffers(device, &cbai, &layer->cmd);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(device, &fci, nullptr, &layer->fence);
    controllerLayer = layer;
    if (!layer->renderer.init(physical, device, format, layer->width, layer->height, layer->images, *right, *left)) {
        destroyControllerLayer();
        return;
    }
    qCInfo(lcXr).noquote() << "controllers:" << (layer->hdr ? "HDR (16-bit float, linear light)" : "SDR (8-bit sRGB)") << "layer";
}

void XrCompositor::Impl::destroyControllerLayer()
{
    if (!controllerLayer) {
        return;
    }
    controllerLayer->renderer.destroy();
    if (controllerLayer->fence != VK_NULL_HANDLE) {
        vkDestroyFence(device, controllerLayer->fence, nullptr);
    }
    if (controllerLayer->cmd != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(device, pool, 1, &controllerLayer->cmd);
    }
    for (XrSwapchain swapchain : controllerLayer->swapchains) {
        if (swapchain != XR_NULL_HANDLE) {
            xrDestroySwapchain(swapchain);
        }
    }
    controllerLayer.reset();
}

bool XrCompositor::Impl::createWindowPass()
{
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(device, &sci, nullptr, &sampler);
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    vkCreateSampler(device, &sci, nullptr, &chromaSampler);

    const VkDescriptorSetLayoutBinding bindings[3] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, // P010's chroma (else the source)
    };
    VkDescriptorSetLayoutCreateInfo dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsl.bindingCount = 3;
    dsl.pBindings = bindings;
    vkCreateDescriptorSetLayout(device, &dsl, nullptr, &setLayout);
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PassParams)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &setLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    vkCreatePipelineLayout(device, &plci, nullptr, &pipelineLayout);

    const auto pipeline = [&](const uint32_t *code, size_t bytes, VkPipeline &out, VkPipelineLayout layout) {
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = bytes;
        smci.pCode = code;
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device, &smci, nullptr, &module) != VK_SUCCESS) {
            return false;
        }
        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = module;
        cpci.stage.pName = "main";
        cpci.layout = layout;
        const VkResult r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &out);
        vkDestroyShaderModule(device, module, nullptr);
        return r == VK_SUCCESS;
    };
    if (!pipeline(kWindowPassRgba8, sizeof(kWindowPassRgba8), pipelineRgba8, pipelineLayout)
        || !pipeline(kWindowPassRgba16f, sizeof(kWindowPassRgba16f), pipelineRgba16f, pipelineLayout)) {
        qCWarning(lcXr) << "window pass pipelines not created";
        return false;
    }

    // The resample pass: the full-size image in, the smaller or enlarged copy out, both storage images.
    const VkDescriptorSetLayoutBinding downsampleBindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    dsl.pBindings = downsampleBindings;
    vkCreateDescriptorSetLayout(device, &dsl, nullptr, &downsampleSetLayout);
    const VkPushConstantRange downsampleRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DownsampleParams)};
    plci.pSetLayouts = &downsampleSetLayout;
    plci.pPushConstantRanges = &downsampleRange;
    vkCreatePipelineLayout(device, &plci, nullptr, &downsamplePipelineLayout);
    if (!pipeline(kDownsampleRgba8, sizeof(kDownsampleRgba8), downsampleRgba8, downsamplePipelineLayout)
        || !pipeline(kDownsampleRgba16f, sizeof(kDownsampleRgba16f), downsampleRgba16f, downsamplePipelineLayout)) {
        qCWarning(lcXr) << "downsample pipelines not created: far windows are not filtered";
        sharpOff = true;
    }
    if (qEnvironmentVariableIsSet("RIFT_HOME_SHARP") && qEnvironmentVariableIntValue("RIFT_HOME_SHARP") == 0) {
        sharpOff = true;
    }
    if (const float factor = qEnvironmentVariable("RIFT_HOME_SHARP_FACTOR").toFloat(); factor > 0.0f) {
        sharpFactor = factor;
    }

    // A window pass and a downsample pass a copy (the window pass alone for most).
    const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 512}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 768}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 512;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = sizes;
    return vkCreateDescriptorPool(device, &dpci, nullptr, &descriptorPool) == VK_SUCCESS;
}

VkImageView XrCompositor::Impl::view(VkImage image, VkFormat format, VkImageUsageFlags usage)
{
    VkImageViewUsageCreateInfo usageInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    usageInfo.usage = usage;
    VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivci.pNext = &usageInfo;
    ivci.image = image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = format;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(device, &ivci, nullptr, &v) == VK_SUCCESS) {
        frameViews.push_back(v);
    }
    return v;
}

void XrCompositor::Impl::barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
                                 VkAccessFlags dstAccess, uint32_t srcFamily, uint32_t dstFamily)
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = srcFamily;
    b.dstQueueFamilyIndex = dstFamily;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
}

bool XrCompositor::Impl::createImage(VkFormat format, int width, int height, VkImageUsageFlags usage, VkImage &image,
                                     VkDeviceMemory &memory)
{
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {uint32_t(width), uint32_t(height), 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &ici, nullptr, &image) != VK_SUCCESS) {
        image = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, image, &requirements);
    const int32_t type = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = requirements.size;
    mai.memoryTypeIndex = uint32_t(type);
    if (type < 0 || vkAllocateMemory(device, &mai, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyImage(device, image, nullptr);
        image = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        return false;
    }
    vkBindImageMemory(device, image, memory, 0);
    return true;
}

bool XrCompositor::Impl::importDmabuf(const BufferHandoff &buffer, VkFormat format, int firstPlane, int planes, int width,
                                      int height, VkImage &image, VkDeviceMemory &memory)
{
    image = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    if (!dmabufImport || buffer.planes.empty()) {
        return false;
    }
    // One memory object for the planes imported: they must be the same dmabuf. (P010 from a VA-API decoder comes as two
    // dmabufs, one a plane: each plane is imported by itself.)
    const int last = planes < 0 ? int(buffer.planes.size()) : std::min(int(buffer.planes.size()), firstPlane + planes);
    if (firstPlane < 0 || firstPlane >= last) {
        return false;
    }
    const int memoryFd = buffer.planes[size_t(firstPlane)].fd;
    struct stat first{};
    fstat(memoryFd, &first);
    for (int i = firstPlane; i < last; ++i) {
        struct stat st{};
        fstat(buffer.planes[size_t(i)].fd, &st);
        if (st.st_ino != first.st_ino) {
            qCWarning(lcXr) << "dmabuf with planes in separate buffers: not imported";
            return false;
        }
    }

    std::vector<VkSubresourceLayout> layouts;
    for (int i = firstPlane; i < last; ++i) {
        layouts.push_back({buffer.planes[size_t(i)].offset, 0, buffer.planes[size_t(i)].stride, 0, 0});
    }
    if (layouts.empty()) {
        return false;
    }
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
    modifier.drmFormatModifier = buffer.planes[size_t(firstPlane)].modifier; // the planes imported share it (checked by the client's layout)
    modifier.drmFormatModifierPlaneCount = uint32_t(layouts.size());
    modifier.pPlaneLayouts = layouts.data();
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external.pNext = &modifier;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.pNext = &external;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {uint32_t(width > 0 ? width : buffer.width), uint32_t(height > 0 ? height : buffer.height), 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage source = VK_NULL_HANDLE;
    VkResult r = vkCreateImage(device, &ici, nullptr, &source);
    if (r != VK_SUCCESS) {
        qCWarning(lcXr) << "dmabuf image:" << r << Graphics::drmFormatName(buffer.drmFormat) << Qt::hex << buffer.modifier;
        return false;
    }
    VkMemoryFdPropertiesKHR fdProperties{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    getMemoryFdProperties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, memoryFd, &fdProperties);
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, source, &requirements);
    const int32_t type = memoryType(requirements.memoryTypeBits & fdProperties.memoryTypeBits, 0);
    const int fd = fcntl(memoryFd, F_DUPFD_CLOEXEC, 0); // Vulkan owns it once imported
    if (type < 0 || fd < 0) {
        if (fd >= 0) {
            ::close(fd);
        }
        vkDestroyImage(device, source, nullptr);
        qCWarning(lcXr) << "dmabuf: no memory type to import into";
        return false;
    }
    VkImportMemoryFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import.fd = fd;
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.pNext = &import;
    dedicated.image = source;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &dedicated;
    mai.allocationSize = requirements.size;
    mai.memoryTypeIndex = uint32_t(type);
    VkDeviceMemory imported = VK_NULL_HANDLE;
    r = vkAllocateMemory(device, &mai, nullptr, &imported);
    if (r != VK_SUCCESS) {
        ::close(fd);
        vkDestroyImage(device, source, nullptr);
        qCWarning(lcXr) << "dmabuf import:" << r;
        return false;
    }
    vkBindImageMemory(device, source, imported, 0);
    image = source;
    memory = imported;
    return true;
}

std::optional<ImportedBuffer> XrCompositor::Impl::importBuffer(const BufferHandoff &buffer, VkFormat first, bool yuv)
{
    // P010 (HDR video from Firefox's VA-API decoder): luma, and the chroma plane half the size each way.
    ImportedBuffer imported;
    if (!importDmabuf(buffer, first, 0, yuv ? 1 : -1, 0, 0, imported.image, imported.memory)) {
        return std::nullopt;
    }
    if (yuv && !importDmabuf(buffer, VK_FORMAT_R16G16_UNORM, 1, 1, (buffer.width + 1) / 2, (buffer.height + 1) / 2, imported.chroma,
                             imported.chromaMemory)) {
        destroyImportNow(device, imported); // (any thread)
        return std::nullopt;
    }
    imported.format = first;
    imported.used = Clock::now();
    return imported;
}

bool XrCompositor::Impl::dmabufSources(const BufferHandoff &buffer, VkFormat format, bool yuv,
                                       std::vector<std::pair<VkImage, VkDeviceMemory>> &imports, VkImage &source, VkImage &chroma)
{
    source = chroma = VK_NULL_HANDLE;
    const VkFormat first = yuv ? VK_FORMAT_R16_UNORM : format;
    const auto importNow = [&]() {
        const Clock::time_point start = Clock::now();
        std::optional<ImportedBuffer> imported = importBuffer(buffer, first, yuv);
        frameImportMs += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        return imported;
    };
    const std::string key = importKey(buffer, first, yuv);
    if (key.empty()) {
        const std::optional<ImportedBuffer> once = importNow();
        if (!once) {
            return false;
        }
        dropImport(*once, &imports); // with the frame's copies
        source = once->image;
        chroma = once->chroma;
        return true;
    }
    auto it = importCache.find(key); // (the key has the formats it is shown with: another way is another import)
    if (it == importCache.end()) {
        const std::optional<ImportedBuffer> fresh = importNow();
        if (!fresh) {
            return false;
        }
        it = importCache.emplace(key, *fresh).first;
        if (importer) {
            importer->held(key);
        }
    }
    it->second.used = Clock::now();
    source = it->second.image;
    chroma = it->second.chroma;
    return true;
}

void XrCompositor::Impl::takeFromClient(VkImage image)
{
    // The client writes its buffer outside this device's queues (a foreign queue family): ours for this copy only.
    const uint32_t foreign = queueFamilyForeign ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
    barrier(image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT, foreign,
            queueFamily);
}

void XrCompositor::Impl::handBackToClient(VkImage image)
{
    const uint32_t foreign = queueFamilyForeign ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
    barrier(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT, 0, queueFamily,
            foreign);
}

void XrCompositor::Impl::dropImport(const ImportedBuffer &imported, std::vector<std::pair<VkImage, VkDeviceMemory>> *later)
{
    if (!later) {
        freeLater([device = device, imported] {
            destroyImportNow(device, imported);
        });
        return;
    }
    const std::pair<VkImage, VkDeviceMemory> parts[2] = {{imported.image, imported.memory}, {imported.chroma, imported.chromaMemory}};
    for (const auto &[image, memory] : parts) {
        if (image != VK_NULL_HANDLE) {
            later->emplace_back(image, memory);
        }
    }
}

void XrCompositor::Impl::destroyImportNow(VkDevice device, const ImportedBuffer &imported)
{
    const std::pair<VkImage, VkDeviceMemory> parts[2] = {{imported.image, imported.memory}, {imported.chroma, imported.chromaMemory}};
    for (const auto &[image, memory] : parts) {
        if (image != VK_NULL_HANDLE) {
            vkDestroyImage(device, image, nullptr);
        }
        if (memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, memory, nullptr);
        }
    }
}

void XrCompositor::Impl::freeLater(std::function<void()> free)
{
    if (importer) {
        importer->post(std::move(free));
    } else {
        free();
    }
}

void XrCompositor::Impl::freeImageLater(VkImage image, VkDeviceMemory memory)
{
    if (image == VK_NULL_HANDLE && memory == VK_NULL_HANDLE) {
        return;
    }
    freeLater([device = device, image, memory] {
        if (image != VK_NULL_HANDLE) {
            vkDestroyImage(device, image, nullptr);
        }
        if (memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, memory, nullptr);
        }
    });
}

void XrCompositor::Impl::pruneImports(const LayerBridge::Frame &taken)
{
    // The Importer's work first.
    if (importer) {
        for (auto &[key, result] : importer->collect()) {
            if (!result) {
                if (failedImports.size() >= kFailedImportsKept) {
                    failedImports.clear();
                }
                failedImports[key] = Clock::now();
                continue;
            }
            const auto [it, inserted] = importCache.try_emplace(key, *result);
            if (inserted) {
                it->second.used = Clock::now();
            } else {
                dropImport(*result, nullptr); // imported here meanwhile
            }
        }
    }
    // Imports are kept by the memory they show, which outlives a client's buffer object (another one, Firefox's after a
    // seek, may show it): a client's buffer gone takes none with it; unused ones go below. Frames not taken for long
    // (the bridge stopped listing what went): all of them.
    // Each import let go: the Importer may make it again.
    const auto release = [this](const std::string &key, const ImportedBuffer &imported) {
        dropImport(imported, nullptr);
        if (importer) {
            importer->released(key);
        }
    };
    if (taken.dropAllImports) {
        failedImports.clear();
        for (const auto &[key, imported] : importCache) {
            release(key, imported);
        }
        importCache.clear();
    }
    const Clock::time_point now = Clock::now();
    std::erase_if(importCache, [&](const auto &entry) {
        if (now - entry.second.used < importKeptUnused) {
            return false;
        }
        release(entry.first, entry.second);
        return true;
    });
    while (importCache.size() > kImportsKept) {
        auto oldest = std::min_element(importCache.begin(), importCache.end(), [](const auto &a, const auto &b) {
            return a.second.used < b.second.used;
        });
        release(oldest->first, oldest->second);
        importCache.erase(oldest);
    }
}

VkImage XrCompositor::Impl::uploadShm(SurfaceLayer &layer, const BufferHandoff &buffer, VkFormat format)
{
    const VkDeviceSize size = VkDeviceSize(buffer.pixels.size());
    if (layer.stagingSize < size) {
        if (layer.staging != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, layer.staging, nullptr);
            vkFreeMemory(device, layer.stagingMemory, nullptr);
            layer.staging = VK_NULL_HANDLE;
            layer.stagingSize = 0;
        }
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        vkCreateBuffer(device, &bci, nullptr, &layer.staging);
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, layer.staging, &requirements);
        const int32_t type = memoryType(requirements.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = requirements.size;
        mai.memoryTypeIndex = uint32_t(type);
        if (type < 0 || vkAllocateMemory(device, &mai, nullptr, &layer.stagingMemory) != VK_SUCCESS) {
            vkDestroyBuffer(device, layer.staging, nullptr);
            layer.staging = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
        vkBindBufferMemory(device, layer.staging, layer.stagingMemory, 0);
        vkMapMemory(device, layer.stagingMemory, 0, VK_WHOLE_SIZE, 0, &layer.stagingMapped);
        layer.stagingSize = size;
    }
    std::memcpy(layer.stagingMapped, buffer.pixels.constData(), size_t(size));

    // The pixels go into a device image the pass samples (kept with the layer, remade when the buffer changes).
    if (layer.sourceImage == VK_NULL_HANDLE || layer.sourceFormat != format || layer.sourceWidth != buffer.width
        || layer.sourceHeight != buffer.height) {
        if (layer.sourceImage != VK_NULL_HANDLE) {
            vkDestroyImage(device, layer.sourceImage, nullptr);
            vkFreeMemory(device, layer.sourceMemory, nullptr);
        }
        if (!createImage(format, buffer.width, buffer.height, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         layer.sourceImage, layer.sourceMemory)) {
            return VK_NULL_HANDLE;
        }
        layer.sourceFormat = format;
        layer.sourceWidth = buffer.width;
        layer.sourceHeight = buffer.height;
    }
    barrier(layer.sourceImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.bufferRowLength = uint32_t(buffer.stride) / bytesPerPixel(format);
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {uint32_t(buffer.width), uint32_t(buffer.height), 1};
    vkCmdCopyBufferToImage(cmd, layer.staging, layer.sourceImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(layer.sourceImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    return layer.sourceImage;
}

bool XrCompositor::Impl::recordCopy(SurfaceLayer &layer, const BufferHandoff &buffer, VkImage swapchainImage,
                                    const Target &target, const SurfaceGeometry &geometry, float radius,
                                    std::vector<std::pair<VkImage, VkDeviceMemory>> &imports)
{
    const bool yuv = (target.flags & kYuvBt2020) != 0 && buffer.kind == BufferHandoff::Kind::Dmabuf && buffer.planes.size() >= 2;
    const bool client = buffer.kind == BufferHandoff::Kind::Dmabuf;
    VkImage source = VK_NULL_HANDLE;
    VkImage chroma = VK_NULL_HANDLE; // P010 (HDR video from Firefox's VA-API decoder): its chroma plane
    if (!client) {
        source = uploadShm(layer, buffer, target.source);
    } else if (!dmabufSources(buffer, target.source, yuv, imports, source, chroma)) {
        return false;
    }
    if (source == VK_NULL_HANDLE || (yuv && chroma == VK_NULL_HANDLE)) {
        return false;
    }
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = descriptorPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &setLayout;
    if (vkAllocateDescriptorSets(device, &dsai, &set) != VK_SUCCESS) {
        return false;
    }
    const VkImageView sourceView = view(source, yuv ? VK_FORMAT_R16_UNORM : target.source, VK_IMAGE_USAGE_SAMPLED_BIT);
    const VkImageView chromaView = yuv ? view(chroma, VK_FORMAT_R16G16_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT) : sourceView;
    const VkImageView passView = view(layer.passImage, target.storage, VK_IMAGE_USAGE_STORAGE_BIT);
    if (sourceView == VK_NULL_HANDLE || chromaView == VK_NULL_HANDLE || passView == VK_NULL_HANDLE) {
        return false;
    }
    const VkDescriptorImageInfo sourceInfo{sampler, sourceView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo chromaInfo{yuv ? chromaSampler : sampler, chromaView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo passInfo{VK_NULL_HANDLE, passView, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[3] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                                      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &sourceInfo;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &passInfo;
    writes[2].dstSet = set;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].pImageInfo = &chromaInfo;
    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

    // The pass writes the scratch image (storage), which is copied as it is into the swapchain image: the runtime's
    // sRGB swapchains cannot be storage images on NVIDIA, and its images are made without extended usage.
    barrier(layer.passImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, target.floatOutput ? pipelineRgba16f : pipelineRgba8);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
    PassParams params{{buffer.width, buffer.height},
                            {geometry.x, geometry.y},
                            {geometry.windowWidth, geometry.windowHeight},
                            radius,
                            target.flags,
                            uint32_t(target.decode.curve),
                            target.decode.exponent,
                            target.decode.gain,
                            (target.decode.convert ? 1u : 0u) | (target.decode.encodeSrgb ? 2u : 0u),
                            {}};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            params.m[row * 4 + col] = target.decode.matrix[size_t(row * 3 + col)];
        }
        params.m[row * 4 + 3] = target.decode.luma[size_t(row)];
    }
    // The corners are cut in window pixels: a surface's buffer may be shown at another size (Firefox's video:
    // only a buffer-sized part of a fullscreen video was left, the rest cut away as outside the window).
    params.scale[0] = geometry.width > 0 && buffer.width > 0 ? float(geometry.width) / float(buffer.width) : 1.0f;
    params.scale[1] = geometry.height > 0 && buffer.height > 0 ? float(geometry.height) / float(buffer.height) : 1.0f;
    // P010 HDR video: YUV->RGB overshoots where the half-size chroma meets a bright edge (a sun on black), and PQ is so
    // steep at the top that a little more code is thousands of nits (a halo around the sun; a frame
    // reached 5079 nits with MaxCLL 1000). Kept within the content's own peak, as a display's own peak clips it.
    if ((target.flags & kYuvBt2020) != 0 && buffer.plan.hdr.transfer == Color::LayerTransfer::Pq) {
        const float peak = std::max(buffer.plan.hdr.maxCllNits, buffer.plan.hdr.masteringMaxNits);
        if (peak > 0.0f) {
            const double y = std::pow(std::min(double(peak), 10000.0) / 10000.0, 2610.0 / 16384.0);
            params.yuvMax = float(std::pow((3424.0 / 4096.0 + 2413.0 / 128.0 * y) / (1.0 + 2392.0 / 128.0 * y), 2523.0 / 32.0));
        }
    }
    vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
    // The client's images are ours only around the pass (taken here, after every step that can fail).
    if (client) {
        takeFromClient(source);
        if (chroma != VK_NULL_HANDLE) {
            takeFromClient(chroma);
        }
    }
    vkCmdDispatch(cmd, uint32_t(buffer.width + 15) / 16, uint32_t(buffer.height + 15) / 16, 1);
    if (client) {
        handBackToClient(source);
        if (chroma != VK_NULL_HANDLE) {
            handBackToClient(chroma);
        }
    }
    // The pass image stays in GENERAL: the full-size picture, kept for the filtered copies.
    barrier(layer.passImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    return recordShow(layer, swapchainImage);
}

/*!
 * The layer's picture into its swapchain image: the full-size image as it is, or at its level an
 * area-filtered copy in the image's top-left corner (one more row and column repeat its edge). From the pass image,
 * which holds the newest full-size picture in GENERAL layout.
 */
bool XrCompositor::Impl::recordShow(SurfaceLayer &layer, VkImage swapchainImage)
{
    const bool up = layer.level == Spatial::Sharpness::kUpLevel && !sharpOff;
    const int shownWidth = Spatial::Sharpness::shownSize(layer.width, layer.level);
    const int shownHeight = Spatial::Sharpness::shownSize(layer.height, layer.level);
    const bool smaller = layer.level > 0 && !sharpOff && shownWidth < layer.width && shownHeight < layer.height;
    if ((up && (layer.swapWidth < shownWidth || layer.swapHeight < shownHeight))
        || (!up && (layer.swapWidth < layer.width || layer.swapHeight < layer.height))) {
        return false; // the swapchain is not this level's (made for it before the image is acquired)
    }
    VkImage source = layer.passImage;
    uint32_t copyWidth = uint32_t(layer.width);
    uint32_t copyHeight = uint32_t(layer.height);
    if (up || smaller) {
        // The copy: the full-size picture's size when smaller (its top-left part used), twice it when enlarged.
        const int imageWidth = up ? shownWidth : layer.width;
        const int imageHeight = up ? shownHeight : layer.height;
        if (layer.scaledImage != VK_NULL_HANDLE && (layer.scaledWidth != imageWidth || layer.scaledHeight != imageHeight)) {
            freeImageLater(layer.scaledImage, layer.scaledMemory); // (once a frame a layer: not in this frame's copies)
            layer.scaledImage = VK_NULL_HANDLE;
            layer.scaledMemory = VK_NULL_HANDLE;
        }
        if (layer.scaledImage == VK_NULL_HANDLE) {
            if (!createImage(layer.storage, imageWidth, imageHeight, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                             layer.scaledImage, layer.scaledMemory)) {
                layer.scaledImage = VK_NULL_HANDLE;
                return false;
            }
            layer.scaledWidth = imageWidth;
            layer.scaledHeight = imageHeight;
        }
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = descriptorPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &downsampleSetLayout;
        const VkImageView masterView = view(layer.passImage, layer.storage, VK_IMAGE_USAGE_STORAGE_BIT);
        const VkImageView scaledView = view(layer.scaledImage, layer.storage, VK_IMAGE_USAGE_STORAGE_BIT);
        if (vkAllocateDescriptorSets(device, &dsai, &set) != VK_SUCCESS || masterView == VK_NULL_HANDLE || scaledView == VK_NULL_HANDLE) {
            return false;
        }
        const VkDescriptorImageInfo infos[2] = {{VK_NULL_HANDLE, masterView, VK_IMAGE_LAYOUT_GENERAL},
                                                {VK_NULL_HANDLE, scaledView, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet writes[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        for (uint32_t i = 0; i < 2; ++i) {
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
        // The values' encoding, for filtering in linear light: 8-bit sRGB, PQ, or linear already.
        // PQ (HDR video) is filtered in its own encoding, not in linear light: averaging a 1000 cd/m² sun with the black
        // around it in linear light spread its light into a ring that the runtime's mapping onto the CV1 showed as a halo
        // (the frame itself was clean). Video scalers work in the encoded values too.
        const uint32_t encoding = layer.format == VK_FORMAT_R8G8B8A8_SRGB ? 1u : 0u;
        const DownsampleParams params{{layer.width, layer.height}, {shownWidth, shownHeight}, encoding, up ? 1u : 0u};
        barrier(layer.scaledImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layer.storage == VK_FORMAT_R16G16B16A16_SFLOAT ? downsampleRgba16f : downsampleRgba8);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, downsamplePipelineLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, downsamplePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
        vkCmdDispatch(cmd, uint32_t(shownWidth + 1 + 15) / 16, uint32_t(shownHeight + 1 + 15) / 16, 1);
        barrier(layer.scaledImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        source = layer.scaledImage;
        copyWidth = uint32_t(up ? shownWidth : shownWidth + 1);
        copyHeight = uint32_t(up ? shownHeight : shownHeight + 1);
    }
    barrier(swapchainImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {copyWidth, copyHeight, 1};
    vkCmdCopyImage(cmd, source, VK_IMAGE_LAYOUT_GENERAL, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    // A swapchain bigger than the picture: its last column and row once more beside it, so the runtime's bilinear
    // sampling at the shown part's edge never reaches pixels of an older picture.
    const auto again = [&](int32_t fromX, int32_t fromY, int32_t toX, int32_t toY, uint32_t w, uint32_t h) {
        VkImageCopy edge = region;
        edge.srcOffset = {fromX, fromY, 0};
        edge.dstOffset = {toX, toY, 0};
        edge.extent = {w, h, 1};
        vkCmdCopyImage(cmd, source, VK_IMAGE_LAYOUT_GENERAL, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &edge);
    };
    const bool wider = uint32_t(layer.swapWidth) > copyWidth;
    const bool taller = uint32_t(layer.swapHeight) > copyHeight;
    if (wider) {
        again(int32_t(copyWidth) - 1, 0, int32_t(copyWidth), 0, 1, copyHeight);
    }
    if (taller) {
        again(0, int32_t(copyHeight) - 1, 0, int32_t(copyHeight), copyWidth, 1);
    }
    if (wider && taller) {
        again(int32_t(copyWidth) - 1, int32_t(copyHeight) - 1, int32_t(copyWidth), int32_t(copyHeight), 1, 1);
    }
    // OpenXR wants a colour swapchain image back in COLOR_ATTACHMENT_OPTIMAL.
    barrier(swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
    if (!up && !smaller) {
        layer.level = 0;
    }
    layer.shownWidth = up || smaller ? shownWidth : layer.width;
    layer.shownHeight = up || smaller ? shownHeight : layer.height;
    return true;
}

void XrCompositor::Impl::frame(LayerBridge &bridge, const Options &options)
{
    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    if (!check(xrWaitFrame(session, &waitInfo, &frameState), "xrWaitFrame")) {
        return;
    }
    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    if (!check(xrBeginFrame(session, &beginInfo), "xrBeginFrame")) {
        return;
    }
    const auto msSince = [](Clock::time_point from) {
        return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
    };
    const Clock::time_point frameStart = Clock::now();

    LayerBridge::Frame taken = bridge.take();
    bridgeForStatus = &bridge;
    const Clock::time_point destroyStart = Clock::now();
    for (uint64_t id : taken.destroyed) {
        destroyLayer(id);
    }
    pruneImports(taken); // the last frame's copies are done (waited for): no import is in use
    destroyTime.add(msSince(destroyStart));
    const Clock::time_point spareStart = Clock::now();
    pruneSpareSwapchains(false);
    spareTime.add(msSince(spareStart));
    frameImportMs = 0.0;
    double imageWaitMs = 0.0;
    double recordMs = 0.0;
    if (!visualsUploaded) {
        uploadVisuals(taken);
    }
    std::erase_if(retiredEnvironmentJobs, [](const std::shared_ptr<EnvironmentJob> &job) {
        std::lock_guard lock(job->mutex);
        return job->finished;
    });
    if (environmentJob) {
        // A room built from a new photo (Settings): shown from this frame.
        std::lock_guard lock(environmentJob->mutex);
        if (environmentJob->done) {
            if (!environmentJob->room.isNull()) {
                environmentPanorama = environmentJob->panorama;
                destroyLayer(kEnvironmentLayer); // its size or kind may differ
                taken.buffers[kEnvironmentLayer] = roomBuffer(environmentJob->room);
            }
            environmentJob->done = false;
            environmentJob->room = RoomImage();
        }
    }
    takePaintedArt();
    for (auto &[id, art] : pendingArt) {
        taken.buffers[id] = std::move(art);
    }
    pendingArt.clear();
    sizeStates.clear();
    for (const WindowPlacement &window : taken.windows) {
        sizeStates[window.windowId] = {window.fullscreen, window.maximized};
        // The app's own minimize button: hidden in the room (Home's running apps bring it back).
        int &seen = minimizeSeen[window.windowId];
        if (window.minimizeRequests > seen) {
            seen = window.minimizeRequests;
            layout.setMinimized(window.windowId, true);
            qCInfo(lcXr) << "window" << window.windowId << "minimized (its own button)";
        }
    }
    // Home's running apps: the open windows, in the order they opened.
    runningWindows.clear();
    for (const WindowPlacement &window : taken.windows) {
        const auto placed = layout.windows().find(window.windowId);
        runningWindows.push_back({window.windowId, QString::fromStdString(window.title), window.appId,
                           placed != layout.windows().end() && placed->second.minimized});
    }
    std::sort(runningWindows.begin(), runningWindows.end(), [](const RunningWindow &a, const RunningWindow &b) {
        return a.windowId < b.windowId;
    });
    // Then the other VR apps connected to the runtime: a card each, brought in front when tapped.
    if (runtimeApps) {
        for (const VrApp &app : runtimeApps->apps()) {
            runningWindows.push_back({0, QString::fromStdString(app.name), std::string(), false, app.id, true});
        }
    }

    // Copies: only buffers the client's GPU has finished (never wait for a client).
    std::vector<std::unique_ptr<BufferHandoff>> done;
    std::vector<std::pair<VkImage, VkDeviceMemory>> imports;
    std::vector<XrSwapchain> acquired;
    bool recording = false;
    // Windows out of view (minimised, hidden behind a film in cinema, faded out under Home): their newest buffer waits,
    // not copied, until they show again (many copies at once cost a frame now and then).
    std::set<uint64_t> outOfView;
    const int cinemaNow = layout.cinemaWindow();
    for (const WindowPlacement &window : taken.windows) {
        const auto placed = layout.windows().find(window.windowId);
        const bool hidden = (placed != layout.windows().end() && placed->second.minimized) || homeFade > 0.99f
            || (cinemaNow && !shownInCinema(window.windowId) && cinemaFade > 0.99f);
        if (hidden && !options.viewLocked) {
            for (const SurfacePlacement &surface : window.surfaces) {
                outOfView.insert(surface.surfaceId);
            }
        }
    }
    // Where each surface sits in its window's visible geometry: the pass rounds the window's corners there.
    std::map<uint64_t, SurfaceGeometry> geometries;
    for (const WindowPlacement &window : taken.windows) {
        for (const SurfacePlacement &surface : window.surfaces) {
            if (surface.clipped) {
                geometries[surface.surfaceId] = {surface.x, surface.y, window.width, window.height, surface.width, surface.height};
            }
        }
    }
    std::set<uint64_t> copiedNow;
    // At most two new swapchains a frame (each costs the runtime 1-14 ms; Home opening asked for 10-12 at once and its
    // frame took 20-35 ms): the others' pictures wait a frame, as a picture whose client GPU work is not done.
    constexpr int kNewSwapchainsAFrame = 2;
    const int swapchainsBefore = swapchainCount;
    for (auto &[id, buffer] : taken.buffers) {
        if (outOfView.contains(id) && layers.contains(id) && layers.at(id).hasImage) {
            ++skipped;
            bridge.putBack(id, std::move(buffer)); // the newest waits (an older one is let go)
            continue;
        }
        if (buffer->acquireFence >= 0 && !fenceSignalled(buffer->acquireFence)) {
            ++deferred;
            bridge.putBack(id, std::move(buffer));
            continue;
        }
        const std::optional<Target> target = chooseTarget(buffer->plan, buffer->drmFormat, swapchainFormats);
        if (!target) {
            ++notShown;
            if (warned.insert(id).second) {
                qCWarning(lcXr).noquote() << "surface" << id << "not shown yet:" << Graphics::drmFormatName(buffer->drmFormat)
                                          << buffer->plan.toString();
            }
            done.push_back(std::move(buffer));
            continue;
        }
        // Memory not imported yet (a new window's buffers, a new video's frames): imported on the Importer's thread
        // (asked for when the app attached it, or now), its picture waits a frame or two (as above). Without the
        // Importer: here, past kImportBudgetMs of imports in a frame the others wait. An import never makes a frame late.
        if (buffer->kind == BufferHandoff::Kind::Dmabuf) {
            const bool yuv = (target->flags & kYuvBt2020) != 0 && buffer->planes.size() >= 2;
            const std::string key = importKey(*buffer, yuv ? VK_FORMAT_R16_UNORM : target->source, yuv);
            if (!key.empty() && !importCache.contains(key)) {
                if (const auto failed = failedImports.find(key); failed != failedImports.end()) {
                    if (Clock::now() - failed->second < kImportRetry) {
                        ++notShown;
                        done.push_back(std::move(buffer));
                        continue;
                    }
                    failedImports.erase(failed);
                }
                if (importer || frameImportMs >= kImportBudgetMs) {
                    if (importer) {
                        importer->request(*buffer);
                    }
                    ++deferred;
                    bridge.putBack(id, std::move(buffer));
                    continue;
                }
            }
        }
        SurfaceLayer &layer = layers[id];
        const bool fresh = layer.swapchain == XR_NULL_HANDLE;
        layer.level = Spatial::Sharpness::nextLevel(layer.level, layer.wantLevel); // its swapchain's size follows it
        if (swapchainCount - swapchainsBefore >= kNewSwapchainsAFrame && !swapchainFits(layer, target->swapchain, buffer->width, buffer->height)) {
            ++deferred;
            bridge.putBack(id, std::move(buffer));
            continue;
        }
        if (!ensureSwapchain(layer, *target, buffer->width, buffer->height)) {
            done.push_back(std::move(buffer));
            continue;
        }
        if (fresh) {
            qCInfo(lcXr).noquote() << "surface" << id << "layer:" << QStringLiteral("%1x%2").arg(buffer->width).arg(buffer->height)
                                   << buffer->plan.toString() << "| swapchain VkFormat" << int(target->swapchain)
                                   << "| pass" << (target->floatOutput ? "rgba16f" : "rgba8")
                                   << ((target->flags & kOpaque) ? "opaque" : "");
        }
        if (!recording) {
            vkResetCommandBuffer(cmd, 0);
            vkResetDescriptorPool(device, descriptorPool, 0);
            VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &cbbi);
            recording = true;
        }
        uint32_t index = 0;
        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        const Clock::time_point acquiring = Clock::now();
        if (!check(xrAcquireSwapchainImage(layer.swapchain, &acquireInfo, &index), "xrAcquireSwapchainImage")) {
            done.push_back(std::move(buffer));
            continue;
        }
        XrSwapchainImageWaitInfo imageWait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        imageWait.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(layer.swapchain, &imageWait);
        imageWaitMs += msSince(acquiring);
        acquired.push_back(layer.swapchain);

        // Rounded corners on window surfaces (not on popups, beams, cursors; not in the view-locked exactness check).
        const auto geometry = geometries.find(id);
        const float radius = geometry == geometries.end() || options.viewLocked ? 0.0f : options.cornerRadius;
        if (layer.hasImage && layer.hdr.transfer != buffer->plan.hdr.transfer) {
            // What the runtime is told changes (a video's first frames untagged, then PQ): its HDR layer, and the Rift
            // panel's "HDR content", follow the transfer sent here.
            qCInfo(lcXr).noquote() << "surface" << id << "now" << buffer->plan.toString();
        }
        layer.hdr = buffer->plan.hdr; // the resample pass decodes by it
        copiedNow.insert(id);
        const Clock::time_point recordStart = Clock::now();
        const bool recorded = recordCopy(layer, *buffer, layer.images[index], *target,
                                         geometry == geometries.end() ? SurfaceGeometry{} : geometry->second, radius, imports);
        recordMs += msSince(recordStart);
        if (recorded) {
            layer.hasImage = true;
            layer.alpha = radius > 0.0f || !(target->flags & kOpaque);
            ++copies;
        } else {
            ++notShown;
        }
        done.push_back(std::move(buffer));
    }
    // Windows that came nearer or went farther without a new buffer: their filtered copy made again from the full-size
    // picture, a few a frame.
    int rescalesNow = 0;
    for (auto &[id, layer] : layers) {
        if (rescalesNow >= 4 || !layer.hasImage || copiedNow.contains(id) || outOfView.contains(id) || layer.passImage == VK_NULL_HANDLE) {
            continue;
        }
        const int next = Spatial::Sharpness::nextLevel(layer.level, layer.wantLevel);
        if (next == layer.level) {
            continue;
        }
        // Into or out of the enlarged copy: a swapchain of the other size (the full-size picture stays).
        const int previous = layer.level;
        layer.level = next;
        const Target same{VK_FORMAT_UNDEFINED, layer.format, layer.storage, layer.storage == VK_FORMAT_R16G16B16A16_SFLOAT, 0, {}};
        if (!ensureSwapchain(layer, same, layer.width, layer.height)) {
            layer.level = previous;
            continue;
        }
        if (!recording) {
            vkResetCommandBuffer(cmd, 0);
            vkResetDescriptorPool(device, descriptorPool, 0);
            VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &cbbi);
            recording = true;
        }
        uint32_t index = 0;
        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        const Clock::time_point acquiring = Clock::now();
        if (!check(xrAcquireSwapchainImage(layer.swapchain, &acquireInfo, &index), "xrAcquireSwapchainImage")) {
            continue;
        }
        XrSwapchainImageWaitInfo imageWait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        imageWait.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(layer.swapchain, &imageWait);
        imageWaitMs += msSince(acquiring);
        acquired.push_back(layer.swapchain);
        const Clock::time_point recordStart = Clock::now();
        recordShow(layer, layer.images[index]);
        recordMs += msSince(recordStart);
        ++rescalesNow;
        ++rescaled;
    }
    const Clock::time_point copySubmit = Clock::now();
    if (recording) {
        vkEndCommandBuffer(cmd);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        vkQueueSubmit(queue, 1, &submit, fence);
        // Copies are small; waiting here lets the clients have their buffers back at once.
        vkWaitForFences(device, 1, &fence, VK_TRUE, 1'000'000'000ull);
        vkResetFences(device, 1, &fence);
    }
    copyWaitTime.add(msSince(copySubmit));
    const Clock::time_point finishStart = Clock::now();
    for (VkImageView v : frameViews) {
        vkDestroyImageView(device, v, nullptr);
    }
    frameViews.clear();
    for (XrSwapchain swapchain : acquired) {
        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(swapchain, &releaseInfo);
    }
    for (auto &[image, memory] : imports) {
        vkDestroyImage(device, image, nullptr);
        vkFreeMemory(device, memory, nullptr);
    }
    for (auto &buffer : done) {
        bridge.release(std::move(buffer));
    }
    finishTime.add(msSince(finishStart));
    imageWaitTime.add(imageWaitMs);
    importTime.add(frameImportMs);
    recordTime.add(recordMs);
    copyTime.add(msSince(frameStart));

    // Where the head is, the windows in the room, and what the controllers do with them.
    const XrTime displayTime = frameState.predictedDisplayTime;
    const float dt = lastDisplayTime ? std::clamp(float(displayTime - lastDisplayTime) * 1e-9f, 0.0f, 0.1f) : 0.011f;
    lastDisplayTime = displayTime;
    XrSpaceLocation headLocation{XR_TYPE_SPACE_LOCATION};
    const bool headValid = XR_SUCCEEDED(xrLocateSpace(viewSpace, space, displayTime, &headLocation))
        && (headLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
    if (headValid) {
        roomHead = toPose(headLocation.pose);
    }
    if (reclineProgress < 1.0f) {
        reclineProgress = std::min(1.0f, reclineProgress + dt / Spatial::kReclineSeconds);
        recline = Spatial::reclineBlend(reclineFrom, reclineTarget, Spatial::reclineEase(reclineProgress), layout.home().position);
    }
    head = recline.inverse() * roomHead;
    if (options.deskView) {
        head = Spatial::Pose{}; // the room laid out around the headset's own view (its quads are in VIEW space)
    }
    updateLensTest(displayTime);
    if (sharpDensity <= 0.0f && !sharpOff && eyeWidth > 0 && headValid) {
        // Pixels a radian at the lens centre: the recommended width over the views' tangent width.
        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
        locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime = displayTime;
        locate.space = viewSpace;
        XrViewState viewState{XR_TYPE_VIEW_STATE};
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t located = 0;
        if (XR_SUCCEEDED(xrLocateViews(session, &locate, &viewState, 2, &located, views)) && located > 0) {
            float tangents = 0.0f;
            for (uint32_t v = 0; v < located; ++v) {
                tangents += std::tan(views[v].fov.angleRight) - std::tan(views[v].fov.angleLeft);
            }
            tangents /= float(located);
            if (tangents > 0.1f) {
                const float native = float(eyeWidth) / tangents;
                sharpDensity = sharpFactor * native;
                qCInfo(lcXr).noquote() << QStringLiteral("sharp at a distance: the runtime's 1:1 density %1 px a radian (%2 px wide over %3), "
                                                         "windows shown denser than %4 get a filtered copy")
                                              .arg(native, 0, 'f', 0).arg(eyeWidth).arg(tangents, 0, 'f', 3).arg(sharpDensity, 0, 'f', 0);
            }
        }
    }
    if (headValid) {
        headHistory.push_back(head);
        if (headHistory.size() > 45) {
            headHistory.erase(headHistory.begin());
        }
    }
    if (recentreFrames > 0) {
        --recentreFrames;
    } else if (recentreFrames == 0) {
        recentreFrames = -1;
        const std::optional<Spatial::Pose> oldToNew = Spatial::recentreFromHeads(headHistory);
        layout.recentered(oldToNew);
        headHistory.clear();
        if (reclined) {
            recline = reclineFrom = reclineTarget = Spatial::reclineTo(layout.home(), roomHead);
            reclineProgress = 1.0f;
            head = recline.inverse() * roomHead;
        }
        if (oldToNew) {
            qCInfo(lcXr).noquote() << QStringLiteral("recentered: the windows come to the new front, pinned ones stay (LOCAL moved %1 m, turned %2 degrees)")
                                          .arg(oldToNew->position.length(), 0, 'f', 2)
                                          .arg(2.0f * std::atan2(oldToNew->orientation.y, oldToNew->orientation.w) * 180.0f / float(M_PI), 0, 'f', 1);
        } else {
            qCInfo(lcXr) << "recentered: the windows come to the new front (LOCAL did not measurably move)";
        }
    }
    // Home and cinema fade in and out over a quarter second.
    const auto ease = [dt](float &value, float target) {
        const float step = dt / 0.25f;
        value = target > value ? std::min(target, value + step) : std::max(target, value - step);
    };
    ease(homeFade, launcherOpen ? 1.0f : 0.0f);
    ease(cinemaFade, layout.cinemaWindow() ? 1.0f : 0.0f);
    if (!options.viewLocked) {
        std::vector<Spatial::WindowInfo> infos;
        for (const WindowPlacement &window : taken.windows) {
            infos.push_back({window.windowId, window.appId, window.width, window.height, window.fullscreen, window.pixelScale});
        }
        // The windows a session starts with line up side by side; later ones open in front of the user.
        if (focused && !firstFocusAt) {
            firstFocusAt = Clock::now();
        }
        layout.setOpenInFront(firstFocusAt && Clock::now() - *firstFocusAt > std::chrono::seconds(6));
        std::map<int, Spatial::Vec3> placedBefore;
        for (const auto &[id, window] : layout.windows()) {
            placedBefore[id] = window.pose.position;
        }
        layout.sync(infos, head);
        // Windows the layout moved by itself (back where they were once the window that pushed them aside went, out of
        // fullscreen): logged, as the moves aside are (makeMain()).
        for (const auto &[id, window] : layout.windows()) {
            const auto was = placedBefore.find(id);
            if (was != placedBefore.end() && (window.pose.position - was->second).length() > 0.01f) {
                const Spatial::Vec3 relative = window.pose.position - head.position;
                qCInfo(lcXr).noquote() << QStringLiteral("window %1 moved by the layout to %2 degrees, %3 m (back where it was, or out of fullscreen)")
                                              .arg(id)
                                              .arg(Spatial::azimuthOf(relative) * 180.0f / float(M_PI), 0, 'f', 1)
                                              .arg(std::hypot(relative.x, relative.z), 0, 'f', 2);
            }
        }
        // Settings' window in front whenever a new one appears: started from the dock, or the session's password
        // agent showing a prompt (a hidden Qt window comes back as a new one).
        if (const int window = settingsWindow(); window && (settingsToFront || window != settingsSeen)) {
            makeMain(window, "Settings in front");
            settingsToFront = false;
            settingsSeen = window;
        } else if (!window) {
            settingsSeen = 0;
        }
        // The keyboard by itself when the user taps a text field (if the setting says so): a text field focused in
        // the window last clicked (apps focus their fields when they start: Konsole's terminal is one); gone when left.
        bool wants = false;
        for (const WindowPlacement &window : taken.windows) {
            wants |= window.wantsKeyboard && window.windowId == lastClickedWindow;
        }
        if (options.keyboardShowOnTextField) {
            if (wants) {
                textFieldLeftAt.reset();
                if (!keyboardWanted && !keyboardOpen) {
                    openKeyboard(true, Spatial::azimuthOf(head.forward()));
                }
            } else if (keyboardWanted) {
                textFieldLeftAt = Clock::now();
            }
            if (!wants && textFieldLeftAt && Clock::now() - *textFieldLeftAt > std::chrono::milliseconds(1200)) {
                textFieldLeftAt.reset();
                if (keyboardOpen && keyboardAuto) {
                    closeKeyboard(bridge);
                }
            }
        }
        keyboardWanted = wants;
        layoutChrome();
        takeNotifications(bridge);
        interact(bridge, displayTime, dt);
        layout.updateFollow(head, dt);
        updateChromeArt();
        saveLayout(false);
    }

    // Layers: every surface of every window, on the window's plane, bottom to top; then beams and cursors.
    // Every layer that can be added (the vectors below must not grow: headers and next pointers point into them):
    // surfaces, frames, keyboard (with its bar and two highlights), home (and its plate), the dock (and two highlights),
    // two beams and two cursors.
    size_t count = 32 + chrome.size() * kChromeParts;
    for (const WindowPlacement &window : taken.windows) {
        count += window.surfaces.size();
    }
    shotLayers.clear();
    std::vector<XrCompositionLayerQuad> quads;
    std::vector<XrCompositionLayerCylinderKHR> cylinders; //!< curved windows' surfaces and bars
    std::vector<XrCompositionLayerHdrMNDX> hdrs;
    std::vector<const XrCompositionLayerBaseHeader *> headers;
    quads.reserve(count); // stable addresses: the headers and next pointers point into these
    cylinders.reserve(count);
    hdrs.reserve(count);
    float quadFade = 1.0f; //!< the layers added now are this visible (colour scale; hidden without the extension below 0.5)
    std::vector<XrCompositionLayerColorScaleBiasKHR> scales;
    scales.reserve(count + 2);
    std::vector<XrCompositionLayerBackdropBlurMNDX> blurs; //!< frosted glass behind the glass layers
    blurs.reserve(count);
    const auto canAdd = [&]() {
        return headers.size() < maxLayers && quadFade >= 0.01f && (colorScaleLayers || quadFade >= 0.5f);
    };
    // The structures chained on a layer: its HDR description, its fade, and on glass (the dock, the keyboard, the frame's
    // menu, Settings' window) the runtime's backdrop blur (RiftLinux: at most 4 blurred layers a view).
    XrCompositionLayerBackdropBlurMNDX chainedBlur{XR_TYPE_COMPOSITION_LAYER_BACKDROP_BLUR_MNDX}; //!< the blur chain() put on the layer it chained last (for the screenshot; none: radians 0)
    const auto chain = [&](const SurfaceLayer &layer, bool glass) -> const void * {
        chainedBlur = {XR_TYPE_COMPOSITION_LAYER_BACKDROP_BLUR_MNDX};
        const void *next = nullptr;
        const void **tail = &next;
        if (hdrExtension) {
            XrCompositionLayerHdrMNDX hdr{XR_TYPE_COMPOSITION_LAYER_HDR_MNDX};
            hdr.transfer = XrHdrTransferMNDX(layer.hdr.transfer);
            hdr.primaries = XrHdrPrimariesMNDX(layer.hdr.primaries);
            hdr.referenceWhiteNits = layer.hdr.referenceWhiteNits;
            hdr.masteringMinNits = layer.hdr.masteringMinNits;
            hdr.masteringMaxNits = layer.hdr.masteringMaxNits;
            hdr.maxContentLightLevelNits = layer.hdr.maxCllNits;
            hdr.maxFrameAverageLightLevelNits = layer.hdr.maxFallNits;
            hdrs.push_back(hdr);
            *tail = &hdrs.back();
            tail = &hdrs.back().next;
        }
        if (colorScaleLayers && quadFade < 0.999f) {
            // Premultiplied alpha: colour and alpha scaled together fade the layer out.
            XrCompositionLayerColorScaleBiasKHR fade{XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR};
            fade.colorScale = {quadFade, quadFade, quadFade, quadFade};
            fade.colorBias = {0.0f, 0.0f, 0.0f, 0.0f};
            scales.push_back(fade);
            *tail = &scales.back();
            tail = &scales.back().next;
        }
        // At most 4 blurred layers a view (RiftLinux); more glass stays clear glass (bottom to top: the windows'
        // first, the dock and cards last).
        if (glass && blurLayers && layer.alpha && quadFade > 0.95f && blurs.size() < kMaxBlurredLayers) {
            // Frosted: about a degree of blur, the colours a little livelier, a breath of the style's tint; full blur from
            // half the glass's alpha, none where it is clear (rounded corners stay sharp).
            const bool light = options.frameTheme == FrameArt::Theme::Light;
            XrCompositionLayerBackdropBlurMNDX blur{XR_TYPE_COMPOSITION_LAYER_BACKDROP_BLUR_MNDX};
            blur.blurRadians = 0.018f;
            blur.saturation = 1.25f;
            blur.tint = light ? XrColor4f{0.92f, 0.92f, 0.94f, 0.16f} : XrColor4f{0.02f, 0.02f, 0.025f, 0.30f};
            blur.maskAlpha = 0.4f;
            blurs.push_back(blur);
            chainedBlur = blur;
            *tail = &blurs.back();
            tail = &blurs.back().next;
        }
        return next;
    };
    // Sharp at a distance, for every layer shown (windows, Home, frames, cards, the keyboard): the level of
    // filtered copy it wants where it is nearest the eyes, from its pixels a metre; the least of the places it shows.
    std::unordered_map<const SurfaceLayer *, float> wanted;
    const auto want = [&](const SurfaceLayer &layer, const Spatial::Pose &pose, XrExtent2Df size) {
        if (options.viewLocked || sharpOff || sharpDensity <= 0.0f || size.width <= 0.0f || size.height <= 0.0f) {
            return;
        }
        const Spatial::Vec3 eye = pose.inverse().transform(head.position);
        const Spatial::Vec3 nearest{std::clamp(eye.x, -size.width / 2.0f, size.width / 2.0f), std::clamp(eye.y, -size.height / 2.0f, size.height / 2.0f), 0.0f};
        const float density = std::max(float(layer.width) / size.width, float(layer.height) / size.height);
        const float level = Spatial::Sharpness::wantedLevel(density, (eye - nearest).length(), sharpDensity);
        const auto [it, inserted] = wanted.try_emplace(&layer, level);
        if (!inserted) {
            it->second = std::min(it->second, level);
        }
    };
    // What the screenshot needs of a layer just added (the colour scale as chain() put it on).
    const auto record = [&](const SurfaceLayer &layer, Xr::ViewShot::Kind kind, const XrPosef &pose, XrSpace layerSpace) -> ShotLayer & {
        ShotLayer &shot = shotLayers.emplace_back();
        shot.layer = &layer;
        shot.kind = kind;
        shot.pose = toPose(pose);
        shot.headLocked = layerSpace != space;
        if (colorScaleLayers && quadFade < 0.999f) {
            std::fill(std::begin(shot.scale), std::end(shot.scale), quadFade);
        }
        return shot;
    };
    const auto addQuad = [&](const SurfaceLayer &layer, const Spatial::Pose &pose, XrExtent2Df size, XrSpace quadSpace, bool glass = false) {
        if (!canAdd()) {
            return;
        }
        want(layer, pose, size);
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        quad.layerFlags = layer.alpha ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
        quad.space = quadSpace;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = layer.swapchain;
        quad.subImage.imageRect = shownRect(layer);
        quad.pose = toXr(quadSpace == space ? recline * pose : pose);
        quad.size = size;
        quad.next = chain(layer, glass);
        quads.push_back(quad);
        headers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&quads.back()));
        ShotLayer &shot = record(layer, Xr::ViewShot::Kind::Quad, quad.pose, quadSpace);
        shot.width = size.width;
        shot.height = size.height;
        shot.blurRadians = chainedBlur.blurRadians;
        shot.blurSaturation = chainedBlur.saturation;
        std::copy_n(&chainedBlur.tint.r, 4, shot.blurTint);
        shot.blurMaskAlpha = chainedBlur.maskAlpha;
    };
    /*!
     * A piece of a curved window: @p width x @p height metres of arc on the cylinder of @p radius
     * whose axis is @p surface's Y through (0, 0, radius), centred @p x, @p y along it (window-local metres), @p lift in
     * front. The cylinder layer shows its image around -Z from its pose, so the pose sits on the axis, turned to x.
     */
    const auto addArc = [&](const SurfaceLayer &layer, const Spatial::Pose &surface, float radius, float x, float y, float width,
                            float height, float lift, XrSpace arcSpace, bool glass = false) {
        if (!canAdd()) {
            return;
        }
        want(layer, {surface.orientation, surface.transform({x, y, 0.0f})}, {width, height}); // as if flat: near enough
        XrCompositionLayerCylinderKHR arc{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
        arc.layerFlags = layer.alpha ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
        arc.space = arcSpace;
        arc.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        arc.subImage.swapchain = layer.swapchain;
        arc.subImage.imageRect = shownRect(layer);
        const Spatial::Pose arcPose{surface.orientation * Spatial::Quat::fromYaw(-x / radius), surface.transform({0.0f, y, radius})};
        arc.pose = toXr(arcSpace == space ? recline * arcPose : arcPose);
        arc.radius = radius - lift;
        arc.centralAngle = width / radius;
        arc.aspectRatio = width / height; // the arc's height: radius x angle / aspect = height
        arc.next = chain(layer, glass);
        cylinders.push_back(arc);
        headers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&cylinders.back()));
        ShotLayer &shot = record(layer, Xr::ViewShot::Kind::Cylinder, arc.pose, arcSpace);
        shot.radius = arc.radius;
        shot.centralAngle = arc.centralAngle;
        shot.aspectRatio = arc.aspectRatio;
        shot.blurRadians = chainedBlur.blurRadians;
        shot.blurSaturation = chainedBlur.saturation;
        std::copy_n(&chainedBlur.tint.r, 4, shot.blurTint);
        shot.blurMaskAlpha = chainedBlur.maskAlpha;
    };
    const std::map<int, float> angles = layoutAngles(taken.windows, options.pixelsPerMeter, options.distance, options.gapDegrees);
    const XrSpace base = options.deskView ? viewSpace : space; //!< the room's space

    // The room around, behind everything: a panorama on a sphere far away, or a photo on a curved screen in front of
    // the home, its middle a little below eye height (where a photo's horizon usually is). Not in the exactness check.
    XrCompositionLayerCylinderKHR cylinder{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
    XrCompositionLayerEquirect2KHR equirect{XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR};
    XrCompositionLayerHdrMNDX environmentHdr{XR_TYPE_COMPOSITION_LAYER_HDR_MNDX};
    XrCompositionLayerColorScaleBiasKHR environmentDim{XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR};
    const auto environment = layers.find(kEnvironmentLayer);
    // The room's brightness (Settings), and in cinema its light down to the cinema setting's share of that: off, 15 %
    // (a theater; a quarter was not dim enough) or 40 %. Off: not drawn at all.
    const float light = options.roomBrightness * (1.0f - (1.0f - options.cinemaRoomLight) * cinemaFade);
    if (!options.viewLocked && environment != layers.end() && environment->second.hasImage && light > 0.004f) {
        const SurfaceLayer &layer = environment->second;
        const Spatial::Pose &home = layout.home();
        environmentHdr.transfer = XrHdrTransferMNDX(layer.hdr.transfer);
        environmentHdr.primaries = XrHdrPrimariesMNDX(layer.hdr.primaries);
        environmentHdr.referenceWhiteNits = layer.hdr.referenceWhiteNits;
        // A room is one still picture: its peak is every frame's scene peak, so the runtime maps it the same wherever
        // one looks (measured from the view, the sky brightened over half a second each time the sun left it). With
        // the room's brightness (the colour scale below) the peak shown is that much lower.
        const float shownPeak = layer.hdr.maxCllNits * (colorScaleLayers ? light : 1.0f);
        environmentHdr.maxContentLightLevelNits = shownPeak;
        environmentHdr.sceneMaxNits = shownPeak;
        if (colorScaleLayers && light < 0.999f) {
            environmentDim = {XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR, nullptr, {light, light, light, 1.0f}, {0.0f, 0.0f, 0.0f, 0.0f}};
            environmentHdr.next = &environmentDim;
        }
        const XrSwapchainSubImage image{layer.swapchain, shownRect(layer), 0};
        const float aspect = float(layer.width) / float(layer.height);
        if (environmentPanorama && equirectLayers) {
            equirect.next = hdrExtension ? &environmentHdr : nullptr;
            equirect.space = base;
            equirect.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            equirect.subImage = image;
            equirect.pose = toXr({home.orientation * Spatial::Quat::fromYaw(-Spatial::radians(options.roomTurnDegrees)), home.position});
            equirect.radius = options.environmentDistance; // room-sized, so both eyes place it (0: infinitely far)
            equirect.centralHorizontalAngle = 2.0f * float(M_PI);
            equirect.upperVerticalAngle = float(M_PI) / 2.0f;
            equirect.lowerVerticalAngle = -float(M_PI) / 2.0f;
            headers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&equirect));
            ShotLayer &shot = record(layer, Xr::ViewShot::Kind::Equirect, equirect.pose, base);
            shot.radius = equirect.radius;
            shot.centralAngle = equirect.centralHorizontalAngle;
            shot.upperAngle = equirect.upperVerticalAngle;
            shot.lowerAngle = equirect.lowerVerticalAngle;
            if (colorScaleLayers && light < 0.999f) {
                std::fill_n(shot.scale, 3, light);
            }
        } else if (cylinderLayers) {
            cylinder.next = hdrExtension ? &environmentHdr : nullptr;
            cylinder.space = base;
            cylinder.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            cylinder.subImage = image;
            cylinder.pose = toXr({home.orientation, home.position + Spatial::Vec3{0.0f, -0.15f, 0.0f}});
            cylinder.radius = options.environmentDistance;
            cylinder.centralAngle = Spatial::radians(options.environmentAngle);
            cylinder.aspectRatio = aspect;
            headers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&cylinder));
            ShotLayer &shot = record(layer, Xr::ViewShot::Kind::Cylinder, cylinder.pose, base);
            shot.radius = cylinder.radius;
            shot.centralAngle = cylinder.centralAngle;
            shot.aspectRatio = cylinder.aspectRatio;
            if (colorScaleLayers && light < 0.999f) {
                std::fill_n(shot.scale, 3, light);
            }
        } else {
            const float width = 2.0f * options.environmentDistance * std::tan(Spatial::radians(options.environmentAngle) / 2.0f);
            // The room stays where it is when reclined: addQuad's recline undone.
            addQuad(layer, recline.inverse() * Spatial::Pose{home.orientation, home.transform({0.0f, -0.15f, -options.environmentDistance})},
                    {width, width / aspect}, base);
        }
    }
    const auto addChrome = [&](const Spatial::SpatialWindow &window, const Chrome &c) {
        const int windowId = window.windowId;
        const float radius = layout.curveRadius(window);
        // On the window's surface (on a curved window: on its curve, facing its axis); bars along the curve bend with it.
        const auto part = [&](int p, const Spatial::FrameRect &r, bool bends = false) {
            const auto layer = layers.find(chromeLayer(windowId, p));
            if (layer == layers.end() || !layer->second.hasImage) {
                return;
            }
            const bool glass = p == kMenuPart; // the menu's glass is frosted (the bars are too thin to show it)
            if (bends && radius > 0.0f) {
                addArc(layer->second, c.surface, radius, r.x, r.y, r.width, r.height, 0.002f, base, glass);
            } else {
                addQuad(layer->second, layout.pointOn(window, r.x, r.y, 0.002f), {r.width, r.height}, base, glass);
            }
        };
        part(kGrabPart, c.frame.grab, true);
        part(kMenuPart, c.frame.menu);
        part(kTiltPart, c.frame.tilt);
        part(kResetPart, c.frame.resetTilt);
        part(kScalePart, c.frame.scale);
        if (!window.fullscreen) {
            // Not on a fullscreen window: its size in pixels is the screen's, the scale handle makes it bigger (the
            // button did nothing there).
            part(kResizePart, c.frame.resize);
        }
        if (cylinderLayers) {
            // The curvature slider: its track, the knob where the curvature is, the flat dot.
            part(kCurvePart, c.frame.curve, true);
            const float knob = Spatial::FrameLayout::kKnob * c.frame.unit;
            const auto knobLayer = layers.find(chromeLayer(windowId, kKnobPart));
            if (knobLayer != layers.end() && knobLayer->second.hasImage) {
                addQuad(knobLayer->second, layout.pointOn(window, c.frame.knobX(window.curvature), c.frame.curve.y, 0.004f), {knob, knob}, base);
            }
            part(kFlatPart, c.frame.resetCurve);
        }
        const int button = menuIndex(c.hovered);
        if (button >= 0 && c.labelArt == labelFor(windowId, c.hovered)) {
            const float u = c.frame.unit;
            const float w = FrameArt::kLabelWidth * u;
            part(kLabelPart, {c.frame.menu.x - c.frame.menu.width / 2.0f - 0.012f * u - w / 2.0f, c.frame.buttons[button].y, w,
                              FrameArt::kLabelHeight * u});
        }
    };
    // Layers are not depth tested: farther windows first so nearer ones cover them, the held window last (on top).
    std::vector<const WindowPlacement *> order;
    for (const WindowPlacement &window : taken.windows) {
        order.push_back(&window);
    }
    if (!options.viewLocked) {
        const auto rank = [&](const WindowPlacement *w) {
            const auto placed = layout.windows().find(w->windowId);
            const float distance = placed == layout.windows().end() ? 0.0f : (placed->second.pose.position - head.position).length();
            return std::pair{w->windowId == layout.grabbedWindow(), -distance};
        };
        std::stable_sort(order.begin(), order.end(), [&](const WindowPlacement *a, const WindowPlacement *b) {
            return rank(a) < rank(b);
        });
    }
    for (const WindowPlacement *windowPtr : order) {
        const WindowPlacement &window = *windowPtr;
        // The window's place: from the layout in the room, or (debug) fixed to the view.
        Spatial::Pose windowPose;
        float scale = 1.0f;
        if (options.viewLocked) {
            const float angle = angles.at(window.windowId);
            windowPose = {Spatial::Quat::fromYaw(-angle), {options.distance * std::sin(angle), 0.0f, -options.distance * std::cos(angle)}};
        } else {
            const auto placed = layout.windows().find(window.windowId);
            if (placed == layout.windows().end()) {
                continue;
            }
            if (placed->second.minimized) {
                continue; // hidden: neither it nor its frame
            }
            windowPose = layout.surfacePose(placed->second);
            scale = placed->second.scale;
        }
        const float ppm = options.pixelsPerMeter / scale;
        // Curved: every surface a piece of the window's cylinder.
        float radius = 0.0f;
        if (!options.viewLocked && cylinderLayers) {
            if (const auto placed = layout.windows().find(window.windowId); placed != layout.windows().end()) {
                radius = layout.curveRadius(placed->second);
            }
        }
        // Home fades every window out; cinema mode the others.
        const int cinema = layout.cinemaWindow();
        quadFade = options.viewLocked ? 1.0f : (1.0f - homeFade) * (cinema && !shownInCinema(window.windowId) ? 1.0f - cinemaFade : 1.0f);
        for (const SurfacePlacement &surface : window.surfaces) {
            const auto it = layers.find(surface.surfaceId);
            if (it == layers.end() || !it->second.hasImage) {
                continue;
            }
            const float lx = (surface.x + surface.width / 2.0f - window.width / 2.0f) / ppm;
            const float ly = -(surface.y + surface.height / 2.0f - window.height / 2.0f) / ppm;
            // Settings' window is glass too (its translucent background frosted).
            const bool glass = window.appId == "org.rifthome.settings";
            if (radius > 0.0f) {
                addArc(it->second, windowPose, radius, lx, ly, surface.width / ppm, surface.height / ppm, 0.0f, base, glass);
                continue;
            }
            const Spatial::Pose pose{windowPose.orientation, windowPose.transform({lx, ly, 0.0f})};
            addQuad(it->second, pose, {surface.width / ppm, surface.height / ppm}, options.viewLocked ? viewSpace : base, glass);
        }
        // Its frame, while it shows: drawn with its window, so nearer windows cover it. While the keyboard is open, the
        // window it types into shows its grab bar (dim): where the keys go.
        const auto framed = chrome.find(window.windowId);
        if (cinema && !shownInCinema(window.windowId)) {
            continue; // no frames for the hidden windows
        }
        const auto placedWindow = layout.windows().find(window.windowId);
        if (!options.viewLocked && framed != chrome.end() && framed->second.visible && placedWindow != layout.windows().end()) {
            addChrome(placedWindow->second, framed->second);
        }
        // While the keyboard is open, a neon blue dot above the window its keys go to (the dim grab bar
        // was not noticed).
        const auto dot = layers.find(kFocusDotLayer);
        if (!options.viewLocked && framed != chrome.end() && placedWindow != layout.windows().end() && keyboardOpen && window.focused
            && dot != layers.end() && dot->second.hasImage) {
            // Above the curvature slider (both at the top's middle).
            const Spatial::FrameLayout &f = framed->second.frame;
            const float size = 0.075f * f.unit;
            addQuad(dot->second, layout.pointOn(placedWindow->second, 0.0f, f.windowHeight / 2.0f + 0.095f * f.unit, 0.003f), {size, size}, base);
        }
    }
    quadFade = homeSearching ? 1.0f : 1.0f - homeFade; // the keyboard goes with the windows while home is open, unless it types into its search
    // The keyboard over the windows, the keys under the rays lit.
    const auto keyboardLayer = layers.find(kKeyboardLayer);
    if (keyboardOpen && !options.viewLocked && keyboardLayer != layers.end() && keyboardLayer->second.hasImage) {
        addQuad(keyboardLayer->second, keyboardPose, {keyboard.width(), keyboard.height()}, base, true);
        const auto bar = layers.find(kKeyboardBarLayer);
        if (bar != layers.end() && bar->second.hasImage) {
            const float y = -keyboard.height() / 2.0f - kKeyboardBarGap;
            addQuad(bar->second, {keyboardPose.orientation, keyboardPose.transform({0.0f, y, 0.0f})},
                    {kKeyboardBarWidth, Spatial::FrameLayout::kBarThickness}, base);
        }
        for (int h = 0; h < 2; ++h) {
            const int key = keyHeld[h] >= 0 ? keyHeld[h] : (keyHits[h] ? keyHits[h]->key : -1);
            const auto highlight = layers.find(keyHeld[h] >= 0 ? kKeyPressLayer : kKeyHoverLayer);
            if (key >= 0 && highlight != layers.end() && highlight->second.hasImage) {
                const Spatial::KeyRect &r = keyboard.rects()[size_t(key)];
                addQuad(highlight->second, {keyboardPose.orientation, keyboardPose.transform({r.x, r.y, 0.002f})}, {r.width, r.height}, base);
            }
        }
    }
    // Home: the app icons floating in the room, the one under the ray larger; it fades in as the windows fade out.
    quadFade = homeFade;
    const auto launcherLayer = layers.find(kLauncherLayer);
    if (homeFade > 0.0f && !options.viewLocked && launcherLayer != layers.end() && launcherLayer->second.hasImage
        && launcherArtPage == launcherPage) {
        addQuad(launcherLayer->second, launcherPose, {Spatial::LauncherGrid::width(), Spatial::LauncherGrid::height()}, base);
        const auto plate = layers.find(kHomePlateLayer);
        const int hovered = homeHovered();
        if (hovered >= 0 && hovered == homePlateArt && plate != layers.end() && plate->second.hasImage) {
            float x = 0.0f;
            float y = 0.0f;
            Spatial::LauncherGrid::cellCentre(hovered % Spatial::LauncherGrid::kPerPage, x, y);
            const float grown = cellPressed[0] >= 0 || cellPressed[1] >= 0 ? 1.08f : 1.18f;
            const float size = Spatial::LauncherGrid::kPlate * grown * 240.0f / 190.0f;
            addQuad(plate->second, {launcherPose.orientation, launcherPose.transform({x, y + 0.03f + 0.004f, 0.01f})}, {size, size}, base);
        }
        // The running app's card under a ray, lit.
        for (int h = 0; h < 2; ++h) {
            const int card = cardPressed[h] >= 0 ? cardPressed[h] : cellHits[h] ? cellHits[h]->card : -1;
            const auto highlight = layers.find(cardPressed[h] >= 0 ? kKeyPressLayer : kKeyHoverLayer);
            if (card >= 0 && highlight != layers.end() && highlight->second.hasImage) {
                float cx, cy, cw, ch;
                Spatial::LauncherGrid::card(card, int(runningWindows.size()), cx, cy, cw, ch);
                addQuad(highlight->second, {launcherPose.orientation, launcherPose.transform({cx, cy, 0.006f})}, {cw, ch}, base);
            }
        }
    }
    quadFade = 1.0f;
    // The dock, the item under each ray lit.
    const auto dockLayer = layers.find(kDockLayer);
    if (dockOpen && !options.viewLocked && dockLayer != layers.end() && dockLayer->second.hasImage) {
        addQuad(dockLayer->second, dockPose, {Spatial::DockLayout::width(), Spatial::DockLayout::height()}, base, true);
        const auto dockBar = layers.find(kDockBarLayer);
        if (dockBar != layers.end() && dockBar->second.hasImage) {
            addQuad(dockBar->second, {dockPose.orientation, dockPose.transform({0.0f, -Spatial::DockLayout::height() / 2.0f - kDockBarGap, 0.0f})},
                    {kDockBarWidth, Spatial::FrameLayout::kBarThickness}, base);
        }
        for (int h = 0; h < 2; ++h) {
            const Spatial::DockItem item = dockPressed[h] != Spatial::DockItem::None ? dockPressed[h]
                : dockHits[h]                                                    ? dockHits[h]->item
                                                                                 : Spatial::DockItem::None;
            const auto highlight = layers.find(dockPressed[h] != Spatial::DockItem::None ? kKeyPressLayer : kKeyHoverLayer);
            if (item != Spatial::DockItem::None && highlight != layers.end() && highlight->second.hasImage) {
                const Spatial::FrameRect r = Spatial::DockLayout::rect(item);
                addQuad(highlight->second, {dockPose.orientation, dockPose.transform({r.x, r.y, 0.002f})}, {r.width, r.height}, base);
            }
        }
    }
    // Notifications: the dock's list above it, the newest's card above the dock's place.
    const auto noteList = layers.find(kNoteListLayer);
    if (dockOpen && notesOpen && !options.viewLocked && noteList != layers.end() && noteList->second.hasImage && noteListArt
        && std::get<0>(*noteListArt) == noteRevision) {
        addQuad(noteList->second, noteListPose(),
                {Spatial::NotificationLayout::listWidth(), Spatial::NotificationLayout::listHeight(int(notes.size()), noteListActions())}, base, true);
    }
    const auto noteCardLayer = layers.find(kNoteCardLayer);
    if (noteCardShown() && !options.viewLocked && noteCardLayer != layers.end() && noteCardLayer->second.hasImage && noteCardArt
        && std::get<0>(*noteCardArt) == noteRevision) {
        addQuad(noteCardLayer->second, noteCardPose, {Spatial::NotificationLayout::kCardWidth, Spatial::NotificationLayout::cardHeight(noteCardActions())}, base, true);
    }
    // A short message (game mode's way out), for its few seconds.
    const auto hint = layers.find(kHintLayer);
    if (hintUntil && Clock::now() < *hintUntil && !options.viewLocked && hint != layers.end() && hint->second.hasImage) {
        addQuad(hint->second, hintPose, {Spatial::NotificationLayout::kCardWidth, Spatial::NotificationLayout::kCardHeight}, base, true);
    }
    // Beams from the controllers and cursors where they meet a window.
    const auto beam = layers.find(kBeamLayer);
    const auto cursor = layers.find(kCursorLayer);
    for (int h = 0; h < 2 && !options.viewLocked; ++h) {
        if (!hands[h].tracked) {
            continue;
        }
        const Spatial::Vec3 origin = hands[h].aim.position;
        const Spatial::Vec3 direction = hands[h].aim.forward();
        const float length = hits[h] ? hits[h]->distance
            : chromeHits[h]          ? chromeHits[h]->distance
            : keyHits[h]             ? keyHits[h]->distance
            : cellHits[h]            ? cellHits[h]->distance
            : dockHits[h]            ? dockHits[h]->distance
            : noteHits[h]            ? noteHits[h]->distance
                                     : 2.0f;
        if (beam != layers.end() && beam->second.hasImage) {
            const Spatial::Vec3 centre = origin + direction * (length / 2.0f);
            const Spatial::Vec3 toEye = head.position - centre;
            Spatial::Vec3 z = (toEye - direction * toEye.dot(direction)).normalized();
            if (z.length() < 0.5f) {
                z = direction.cross({0.0f, 1.0f, 0.0f}).normalized();
            }
            const Spatial::Vec3 x = direction.cross(z);
            addQuad(beam->second, {Spatial::Quat::fromBasis(x, direction, z), centre}, {options.pointerWidth, length}, base);
        }
        if (dockHits[h] && cursor != layers.end() && cursor->second.hasImage) {
            const float size = std::clamp(dockHits[h]->distance * 0.012f, 0.006f, 0.05f);
            addQuad(cursor->second, {dockPose.orientation, dockHits[h]->point + dockPose.orientation.rotate({0.0f, 0.0f, 0.004f})},
                    {size, size}, base);
        }
        if (noteHits[h] && cursor != layers.end() && cursor->second.hasImage) {
            const float size = std::clamp(noteHits[h]->distance * 0.012f, 0.006f, 0.05f);
            const Spatial::Quat facing = noteHits[h]->card ? noteCardPose.orientation : noteListPose().orientation;
            addQuad(cursor->second, {facing, noteHits[h]->point + facing.rotate({0.0f, 0.0f, 0.004f})}, {size, size}, base);
        }
        if (cellHits[h] && cursor != layers.end() && cursor->second.hasImage) {
            const float size = std::clamp(cellHits[h]->distance * 0.012f, 0.006f, 0.05f);
            addQuad(cursor->second, {launcherPose.orientation, cellHits[h]->point + launcherPose.orientation.rotate({0.0f, 0.0f, 0.004f})},
                    {size, size}, base);
        }
        if (keyHits[h] && !hits[h] && !chromeHits[h] && cursor != layers.end() && cursor->second.hasImage) {
            const float size = std::clamp(keyHits[h]->distance * 0.012f, 0.006f, 0.05f);
            addQuad(cursor->second, {keyboardPose.orientation, keyHits[h]->point + keyboardPose.orientation.rotate({0.0f, 0.0f, 0.004f})},
                    {size, size}, base);
        }
        const int pointed = hits[h] ? hits[h]->windowId : chromeHits[h] ? chromeHits[h]->windowId : 0;
        // A drag and drop in progress: what it carries, just below and right of the cursor (or at the ray's end off the
        // windows), so the user sees something is held.
        const auto badge = layers.find(kDragLayer);
        if (h == activeHand && bridge.dragging() && badge != layers.end() && badge->second.hasImage) {
            const auto placed = pointed && hits[h] ? layout.windows().find(pointed) : layout.windows().end();
            if (placed != layout.windows().end()) {
                const float size = std::clamp(hits[h]->distance * 0.035f, 0.03f, 0.12f);
                addQuad(badge->second, layout.pointOn(placed->second, hits[h]->localX + size * 0.8f, hits[h]->localY - size * 0.8f, 0.006f), {size, size}, base);
            } else {
                const Spatial::Vec3 end = origin + direction * 1.2f;
                addQuad(badge->second, {facingEye(end), end}, {0.045f, 0.045f}, base);
            }
        }
        if (pointed && cursor != layers.end() && cursor->second.hasImage) {
            const auto placed = layout.windows().find(pointed);
            if (placed != layout.windows().end()) {
                // On the window's surface where the ray meets it (curved or flat), a little in front.
                const float distance = hits[h] ? hits[h]->distance : chromeHits[h]->distance;
                const float x = hits[h] ? hits[h]->localX : chromeHits[h]->localX;
                const float y = hits[h] ? hits[h]->localY : chromeHits[h]->localY;
                const float size = std::clamp(distance * 0.012f, 0.008f, 0.05f);
                addQuad(cursor->second, layout.pointOn(placed->second, x, y, 0.004f), {size, size}, base);
            }
        }
    }
    // The controllers in the room: where they are, for both eyes, over everything else (they are the nearest things).
    XrCompositionLayerProjection controllerProjection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView controllerViews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerHdrMNDX controllerHdr{XR_TYPE_COMPOSITION_LAYER_HDR_MNDX};
    const Clock::time_point controllersStart = Clock::now();
    if (controllerLayer && controllerLayer->renderer.ready() && frameState.shouldRender) {
        std::vector<DrawnController> drawn;
        if (testControllers) {
            // Desk checks: both as one sees them looking down at one's hands holding them ahead, like Vive wands:
            // the handle 60 degrees up, seen from about 42 degrees above. The headset lies still, so the
            // pose is turned by the difference instead: 15 degrees below the view, the handles nearly upright to it (87
            // degrees, tops a little away), the top faces in sight; turned a little to each other.
            XrSpaceLocation viewLocation{XR_TYPE_SPACE_LOCATION};
            if (XR_SUCCEEDED(xrLocateSpace(viewSpace, space, frameState.predictedDisplayTime, &viewLocation))) {
                const Spatial::Pose eyes = toPose(viewLocation.pose);
                for (int h = 0; h < 2; ++h) {
                    const float side = h == 0 ? -1.0f : 1.0f;
                    const Spatial::Quat turn = Spatial::Quat::fromYaw(side * 0.25f) * Spatial::Quat::fromPitch(1.52f);
                    drawn.push_back({h == 0, {eyes.orientation * turn, eyes.transform({side * 0.11f, -0.08f, -0.30f})}});
                }
            }
        } else if (!options.deskView) {
            for (int h = 0; h < 2; ++h) {
                if (hands[h].gripTracked) {
                    drawn.push_back({h == 0, hands[h].grip});
                }
            }
        }
        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
        locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime = frameState.predictedDisplayTime;
        locate.space = space; // the room's (LOCAL), as the grips: a projection layer in VIEW space is not shown so
        XrViewState viewState{XR_TYPE_VIEW_STATE};
        XrView eyes[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t located = 0;
        const XrViewStateFlags needed = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
        const XrResult locatedViews = drawn.empty() ? XR_SUCCESS : xrLocateViews(session, &locate, &viewState, 2, &located, eyes);
        const QString state = drawn.empty()                 ? QStringLiteral("none tracked")
            : XR_FAILED(locatedViews) || located != 2         ? QStringLiteral("eyes not located (%1)").arg(int(locatedViews))
            : (viewState.viewStateFlags & needed) != needed ? QStringLiteral("eyes' poses not valid (flags %1)").arg(viewState.viewStateFlags)
                                                            : QStringLiteral("%1 drawn").arg(drawn.size());
        if (state != controllerState) {
            controllerState = state;
            qCInfo(lcXr).noquote() << "controllers:" << state;
        }
        if (!drawn.empty() && XR_SUCCEEDED(locatedViews) && located == 2 && (viewState.viewStateFlags & needed) == needed) {
            ControllerLayer &cl = *controllerLayer;
            uint32_t indices[2] = {0, 0};
            bool acquiredBoth = true;
            for (int eye = 0; eye < 2 && acquiredBoth; ++eye) {
                XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                acquiredBoth = XR_SUCCEEDED(xrAcquireSwapchainImage(cl.swapchains[eye], &acquireInfo, &indices[eye]));
                XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                waitInfo.timeout = XR_INFINITE_DURATION;
                acquiredBoth = acquiredBoth && XR_SUCCEEDED(xrWaitSwapchainImage(cl.swapchains[eye], &waitInfo));
            }
            if (acquiredBoth) {
                // The previous frame's drawing is long done by now; this frame's is not waited for (the runtime's own
                // work on the images follows it on the same queue).
                if (cl.submitted) {
                    vkWaitForFences(device, 1, &cl.fence, VK_TRUE, 1'000'000'000ull);
                    vkResetFences(device, 1, &cl.fence);
                    cl.submitted = false;
                }
                vkResetCommandBuffer(cl.cmd, 0);
                VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(cl.cmd, &cbbi);
                for (int eye = 0; eye < 2; ++eye) {
                    const XrPosef &p = eyes[eye].pose;
                    EyeView view;
                    view.pose = {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w}, {p.position.x, p.position.y, p.position.z}};
                    view.angleLeft = eyes[eye].fov.angleLeft;
                    view.angleRight = eyes[eye].fov.angleRight;
                    view.angleUp = eyes[eye].fov.angleUp;
                    view.angleDown = eyes[eye].fov.angleDown;
                    cl.renderer.record(cl.cmd, eye, indices[eye], view, drawn);
                    controllerViews[eye].pose = eyes[eye].pose;
                    controllerViews[eye].fov = eyes[eye].fov;
                    controllerViews[eye].subImage.swapchain = cl.swapchains[eye];
                    controllerViews[eye].subImage.imageRect = {{0, 0}, {int32_t(cl.width), int32_t(cl.height)}};
                    controllerViews[eye].subImage.imageArrayIndex = 0;
                }
                vkEndCommandBuffer(cl.cmd);
                VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                submit.commandBufferCount = 1;
                submit.pCommandBuffers = &cl.cmd;
                cl.submitted = vkQueueSubmit(queue, 1, &submit, cl.fence) == VK_SUCCESS;
            }
            for (int eye = 0; eye < 2; ++eye) {
                XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(cl.swapchains[eye], &releaseInfo);
            }
            if (acquiredBoth && canAdd()) {
                controllerProjection.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                controllerProjection.space = space;
                controllerProjection.viewCount = 2;
                controllerProjection.views = controllerViews;
                if (cl.hdr) {
                    // Linear light, 1.0 = the room's white; no metadata: the runtime maps by the highlights it measures.
                    controllerHdr.transfer = XR_HDR_TRANSFER_LINEAR_MNDX;
                    controllerHdr.primaries = XR_HDR_PRIMARIES_BT709_MNDX;
                    controllerProjection.next = &controllerHdr;
                }
                headers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&controllerProjection));
            }
        }
    }
    controllerTime.add(msSince(controllersStart));
    // The lens test: only it (black around), each eye's picture straight ahead of that eye, on its lens's axis where the
    // lens is sharpest, fixed to the head; both eyes' lines of sight parallel, as for something far away.
    XrCompositionLayerQuad lensQuads[2] = {{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}};
    if (lensTest != LensTest::Off) {
        headers.clear();
        const auto lens = layers.find(kLensTestLayer);
        if (lensEyesValid && lensDensity > 0.0f && lens != layers.end() && lens->second.hasImage) {
            const SurfaceLayer &layer = lens->second;
            for (int eye = 0; eye < 2; ++eye) {
                XrCompositionLayerQuad &quad = lensQuads[eye];
                quad.space = viewSpace;
                quad.eyeVisibility = eye == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
                quad.subImage.swapchain = layer.swapchain;
                quad.subImage.imageRect = shownRect(layer);
                quad.pose = toXr({lensEyes[eye].orientation, lensEyes[eye].transform({0.0f, 0.0f, -kLensTestDistance})});
                // kLensTestDegrees across its outer pixels' centres.
                const float side = 2.0f * std::tan(Spatial::radians(FrameArt::kLensTestDegrees / 2.0f)) * float(layer.width) / float(layer.width - 1);
                quad.size = {side * kLensTestDistance, side * kLensTestDistance};
                headers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&quad));
            }
        }
    }
    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = frameState.shouldRender ? uint32_t(headers.size()) : 0;
    endInfo.layers = headers.data();
    const Clock::time_point endStart = Clock::now();
    const XrResult result = xrEndFrame(session, &endInfo);
    endTime.add(msSince(endStart));
    const double work = msSince(frameStart);
    workTime.add(work);
    otherTime.add(work - copyTime.last - controllerTime.last - endTime.last);
    slowFrames += work > 9.0;
    if (result != lastEndFrame) {
        lastEndFrame = result;
        check(result, "xrEndFrame");
        if (XR_SUCCEEDED(result)) {
            qCInfo(lcXr) << "xrEndFrame accepted" << endInfo.layerCount << "layer(s)";
        }
    }
    ++frames;
    lastLayerCount = endInfo.layerCount;
    // The levels wanted this frame, for the next frame's copies (layers not shown keep theirs: nothing to make again).
    for (auto &[id, layer] : layers) {
        if (const auto w = wanted.find(&layer); w != wanted.end()) {
            layer.wantLevel = w->second;
        }
    }
    // Desk checks: one screenshot this many seconds after the first frame (RIFT_HOME_TEST_SCREENSHOT_S).
    static const Clock::time_point firstFrame = Clock::now();
    static const int testShot = qEnvironmentVariableIntValue("RIFT_HOME_TEST_SCREENSHOT_S");
    static bool testShotDone = false;
    if (testShot > 0 && !testShotDone && Clock::now() - firstFrame > std::chrono::seconds(testShot)) {
        testShotDone = true;
        screenshotPending = true;
    }
    if (screenshotPending) {
        screenshotPending = false;
        saveScreenshot();
    }
    logStats();
}

bool XrCompositor::Impl::readBack(const SurfaceLayer &layer, Readback &out)
{
    out.half = layer.storage == VK_FORMAT_R16G16B16A16_SFLOAT;
    out.width = layer.width;
    out.height = layer.height;
    out.size = VkDeviceSize(layer.width) * layer.height * (out.half ? 8 : 4);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = out.size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(device, &bci, nullptr, &out.buffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, out.buffer, &requirements);
    const int32_t type = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = requirements.size;
    mai.memoryTypeIndex = uint32_t(std::max(type, 0));
    if (type < 0 || vkAllocateMemory(device, &mai, nullptr, &out.memory) != VK_SUCCESS) {
        vkDestroyBuffer(device, out.buffer, nullptr);
        out.buffer = VK_NULL_HANDLE;
        return false;
    }
    vkBindBufferMemory(device, out.buffer, out.memory, 0);
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer copy = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &cbai, &copy);
    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(copy, &cbbi);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {uint32_t(layer.width), uint32_t(layer.height), 1};
    vkCmdCopyImageToBuffer(copy, layer.passImage, VK_IMAGE_LAYOUT_GENERAL, out.buffer, 1, &region); // the pass image stays GENERAL
    vkEndCommandBuffer(copy);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &copy;
    vkQueueSubmit(queue, 1, &submit, fence);
    vkWaitForFences(device, 1, &fence, VK_TRUE, 1'000'000'000ull);
    vkResetFences(device, 1, &fence);
    vkFreeCommandBuffers(device, pool, 1, &copy);
    return true;
}

void XrCompositor::Impl::saveLayerDump(const QString &dir)
{
    if (!QDir().mkpath(dir)) {
        return;
    }
    QString list;
    int saved = 0;
    for (auto &[id, layer] : layers) {
        if (!layer.hasImage || layer.passImage == VK_NULL_HANDLE || layer.width * layer.height < 128 * 128) {
            continue; // nothing drawn yet, or a small piece of art
        }
        Readback read;
        if (!readBack(layer, read)) {
            continue;
        }
        // Written to disk off the frame thread (a 4K HDR layer is 66 MB, a PNG takes a while to encode: the windows
        // stood still for that long), straight from the read-back memory, which goes with it.
        const QString base = dir + QStringLiteral("/layer%1").arg(id);
        freeLater([device = device, read, base] {
            void *data = nullptr;
            if (vkMapMemory(device, read.memory, 0, read.size, 0, &data) == VK_SUCCESS) {
                if (read.half) {
                    // As RiftLinux's output captures (RLOF: magic, width, height, RGBA half floats, rows top down).
                    QFile file(base + QStringLiteral(".rlof"));
                    if (file.open(QIODevice::WriteOnly)) {
                        const uint32_t header[3] = {0x464F4C52u, uint32_t(read.width), uint32_t(read.height)};
                        file.write(reinterpret_cast<const char *>(header), sizeof(header));
                        file.write(static_cast<const char *>(data), qint64(read.size));
                    }
                } else {
                    QImage(static_cast<const uchar *>(data), read.width, read.height, read.width * 4, QImage::Format_RGBA8888)
                        .save(base + QStringLiteral(".png"));
                }
                vkUnmapMemory(device, read.memory);
            }
            vkFreeMemory(device, read.memory, nullptr);
            vkDestroyBuffer(device, read.buffer, nullptr);
        });
        ++saved;
        list += QStringLiteral("layer%1 %2x%3 %4 %5\n").arg(id).arg(layer.width).arg(layer.height)
                    .arg(read.half ? QStringLiteral("rgba16f") : QStringLiteral("rgba8"), Color::describe(layer.hdr.transfer));
    }
    QFile listFile(dir + QStringLiteral("/layers.txt"));
    if (listFile.open(QIODevice::WriteOnly)) {
        listFile.write(list.toUtf8());
    }
    qCInfo(lcXr).noquote() << "screenshot: layer dump," << saved << "layer(s) to" << dir;
}

void XrCompositor::Impl::saveScreenshot()
{
    const QString stem = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + QStringLiteral("/RiftHome/screenshots/")
        + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    if (!QDir().mkpath(QFileInfo(stem).absolutePath())) {
        return;
    }
    if (qEnvironmentVariableIntValue("RIFT_HOME_SCREENSHOT_LAYERS") > 0) {
        saveLayerDump(stem);
    }
    // What the user sees: the frame's layers in the order they went out (back to front), each with the pixels of its
    // image read back once (the cursor and the art are shown more than once), the controllers left out.
    struct Job {
        VkDevice device = VK_NULL_HANDLE;
        Xr::ViewShot::Camera camera;
        std::vector<Xr::ViewShot::Layer> layers;
        std::vector<size_t> source; //!< for each layer, its read-back
        std::vector<Readback> reads;
        std::vector<Xr::ViewShot::Decode> decodes;
        QString path;
    };
    auto job = std::make_shared<Job>();
    job->device = device;
    job->path = stem + QStringLiteral(".png");
    job->camera.pose = roomHead;
    job->camera.horizontalFovDegrees = 100.0f;
    job->camera.width = 2560;
    job->camera.height = 1600;
    std::unordered_map<const SurfaceLayer *, size_t> known;
    for (const ShotLayer &shot : shotLayers) {
        const SurfaceLayer &layer = *shot.layer;
        if (!layer.hasImage || layer.passImage == VK_NULL_HANDLE || layer.width <= 0 || layer.height <= 0) {
            continue;
        }
        auto [it, added] = known.try_emplace(&layer, job->reads.size());
        if (added) {
            Readback read;
            if (!readBack(layer, read)) {
                known.erase(it);
                continue;
            }
            Xr::ViewShot::Decode decode;
            const bool pq = hdrExtension && layer.hdr.transfer == Color::LayerTransfer::Pq;
            if (read.half) {
                decode.encoding = pq ? Xr::ViewShot::Encoding::HalfPq : Xr::ViewShot::Encoding::HalfLinear;
            } else {
                decode.encoding = layer.format == VK_FORMAT_R8G8B8A8_SRGB ? Xr::ViewShot::Encoding::Srgb8 : Xr::ViewShot::Encoding::Unorm8;
            }
            decode.referenceWhiteNits = hdrExtension ? layer.hdr.referenceWhiteNits : 0.0f;
            decode.bt2020 = hdrExtension && layer.hdr.primaries == Color::LayerPrimaries::Bt2020;
            job->reads.push_back(read);
            job->decodes.push_back(decode);
        }
        Xr::ViewShot::Layer out;
        out.kind = shot.kind;
        out.pose = shot.pose;
        out.headLocked = shot.headLocked;
        out.width = shot.width;
        out.height = shot.height;
        out.radius = shot.radius;
        out.centralAngle = shot.centralAngle;
        out.aspectRatio = shot.aspectRatio;
        out.upperAngle = shot.upperAngle;
        out.lowerAngle = shot.lowerAngle;
        out.blend = layer.alpha;
        out.blurRadians = shot.blurRadians;
        out.blurSaturation = shot.blurSaturation;
        std::copy(std::begin(shot.blurTint), std::end(shot.blurTint), std::begin(out.blurTint));
        out.blurMaskAlpha = shot.blurMaskAlpha;
        std::copy(std::begin(shot.scale), std::end(shot.scale), std::begin(out.colorScale));
        job->layers.push_back(out);
        job->source.push_back(it->second);
    }
    // Decoded, drawn and written off the frame thread (a 2560x1600 view of dozens of layers takes a few seconds).
    // The runtime's picture where it takes one (exactly what the headset shows: every layer, its frosted glass and HDR
    // mapping); else Rift Home's own from the layers read back above. The runtime renders from its next frame, so the
    // "saved" card waits for the file, or it would be in the picture.
    screenshotPool->start([job, this, apps = runtimeApps] {
        if (apps && apps->canTakeScreenshot()) {
            if (apps->takeScreenshot(job->path.toStdString(), std::chrono::seconds(5))) {
                for (const Readback &read : job->reads) {
                    freeLater([device = job->device, read] {
                        vkFreeMemory(device, read.memory, nullptr);
                        vkDestroyBuffer(device, read.buffer, nullptr);
                    });
                }
                screenshotSaved(job->path);
                return;
            }
            qCInfo(lcXr) << "screenshot: the runtime could not take it, Rift Home draws its own";
        }
        std::vector<std::shared_ptr<const Xr::ViewShot::Texture>> textures;
        for (size_t i = 0; i < job->reads.size(); ++i) {
            const Readback &read = job->reads[i];
            void *data = nullptr;
            auto texture = std::make_shared<Xr::ViewShot::Texture>();
            if (vkMapMemory(job->device, read.memory, 0, read.size, 0, &data) == VK_SUCCESS) {
                *texture = Xr::ViewShot::decodePixels(data, read.width, read.height, job->decodes[i]);
                vkUnmapMemory(job->device, read.memory);
            }
            textures.push_back(std::move(texture));
            freeLater([device = job->device, read] {
                vkFreeMemory(device, read.memory, nullptr);
                vkDestroyBuffer(device, read.buffer, nullptr);
            });
        }
        for (size_t i = 0; i < job->layers.size(); ++i) {
            job->layers[i].texture = textures[job->source[i]];
        }
        const QImage picture = Xr::ViewShot::toSrgb8(Xr::ViewShot::render(job->camera, job->layers));
        const bool ok = picture.save(job->path);
        qCInfo(lcXr).noquote() << "screenshot:" << (ok ? "saved" : "could not save") << job->path << "from" << job->layers.size() << "layer(s)";
        if (ok) {
            screenshotSaved(job->path);
        }
    });
    for (int h = 0; h < 2; ++h) {
        input.haptic(h, 0.5f, 80'000'000);
    }
}

void XrCompositor::Impl::screenshotSaved(const QString &path)
{
    std::lock_guard lock(savedShot->mutex);
    savedShot->path = path;
}

void XrCompositor::Impl::logStats()
{
    const auto now = Clock::now();
    if (now - statsSince < std::chrono::seconds(10)) {
        return;
    }
    int filtered = 0;
    int enlarged = 0;
    // What the layers hold (browsers make and drop video surfaces all the time: these must come back down).
    int swapchains = 0;
    double swapchainMegabytes = 0.0; // the runtime's images, made for Rift Home
    double imageMegabytes = 0.0; // Rift Home's own: pass, filtered and shared-memory source images, staging
    for (const auto &[id, layer] : layers) {
        filtered += layer.hasImage && layer.level > 0;
        enlarged += layer.hasImage && layer.level == Spatial::Sharpness::kUpLevel;
        const auto megabytes = [](VkFormat format, int width, int height) {
            return (format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8.0 : 4.0) * width * height / 1e6;
        };
        if (layer.swapchain != XR_NULL_HANDLE) {
            ++swapchains;
            swapchainMegabytes += megabytes(layer.format, layer.swapWidth, layer.swapHeight) * double(layer.images.size());
        }
        imageMegabytes += layer.passImage != VK_NULL_HANDLE ? megabytes(layer.storage, layer.width, layer.height) : 0.0;
        imageMegabytes += layer.scaledImage != VK_NULL_HANDLE ? megabytes(layer.storage, layer.scaledWidth, layer.scaledHeight) : 0.0;
        imageMegabytes += layer.sourceImage != VK_NULL_HANDLE ? megabytes(layer.sourceFormat, layer.sourceWidth, layer.sourceHeight) : 0.0;
        imageMegabytes += double(layer.stagingSize) / 1e6;
    }
    qCInfo(lcXr).noquote() << QStringLiteral("10 s: %1 frames (%2/s), %3 layer(s), %4 copies, %5 waited for the client's GPU, %6 not shown%7%8%9")
                                  .arg(frames)
                                  .arg(frames / 10.0, 0, 'f', 1)
                                  .arg(lastLayerCount)
                                  .arg(copies)
                                  .arg(deferred)
                                  .arg(notShown)
                                  .arg(keysTyped ? QStringLiteral(", %1 key(s) typed").arg(keysTyped) : QString())
                                  .arg(skipped ? QStringLiteral(", %1 out of view (not copied)").arg(skipped) : QString())
                                  .arg(filtered || enlarged || rescaled
                                           ? QStringLiteral(", %1 shown filtered, %2 enlarged (%3 refiltered)").arg(filtered).arg(enlarged).arg(rescaled)
                                           : QString())
                           << QStringLiteral("| held: %1 layers, %2 swapchains (%3 MB) + %4 spare (%5 MB), images %6 MB, %7 client buffers imported")
                                  .arg(layers.size())
                                  .arg(swapchains)
                                  .arg(swapchainMegabytes, 0, 'f', 0)
                                  .arg(spareSwapchains.size())
                                  .arg(std::accumulate(spareSwapchains.begin(), spareSwapchains.end(), 0.0,
                                                       [](double sum, const SpareSwapchain &spare) { return sum + spare.megabytes(); }),
                                       0, 'f', 0)
                                  .arg(imageMegabytes, 0, 'f', 0)
                                  .arg(importCache.size());
    if (frames > 0) {
        const auto stage = [this](const StageTime &t) {
            return QStringLiteral("%1/%2").arg(t.sum / frames, 0, 'f', 2).arg(t.most, 0, 'f', 1);
        };
        qCInfo(lcXr).noquote() << QStringLiteral("10 s: frame work ms (mean/most) %1: copies %2 (GPU wait %3), controllers %4, xrEndFrame %5, rest %6; %7 frame(s) over 9 ms")
                                      .arg(stage(workTime), stage(copyTime), stage(copyWaitTime), stage(controllerTime), stage(endTime), stage(otherTime))
                                      .arg(slowFrames)
                               << (swapchainCount ? QStringLiteral("| %1 swapchain(s) made, %2 ms, most %3").arg(swapchainCount).arg(swapchainTime.sum, 0, 'f', 1).arg(swapchainTime.most, 0, 'f', 1)
                                                  : QString())
                               << (swapchainsReused ? QStringLiteral("| %1 reused").arg(swapchainsReused) : QString());
        const auto [offFrame, offFrameMs, offFrameMost] = importer ? importer->takeStats() : std::tuple<int, double, double>{};
        qCInfo(lcXr).noquote() << QStringLiteral("10 s: copies' parts ms (mean/most): layers destroyed %1, spare swapchains destroyed %2, swapchain image waits %3, dmabuf imports %4, recording %5 (imports in it), finishing %6")
                                      .arg(stage(destroyTime), stage(spareTime), stage(imageWaitTime), stage(importTime), stage(recordTime), stage(finishTime))
                               << (offFrame ? QStringLiteral("| %1 imported off the frame thread, %2 ms, most %3").arg(offFrame).arg(offFrameMs, 0, 'f', 1).arg(offFrameMost, 0, 'f', 1)
                                            : QString());
    }
    workTime = copyTime = copyWaitTime = controllerTime = endTime = otherTime = swapchainTime = StageTime{};
    destroyTime = imageWaitTime = importTime = recordTime = finishTime = spareTime = StageTime{};
    swapchainsReused = 0;
    swapchainCount = 0;
    slowFrames = 0;
    frames = copies = deferred = notShown = keysTyped = skipped = rescaled = 0;
    statsSince = now;
}

bool XrCompositor::Impl::actionHeld(int hand, Spatial::ButtonAction action, bool stickQuiet) const
{
    using Spatial::ControllerInput;
    using Spatial::inputBit;
    const Spatial::InputMask sticks = inputBit(ControllerInput::LeftStick) | inputBit(ControllerInput::RightStick);
    // The hand a binding belongs to: that of its buttons (grips do not say: they only join a chord).
    const Spatial::InputMask leftHand = inputBit(ControllerInput::LeftPrimary) | inputBit(ControllerInput::LeftSecondary)
        | inputBit(ControllerInput::LeftMenu) | inputBit(ControllerInput::LeftStick);
    const Spatial::InputMask rightHand = inputBit(ControllerInput::RightPrimary) | inputBit(ControllerInput::RightSecondary) | inputBit(ControllerInput::RightStick);
    for (const auto &[heldAction, binding] : bindingResult.heldBy) {
        const Spatial::InputMask mask = binding.mask();
        if (heldAction != action || !(mask & (hand == 0 ? leftHand : rightHand))) {
            continue;
        }
        if (!(mask & sticks) || stickQuiet) {
            return true; // a thumbstick's press counts unless the stick was just scrolling
        }
    }
    return false;
}

void XrCompositor::Impl::performAction(LayerBridge &bridge, Spatial::ButtonAction action)
{
    using Spatial::ButtonAction;
    switch (action) {
    case ButtonAction::Screenshot:
        screenshotPending = true;
        break;
    case ButtonAction::Keyboard:
        if (keyboardOpen) {
            closeKeyboard(bridge);
        } else {
            openKeyboard(false, Spatial::azimuthOf(head.forward()));
        }
        break;
    case ButtonAction::Recline:
        // Lying back, it needs no looking at the dock: the arrangement turns to where the user looks now.
        setRecline(!reclined, roomHead);
        break;
    case ButtonAction::FollowAll:
        performDock(bridge, 0, Spatial::DockItem::FollowAll);
        break;
    case ButtonAction::GameMode:
        performDock(bridge, 0, Spatial::DockItem::Gamepad);
        break;
    case ButtonAction::Settings:
        performDock(bridge, 0, Spatial::DockItem::Settings);
        break;
    case ButtonAction::Notifications:
        performDock(bridge, 0, Spatial::DockItem::Notifications);
        break;
    case ButtonAction::VolumeUp:
        performDock(bridge, 0, Spatial::DockItem::VolumeUp);
        break;
    case ButtonAction::VolumeDown:
        performDock(bridge, 0, Spatial::DockItem::VolumeDown);
        break;
    case ButtonAction::Mute:
        performDock(bridge, 0, Spatial::DockItem::Volume);
        break;
    default:
        break; // Home and the dock have their own handling below (their toggles are rate limited); the clicks are held
    }
}

void XrCompositor::Impl::updateLensTest(XrTime displayTime)
{
    if (std::exchange(lensTestCommand, false) && lensTest == LensTest::Off) {
        lensTest = LensTest::Arming;
        lensTestUntil = Clock::now() + kLensTestLongest;
        lensDensity = 0.0f;
        lensSpacing = -1;
        // The cross pixel for pixel on the panel, as Oculus draws it, where the runtime can (RiftLinux on a CV1): drawn
        // here, it reaches the panel through two resamplings, its lines farther apart.
        lensRuntimeCross = runtimeApps && runtimeApps->canDrawLensPattern();
        if (lensRuntimeCross) {
            runtimeApps->setLensPattern(true);
        }
        qCInfo(lcXr).noquote() << "lens test: shown until a button is pressed, the cross drawn by"
                               << (lensRuntimeCross ? QStringLiteral("the runtime") : QStringLiteral("Rift Home"));
    }
    lensEyesValid = false;
    if (lensTest == LensTest::Off) {
        return;
    }
    if (lensRuntimeCross && !runtimeApps->canDrawLensPattern()) {
        lensRuntimeCross = false; // it refused: drawn here after all
        lensSpacing = -1;
    }
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = displayTime;
    locate.space = viewSpace;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t located = 0;
    const XrViewStateFlags needed = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if (XR_FAILED(xrLocateViews(session, &locate, &viewState, 2, &located, views)) || located != 2 || (viewState.viewStateFlags & needed) != needed) {
        return;
    }
    for (int eye = 0; eye < 2; ++eye) {
        lensEyes[eye] = toPose(views[eye].pose);
    }
    lensEyesValid = true;
    if (lensDensity <= 0.0f) {
        // The runtime's 1:1 size at the lens centre (as for sharpness at a distance): the recommended width over the
        // views' tangent width. A line a pixel wide in the picture is then a pixel of the panel there.
        float tangents = 0.0f;
        for (const XrView &view : views) {
            tangents += (std::tan(view.fov.angleRight) - std::tan(view.fov.angleLeft)) / 2.0f;
        }
        lensDensity = eyeWidth > 0 && tangents > 0.1f ? float(eyeWidth) / tangents : 800.0f;
    }
    // The lens spacing: how far apart the runtime puts the eyes, which follows the CV1's lens slider (RiftLinux: the
    // slider's reading, or the distance set by hand). Painted again when it moves by a millimetre (not on a reading
    // that flickers at a half).
    const float spacing = (lensEyes[1].position - lensEyes[0].position).length() * 1000.0f;
    if (lensSpacing < 0 || std::fabs(spacing - float(lensSpacing)) > 0.7f) {
        lensSpacing = int(std::lround(spacing));
        paintArt(kLensTestLayer, &FrameArt::lensTest, lensDensity, lensSpacing, !lensRuntimeCross);
        qCInfo(lcXr).noquote() << QStringLiteral("lens test: lens spacing %1 mm (the panel's %2 px a radian)").arg(lensSpacing).arg(lensDensity, 0, 'f', 0);
    }
}

void XrCompositor::Impl::endLensTest(const char *why)
{
    lensTest = LensTest::Off;
    lensEyesValid = false;
    if (std::exchange(lensRuntimeCross, false)) {
        runtimeApps->setLensPattern(false);
    }
    cancelArt(kLensTestLayer);
    destroyLayer(kLensTestLayer);
    qCInfo(lcXr) << "lens test ended:" << why;
}

void XrCompositor::Impl::endRecording(const QString &binding)
{
    recordingBinding = false;
    bindingRecorder.reset();
    inputsQuiet = true; // what is still held does nothing
    qCInfo(lcXr).noquote() << "binding recorded:" << (binding.isEmpty() ? QStringLiteral("none") : binding);
    if (bindingRecorded) {
        bindingRecorded(binding);
    }
}

void XrCompositor::Impl::interact(LayerBridge &bridge, XrTime displayTime, float dt)
{
    const bool real = inputReady && focused && !options.deskView && input.sync(space, displayTime, hands);
    if (!real) {
        hands[0] = hands[1] = XrInput::Hand{};
    }
    for (int h = 0; h < 2; ++h) {
        if (hands[h].tracked) {
            // Smoothed in the room, then into the arrangement's space (recline): pointing meets the windows where they
            // are shown. The grip stays in the room: the controller is drawn where it is.
            hands[h].aim = recline.inverse() * rayFilters[h].filter(hands[h].aim, dt);
        } else {
            rayFilters[h].reset();
        }
    }
    const bool scripted = options.selfTest && selfTestHand(hands[1]);
    if (selfTestMenu) {
        performMenu(bridge, 1, selfTestWindow, *selfTestMenu);
        selfTestMenu.reset();
    }
    if (selfTestDock) {
        performDock(bridge, 1, *selfTestDock);
        selfTestDock.reset();
    }
    // Game mode: the controllers are a gamepad for the game; nothing else sees them, no rays.
    if (gameMode) {
        const XrInput::Hand &l = hands[0];
        const XrInput::Hand &r = hands[1];
        Spatial::GamepadState pad = Spatial::gamepadFrom({l.stickX, l.stickY, l.trigger, l.squeeze, l.dock, l.home, l.stickClick},
                                                          {r.stickX, r.stickY, r.trigger, r.squeeze, r.primary, r.secondary, r.stickClick});
        // The left menu button: a tap is Start; held a second it leaves game mode.
        const auto now = Clock::now();
        if (l.menu && !gameLeaving) {
            if (!gameMenuDownAt) {
                gameMenuDownAt = now;
                gameHoldHinted = false;
            } else if (!gameHoldHinted && now - *gameMenuDownAt > std::chrono::milliseconds(300)) {
                gameHoldHinted = true;
                showHint(QStringLiteral("Keep holding \u2630"), QStringLiteral("Game mode stops at one second."), 1);
            }
            if (gameMenuDownAt && now - *gameMenuDownAt > std::chrono::milliseconds(1000)) {
                gameLeaving = true;
                gameMenuDownAt.reset();
                setGameMode(bridge, false);
            }
        } else if (!l.menu) {
            if (gameMenuDownAt && now - *gameMenuDownAt < std::chrono::milliseconds(600)) {
                gameStartFrames = 6;
            }
            gameMenuDownAt.reset();
        }
        pad.start = gameStartFrames > 0;
        gameStartFrames = std::max(0, gameStartFrames - 1);
        if (gameMode) {
            gamepad.send(pad);
            for (int h = 0; h < 2; ++h) {
                hands[h] = XrInput::Hand{}; // no rays, no pointing, no dock or home buttons meanwhile
            }
        }
    }
    if (gameLeaving) {
        // The hold that left game mode: its button and the rest stay quiet until let go (home would open on it).
        if (!hands[0].menu) {
            gameLeaving = false;
        }
        hands[0].menu = false;
        hands[0].home = false;
    }
    // The lens test: any button ends it, once what was held when it opened is let go. Meanwhile the controllers point
    // at nothing and press nothing, and after it nothing until all is let go (the press that ended it would click on
    // what is behind: Settings).
    if (lensTest != LensTest::Off || lensTestQuiet) {
        bool held = false;
        for (const XrInput::Hand &in : hands) {
            held = held || in.trigger > 0.3f || in.squeeze > 0.3f || in.primary || in.secondary || in.dock || in.home || in.menu
                || in.stickClick;
        }
        if (lensTest == LensTest::Arming && !held) {
            lensTest = LensTest::Shown;
        } else if (lensTest == LensTest::Shown && held) {
            endLensTest("a button pressed");
            lensTestQuiet = true;
        } else if (lensTest != LensTest::Off && Clock::now() > lensTestUntil) {
            endLensTest("no button pressed in two minutes");
            lensTestQuiet = held;
        } else if (lensTest == LensTest::Off && !held) {
            lensTestQuiet = false;
        }
        for (XrInput::Hand &in : hands) {
            if (lensTest != LensTest::Off) {
                in.tracked = false; // no ray, nothing pointed at
            }
            in.trigger = in.squeeze = in.stickX = in.stickY = 0.0f;
            in.primary = in.secondary = in.dock = in.home = in.menu = in.stickClick = false;
        }
    }
    // The buttons as the user set them (Settings, Controllers; outside game mode, which zeroed them above). A grip is an
    // input only in a chord (alone it grabs, as ever); the triggers are not inputs (they click, and are how Settings is used).
    if (!effectiveReady || !(effectiveFrom == options.buttons)) {
        effectiveFrom = options.buttons;
        effectiveButtons = options.buttons.effective();
        effectiveReady = true;
    }
    {
        using Spatial::ControllerInput;
        for (int h = 0; h < 2; ++h) {
            gripDown[h] = hands[h].squeeze > (gripDown[h] ? 0.4f : 0.6f);
        }
        const bool inputs[Spatial::kControllerInputs] = {hands[0].dock, hands[0].home, hands[0].menu, hands[0].stickClick, gripDown[0],
                                                         hands[1].primary, hands[1].secondary, hands[1].stickClick, gripDown[1]};
        inputsDown = 0;
        for (std::size_t i = 0; i < Spatial::kControllerInputs; ++i) {
            if (inputs[i]) {
                inputsDown |= Spatial::inputBit(ControllerInput(i));
            }
        }
    }
    if (const int command = std::exchange(recordCommand, 0)) {
        if (command == 1 && !recordingBinding) {
            recordingBinding = true;
            recordingDeadline = Clock::now() + std::chrono::seconds(10);
            bindingRecorder.reset();
            qCInfo(lcXr) << "recording a binding: press a button, or two together";
        } else if (command == 2 && recordingBinding) {
            endRecording({});
        }
    }
    if (recordingBinding) {
        // No button does anything meanwhile; the trigger still clicks (Cancel is pressed with it).
        bindingTracker.reset();
        bindingResult = {};
        if (const auto binding = bindingRecorder.update(inputsDown)) {
            endRecording(binding->name());
        } else if (Clock::now() > recordingDeadline) {
            endRecording({});
        }
    } else {
        if (inputsQuiet) {
            inputsQuiet = inputsDown != 0;
            inputsDown = 0;
        }
        bindingResult = bindingTracker.update(effectiveButtons, inputsDown);
    }
    // A binding given the left click is the trigger pulled: everything that takes the trigger takes it.
    for (int h = 0; h < 2; ++h) {
        if (actionHeld(h, Spatial::ButtonAction::LeftClick, true)) {
            hands[h].trigger = 1.0f;
        }
    }
    for (std::size_t a = 0; a < Spatial::kButtonActions; ++a) {
        if (bindingResult.isPressed(Spatial::ButtonAction(a))) {
            performAction(bridge, Spatial::ButtonAction(a));
        }
    }
    // A screenshot written (on the screenshot pool): said in the room now, after the picture was taken.
    {
        QString saved;
        {
            std::lock_guard lock(savedShot->mutex);
            saved.swap(savedShot->path);
        }
        if (!saved.isEmpty()) {
            showHint(QStringLiteral("Screenshot saved"), QStringLiteral("%1 in Pictures/RiftHome/screenshots").arg(QFileInfo(saved).fileName()), 3);
        }
    }
    if (screenshotInFrames > 0 && --screenshotInFrames == 0) {
        screenshotPending = true;
    }
    // Rays out of the way while the controllers rest (Settings: hidden after so many seconds; they get
    // in the way of films). Hidden, a hand points at nothing (frames and players' controls fade as for a hand put down).
    // Moving it or any button brings its ray back. A trigger pull that wakes it clicks where it still points (the
    // right trigger sometimes did nothing: the hand had held still on what it meant to click); only a
    // controller picked up and pulled at once (moved too) only wakes it.
    for (int h = 0; h < 2; ++h) {
        HandRest &rest = handRest[h];
        if (!hands[h].tracked) {
            continue;
        }
        const auto now = Clock::now();
        const Spatial::Vec3 aimDirection = hands[h].aim.forward();
        const XrInput::Hand &in = hands[h];
        const bool moved = std::acos(std::clamp(aimDirection.dot(rest.direction), -1.0f, 1.0f)) > Spatial::radians(1.5f)
            || (in.aim.position - rest.position).length() > 0.015f;
        const bool pressed = in.trigger > 0.2f || in.squeeze > 0.2f || in.primary || in.stickClick || in.dock || in.home || in.menu
            || std::fabs(in.stickX) > 0.2f || std::fabs(in.stickY) > 0.2f;
        if (moved || pressed) {
            if (rest.hidden) {
                rest.swallowTrigger = in.trigger > 0.2f && moved;
                qCInfo(lcXr) << "pointer" << h << "shown again";
            }
            rest.hidden = false;
            rest.activeAt = now;
            rest.direction = aimDirection;
            rest.position = in.aim.position;
        }
        if (options.pointerHideSeconds > 0 && !(scripted && h == 1) && !rest.hidden
            && now - rest.activeAt > std::chrono::seconds(options.pointerHideSeconds)) {
            rest.hidden = true;
            qCInfo(lcXr) << "pointer" << h << "hidden (still for" << options.pointerHideSeconds << "s)";
        }
        if (rest.swallowTrigger) {
            if (hands[h].trigger < 0.05f) {
                rest.swallowTrigger = false;
            } else {
                hands[h].trigger = 0.0f;
            }
        }
        if (rest.hidden) {
            hands[h].tracked = false;
        }
    }
    const uint32_t timeMs = uint32_t(displayTime / 1000000);
    if (!real && !scripted) {
        if (pointerWindow) {
            bridge.sendInput({PointerEvent::Type::Leave});
            pointerWindow = pressedWindow = 0;
        }
        hits[0].reset();
        hits[1].reset();
        chromeHits[0].reset();
        chromeHits[1].reset();
        if (grabHand >= 0) {
            layout.endGrab();
            grabHand = -1;
            grabByTrigger = false;
        }
        if (chromeDrag) {
            layout.endDrag();
            chromeDrag.reset();
        }
        for (int h = 0; h < 2; ++h) {
            keyHits[h].reset();
            if (keyHeld[h] >= 0) {
                sendKeys(bridge, keyboard.release(keyHeld[h]));
                keyHeld[h] = -1;
            }
        }
        return;
    }

    // The hand whose trigger or grip went down last points (not while a click is held).
    for (int h = 0; h < 2; ++h) {
        const bool down = (hands[h].trigger > 0.6f && !handState[h].trigger) || (hands[h].squeeze > 0.6f && !handState[h].squeeze);
        if (down && pressedWindow == 0) {
            activeHand = h;
        }
    }
    for (int h = 0; h < 2; ++h) {
        hits[h].reset();
        chromeHits[h].reset();
        chromeUnder[h].reset();
        if (!hands[h].tracked) {
            continue;
        }
        const Spatial::Vec3 origin = hands[h].aim.position;
        const Spatial::Vec3 direction = hands[h].aim.forward();
        // A click keeps its window until released, even when the ray leaves it (a drag, a text selection); but a drag and
        // drop goes to whatever window the ray is on (the drop lands there).
        const bool clickHeld = h == activeHand && pressedWindow && !bridge.dragging();
        // Home open: the windows are faded out, out of reach; cinema: only its window, and Settings over it.
        hits[h] = clickHeld ? layout.projectOnto(pressedWindow, origin, direction) : layout.hitTest(origin, direction, layout.cinemaWindow());
        if (!clickHeld && layout.cinemaWindow() && settingsWindow()) {
            const auto onSettings = layout.hitTest(origin, direction, settingsWindow());
            if (onSettings && (!hits[h] || onSettings->distance < hits[h]->distance)) {
                hits[h] = onSettings;
            }
        }
        if (launcherOpen) {
            hits[h].reset();
            continue;
        }
        if (clickHeld) {
            continue;
        }
        chromeUnder[h] = chromeHitTest(origin, direction);
        if (chromeDrag && chromeDrag->hand == h) {
            // Dragging a frame control: the ray is on its window's plane, not on any app.
            hits[h].reset();
            if (const auto onPlane = layout.projectOnto(chromeDrag->windowId, origin, direction)) {
                chromeHits[h] = ChromeHit{chromeDrag->windowId, chromeDrag->control, onPlane->distance, onPlane->point, onPlane->localX, onPlane->localY};
            }
        } else if (chromeUnder[h] && grabHand != h && (!hits[h] || chromeUnder[h]->distance < hits[h]->distance)) {
            hits[h].reset(); // on a frame control, nearer than any app under the ray
            chromeHits[h] = chromeUnder[h];
        }
    }

    // The keyboard: solid, nearer than the windows behind it; a key held keeps its hand on the keyboard.
    for (int h = 0; h < 2; ++h) {
        keyHits[h].reset();
        const bool busy = (h == activeHand && pressedWindow) || grabHand == h || (chromeDrag && chromeDrag->hand == h);
        if (!keyboardOpen || !hands[h].tracked || busy || (launcherOpen && !homeSearching)) {
            continue;
        }
        if (keyboardGrab && keyboardGrab->hand == h) {
            // Moving it: the ray is on it, not on what is behind.
            keyHits[h] = KeyHit{-1, keyboardGrab->distance, hands[h].aim.position + hands[h].aim.forward() * keyboardGrab->distance, true};
            hits[h].reset();
            chromeHits[h].reset();
            chromeUnder[h].reset();
            continue;
        }
        const auto onKeyboard = keyboardHitTest(hands[h].aim.position, hands[h].aim.forward());
        if (!onKeyboard) {
            continue;
        }
        const float nearest = std::min(hits[h] ? hits[h]->distance : 1e9f, chromeHits[h] ? chromeHits[h]->distance : 1e9f);
        if (keyHeld[h] >= 0 || onKeyboard->distance < nearest) {
            keyHits[h] = onKeyboard;
            hits[h].reset();
            chromeHits[h].reset();
            chromeUnder[h].reset();
        }
    }

    // Home on the left menu button (and Y); solid too, the nearer of it and the rest takes the ray. The menu button's
    // presses are logged next to the session's state changes (they take no focus; the Rift panel on
    // the right Oculus button does, while it is open).
    const bool toggleReady = Clock::now() - lastToggle > std::chrono::milliseconds(400);
    if (hands[0].menu != lastMenu) {
        if (hands[0].menu) {
            menuPressedAt = Clock::now();
            qCInfo(lcXr) << "menu button: pressed (home; runtime time" << hands[0].menuChanged / 1000000 << "ms)";
        } else {
            qCInfo(lcXr) << "menu button: released after"
                         << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - menuPressedAt).count()
                         << "ms (runtime time" << hands[0].menuChanged / 1000000 << "ms)";
        }
    }
    const bool homePressed = bindingResult.isPressed(Spatial::ButtonAction::Home);
    if (homePressed && toggleReady) {
        lastToggle = Clock::now();
        openLauncher(!launcherOpen);
    }
    lastMenu = hands[0].menu;
    for (int h = 0; h < 2; ++h) {
        cellHits[h].reset();
        const bool busy = (h == activeHand && pressedWindow) || grabHand == h || (chromeDrag && chromeDrag->hand == h) || keyHeld[h] >= 0;
        if (!launcherOpen || !hands[h].tracked || busy) {
            continue;
        }
        const auto onLauncher = launcherHitTest(hands[h].aim.position, hands[h].aim.forward());
        if (!onLauncher) {
            continue;
        }
        const float nearest = std::min({hits[h] ? hits[h]->distance : 1e9f, chromeHits[h] ? chromeHits[h]->distance : 1e9f,
                                        keyHits[h] ? keyHits[h]->distance : 1e9f});
        if (cellPressed[h] >= 0 || onLauncher->distance < nearest) {
            cellHits[h] = onLauncher;
            hits[h].reset();
            chromeHits[h].reset();
            chromeUnder[h].reset();
            keyHits[h].reset();
        }
    }

    // A drag and drop: a tap when it starts and when it drops (with the badge beside the cursor).
    if (bridge.dragging() != wasDragging) {
        wasDragging = bridge.dragging();
        input.haptic(activeHand, wasDragging ? 0.4f : 0.25f, wasDragging ? 20'000'000 : 12'000'000);
        qCInfo(lcXr) << (wasDragging ? "drag: carrying" : "drag: dropped");
    }
    // The dock: X shows and hides it; solid as the rest, the nearest takes the ray.
    if (bindingResult.isPressed(Spatial::ButtonAction::Dock) && Clock::now() - lastToggle > std::chrono::milliseconds(400)) {
        lastToggle = Clock::now();
        openDock(!dockOpen);
    }
    for (int h = 0; h < 2; ++h) {
        dockHits[h].reset();
        const bool busy = (h == activeHand && pressedWindow) || grabHand == h || (chromeDrag && chromeDrag->hand == h) || keyHeld[h] >= 0
            || cellPressed[h] >= 0 || (keyboardGrab && keyboardGrab->hand == h);
        if (!dockOpen || !hands[h].tracked || busy) {
            continue;
        }
        if (dockGrab && dockGrab->hand == h) {
            // Moving it: the ray is on it, not on what is behind.
            dockHits[h] = DockHit{Spatial::DockItem::None, dockGrab->distance, hands[h].aim.position + hands[h].aim.forward() * dockGrab->distance, true};
            hits[h].reset();
            chromeHits[h].reset();
            chromeUnder[h].reset();
            keyHits[h].reset();
            cellHits[h].reset();
            continue;
        }
        const auto onDock = dockHitTest(hands[h].aim.position, hands[h].aim.forward());
        if (!onDock) {
            continue;
        }
        const float nearest = std::min({hits[h] ? hits[h]->distance : 1e9f, chromeHits[h] ? chromeHits[h]->distance : 1e9f,
                                        keyHits[h] ? keyHits[h]->distance : 1e9f, cellHits[h] ? cellHits[h]->distance : 1e9f});
        if (dockPressed[h] != Spatial::DockItem::None || onDock->distance < nearest) {
            dockHits[h] = onDock;
            hits[h].reset();
            chromeHits[h].reset();
            chromeUnder[h].reset();
            keyHits[h].reset();
            cellHits[h].reset();
        }
    }

    // Notifications: the card and the dock's list, solid as the rest, the nearest takes the ray.
    for (int h = 0; h < 2; ++h) {
        noteHits[h].reset();
        const bool busy = (h == activeHand && pressedWindow) || grabHand == h || (chromeDrag && chromeDrag->hand == h) || keyHeld[h] >= 0
            || cellPressed[h] >= 0 || (keyboardGrab && keyboardGrab->hand == h) || (dockGrab && dockGrab->hand == h)
            || dockPressed[h] != Spatial::DockItem::None;
        if (!hands[h].tracked || busy) {
            continue;
        }
        const auto onNote = noteHitTest(hands[h].aim.position, hands[h].aim.forward());
        if (!onNote) {
            continue;
        }
        const float nearest = std::min({hits[h] ? hits[h]->distance : 1e9f, chromeHits[h] ? chromeHits[h]->distance : 1e9f,
                                        keyHits[h] ? keyHits[h]->distance : 1e9f, cellHits[h] ? cellHits[h]->distance : 1e9f,
                                        dockHits[h] ? dockHits[h]->distance : 1e9f});
        if (notePressed[h] || onNote->distance < nearest) {
            noteHits[h] = onNote;
            hits[h].reset();
            chromeHits[h].reset();
            chromeUnder[h].reset();
            keyHits[h].reset();
            cellHits[h].reset();
            dockHits[h].reset();
        }
    }

    // The start of a trigger pull, remembered for the click (see HandState::armed).
    for (int h = 0; h < 2; ++h) {
        HandState &state = handState[h];
        const float value = hands[h].trigger;
        if (value > 0.1f && state.lastTrigger <= 0.1f && hits[h]) {
            state.armed = HandState::Armed{hits[h]->windowId, hits[h]->x, hits[h]->y, hands[h].aim.forward(), Clock::now()};
        } else if (value < 0.05f) {
            state.armed.reset();
        }
        state.lastTrigger = value;
    }

    // Grip: grab the window the ray is on (or whose frame it is on); while held the thumbstick moves it
    // nearer/farther and resizes it.
    for (int h = 0; h < 2; ++h) {
        HandState &state = handState[h];
        const int hover = hits[h] ? hits[h]->windowId : 0;
        if (hover && hover != state.hoverWindow) {
            input.haptic(h, 0.15f, 8'000'000);
        }
        state.hoverWindow = hover;
        const Spatial::FrameControl control = chromeHits[h] && !chromeDrag ? chromeHits[h]->control : Spatial::FrameControl::None;
        if (control != Spatial::FrameControl::None && control != hapticControl[h]) {
            input.haptic(h, 0.12f, 6'000'000);
        }
        hapticControl[h] = control;
        if (hands[h].squeeze > 0.6f && !state.squeeze) {
            state.squeeze = true;
            const int target = hits[h] ? hits[h]->windowId : chromeHits[h] ? chromeHits[h]->windowId : 0;
            const float distance = hits[h] ? hits[h]->distance : chromeHits[h] ? chromeHits[h]->distance : 0.0f;
            if (target && grabHand < 0 && !chromeDrag && layout.beginGrab(target, hands[h].aim, distance)) {
                grabHand = h;
                grabByTrigger = false;
                input.haptic(h, 0.5f, 25'000'000);
                qCInfo(lcXr) << "grabbed window" << target;
            }
        } else if (hands[h].squeeze < 0.4f && state.squeeze) {
            state.squeeze = false;
            if (grabHand == h && !grabByTrigger) {
                qCInfo(lcXr) << "let go of window" << layout.grabbedWindow();
                layout.endGrab();
                grabHand = -1;
            }
        }
    }

    // Trigger on a frame control: grab bar, scale handle and tilt bar drag while held; menu buttons act on release.
    for (int h = 0; h < 2; ++h) {
        const bool was = handState[h].trigger;
        const bool down = was ? hands[h].trigger > 0.4f : hands[h].trigger > 0.6f;
        if (down && !was && !chromeDrag && chromeHits[h]) {
            beginChromeDrag(h, *chromeHits[h]);
        } else if (!down && was && chromeDrag && chromeDrag->hand == h) {
            endChromeDrag(bridge, h);
        }
    }
    // Trigger on a dock item: it acts when released on it.
    for (int h = 0; h < 2; ++h) {
        const bool was = handState[h].trigger;
        const bool down = was ? hands[h].trigger > 0.4f : hands[h].trigger > 0.6f;
        if (down && !was && dockHits[h] && dockHits[h]->bar && !dockGrab) {
            beginDockGrab(h, false, *dockHits[h]);
        } else if (!down && was && dockGrab && dockGrab->hand == h && !dockGrab->byGrip) {
            endDockGrab();
        } else if (down && !was && dockHits[h] && dockHits[h]->item != Spatial::DockItem::None) {
            dockPressed[h] = dockHits[h]->item;
            input.haptic(h, 0.3f, 10'000'000);
        } else if (!down && was && dockPressed[h] != Spatial::DockItem::None) {
            if (dockHits[h] && dockHits[h]->item == dockPressed[h]) {
                performDock(bridge, h, dockPressed[h]);
            }
            dockPressed[h] = Spatial::DockItem::None;
        }
    }
    // Trigger on a notification: it acts when released on the same part.
    for (int h = 0; h < 2; ++h) {
        const bool was = handState[h].trigger;
        const bool down = was ? hands[h].trigger > 0.4f : hands[h].trigger > 0.6f;
        if (down && !was && noteHits[h] && noteHits[h]->hit.part != Spatial::NotificationHit::Part::None) {
            notePressed[h] = noteHits[h];
            input.haptic(h, 0.3f, 10'000'000);
        } else if (!down && was && notePressed[h]) {
            if (noteHits[h] && noteHits[h]->card == notePressed[h]->card && noteHits[h]->hit == notePressed[h]->hit) {
                performNote(bridge, *notePressed[h]);
            }
            notePressed[h].reset();
        }
    }

    if (launcherOpen) {
        ++homeFrames;
        homeFramesOnHome += cellHits[0] || cellHits[1];
        homeFramesOnApp += (cellHits[0] && cellHits[0]->cell >= 0) || (cellHits[1] && cellHits[1]->cell >= 0);
    }
    // Trigger on an app: it starts when released on the same app; the thumbstick flips pages.
    for (int h = 0; h < 2; ++h) {
        const bool was = handState[h].trigger;
        const bool down = was ? hands[h].trigger > 0.4f : hands[h].trigger > 0.6f;
        if (down && !was && cellHits[h] && cellHits[h]->cell >= 0) {
            cellPressed[h] = cellHits[h]->cell;
            input.haptic(h, 0.3f, 10'000'000);
        } else if (down && !was && cellHits[h] && cellHits[h]->search) {
            // The search field: the keyboard types into it (opened for it if it was closed).
            homeSearching = true;
            if (!keyboardOpen) {
                openKeyboard(false, Spatial::azimuthOf(head.forward()));
                keyboardForSearch = true;
            }
            input.haptic(h, 0.3f, 10'000'000);
            qCInfo(lcXr) << "home: search";
        } else if (down && !was && cellHits[h] && cellHits[h]->card >= 0) {
            cardPressed[h] = cellHits[h]->card;
            cardClosePressed[h] = cellHits[h]->close;
            input.haptic(h, 0.3f, 10'000'000);
        } else if (!down && was && cardPressed[h] >= 0) {
            // A running app: brought in front of the user (back from minimized), home closes; or closed.
            const int card = cardPressed[h];
            if (cellHits[h] && cellHits[h]->card == card && cellHits[h]->close == cardClosePressed[h] && card < int(runningWindows.size())
                && runningWindows[size_t(card)].vr) {
                // Another VR app: the runtime shows it; Rift Home waits until the user switches back (the Rift panel's
                // Switch application) or the app ends. It has no close button here (not Rift Home's to close).
                if (!cardClosePressed[h] && runtimeApps) {
                    runtimeApps->makePrimary(runningWindows[size_t(card)].vrApp);
                    qCInfo(lcXr).noquote() << "home: VR app" << runningWindows[size_t(card)].title
                                           << "brought in front (back to Rift Home: the Rift panel's Switch application)";
                    openLauncher(false);
                }
                input.haptic(h, 0.5f, 20'000'000);
            } else if (cellHits[h] && cellHits[h]->card == card && cellHits[h]->close == cardClosePressed[h] && card < int(runningWindows.size())) {
                const int windowId = runningWindows[size_t(card)].windowId;
                if (cardClosePressed[h]) {
                    PointerEvent e;
                    e.type = PointerEvent::Type::Close;
                    e.windowId = windowId;
                    bridge.sendInput(e);
                    qCInfo(lcXr) << "home: window" << windowId << "closed";
                } else {
                    layout.setMinimized(windowId, false);
                    makeMain(windowId, "brought in front from Home");
                    qCInfo(lcXr) << "home: window" << windowId << "brought in front";
                    openLauncher(false);
                }
                input.haptic(h, 0.5f, 20'000'000);
            }
            cardPressed[h] = -1;
        } else if (down && !was && launcherOpen && !keyHits[h]) {
            qCInfo(lcXr).noquote() << "home: trigger" << (cellHits[h] ? "on home, between apps" : "not on home");
        } else if (!down && was && cellPressed[h] >= 0) {
            const std::vector<int> shown = homeApps();
            const size_t slot = size_t(launcherPage * Spatial::LauncherGrid::kPerPage + cellPressed[h]);
            const int index = slot < shown.size() ? shown[slot] : int(options.apps.size());
            if (!(cellHits[h] && cellHits[h]->cell == cellPressed[h])) {
                qCInfo(lcXr) << "home: released off the app it was pressed on: nothing started";
            }
            if (cellHits[h] && cellHits[h]->cell == cellPressed[h] && index < int(options.apps.size())) {
                PointerEvent e;
                e.type = PointerEvent::Type::Launch;
                e.button = uint32_t(index);
                bridge.sendInput(e);
                qCInfo(lcXr).noquote() << "launcher: starting" << options.apps[size_t(index)].name;
                input.haptic(h, 0.5f, 20'000'000);
                openLauncher(false);
            }
            cellPressed[h] = -1;
        }
        const float sx = hands[h].stickX;
        if (cellHits[h] && std::fabs(sx) > 0.7f && std::fabs(lastStickX[h]) <= 0.7f) {
            const int pages = Spatial::LauncherGrid::pages(int(homeApps().size()));
            launcherPage = std::clamp(launcherPage + (sx > 0.0f ? 1 : -1), 0, pages - 1);
        }
        lastStickX[h] = sx;
    }

    // Trigger on a key: key down (the app repeats while held), up on release wherever the ray is then.
    for (int h = 0; h < 2; ++h) {
        const bool was = handState[h].trigger;
        const bool down = was ? hands[h].trigger > 0.4f : hands[h].trigger > 0.6f;
        if (down && !was && keyHits[h] && keyHits[h]->bar && !keyboardGrab) {
            beginKeyboardGrab(h, false, *keyHits[h]);
        } else if (!down && was && keyboardGrab && keyboardGrab->hand == h && !keyboardGrab->byGrip) {
            endKeyboardGrab();
        } else if (down && !was && keyHits[h] && keyHits[h]->key >= 0 && keyHeld[h] < 0) {
            keyHeld[h] = keyHits[h]->key;
            if (homeSearching) {
                const Spatial::KeyDef &def = keyboard.keys()[size_t(keyHeld[h])];
                const bool upper = keyboard.shift() || (def.letter && keyboard.capsLock());
                keyboard.press(keyHeld[h]); // the modifiers' state only: nothing goes to a window
                typeIntoSearch(bridge, keyHeld[h], upper);
            } else {
                sendKeys(bridge, keyboard.press(keyHeld[h]));
            }
            input.haptic(h, 0.3f, 10'000'000);
            ++keysTyped;
        } else if (!down && was && keyHeld[h] >= 0) {
            if (homeSearching) {
                keyboard.release(keyHeld[h]);
            } else {
                sendKeys(bridge, keyboard.release(keyHeld[h]));
            }
            keyHeld[h] = -1;
            if (keyboard.takeHide()) {
                homeSearching = false;
                keyboardForSearch = false;
                closeKeyboard(bridge);
            }
        }
    }
    for (int h = 0; h < 2; ++h) {
        const bool squeezed = hands[h].squeeze > 0.6f;
        if (squeezed && keyHits[h] && !keyboardGrab && grabHand < 0 && keyHeld[h] < 0) {
            beginKeyboardGrab(h, true, *keyHits[h]);
        } else if (hands[h].squeeze < 0.4f && keyboardGrab && keyboardGrab->hand == h && keyboardGrab->byGrip) {
            endKeyboardGrab();
        }
    }
    if (keyboardGrab) {
        updateKeyboardGrab(dt);
    }
    for (int h = 0; h < 2; ++h) {
        const bool squeezed = hands[h].squeeze > 0.6f;
        if (squeezed && dockHits[h] && !dockGrab && !keyboardGrab && grabHand < 0) {
            beginDockGrab(h, true, *dockHits[h]);
        } else if (hands[h].squeeze < 0.4f && dockGrab && dockGrab->hand == h && dockGrab->byGrip) {
            endDockGrab();
        }
    }
    if (dockGrab) {
        if (!dockOpen) {
            dockGrab.reset();
        } else {
            updateDockGrab(dt);
        }
    }
    if (chromeDrag) {
        const Spatial::Pose &aim = hands[chromeDrag->hand].aim;
        if (chromeDrag->control == Spatial::FrameControl::Scale) {
            layout.updateScale(aim.position, aim.forward());
        }
        // The resize handle: the app asked for the size under the ray, a few times a second (it lays itself out anew).
        if (chromeDrag->control == Spatial::FrameControl::Resize && resizeDrag && chromeHits[chromeDrag->hand]) {
            const auto [width, height] = resizeTarget(chromeDrag->windowId, *chromeHits[chromeDrag->hand]);
            const bool moved = std::abs(width - resizeDrag->width) > 6 || std::abs(height - resizeDrag->height) > 6;
            if (moved) {
                resizeDrag->pendingWidth = width;
                resizeDrag->pendingHeight = height;
                resizeDrag->moved = true; // a real drag: the release sends the last size
            }
            if (width > 0 && height > 0 && moved && Clock::now() - resizeDrag->sent > std::chrono::milliseconds(120)) {
                PointerEvent e;
                e.type = PointerEvent::Type::Resize;
                e.windowId = chromeDrag->windowId;
                e.x = width;
                e.y = height;
                bridge.sendInput(e);
                resizeDrag->width = width;
                resizeDrag->height = height;
                resizeDrag->sent = Clock::now();
                resizeDrag->moved = true;
            }
        }
    }
    // The tilt bar: the thumbstick tilts (up/down) and turns (left/right) the window while the ray is on the bar or
    // the trigger holds it, steadily (dragging with the controller was too hard to control).
    for (int h = 0; h < 2; ++h) {
        const bool held = chromeDrag && chromeDrag->hand == h && chromeDrag->control == Spatial::FrameControl::Tilt;
        const bool hovered = !chromeDrag && chromeHits[h] && chromeHits[h]->control == Spatial::FrameControl::Tilt;
        if (!held && !hovered) {
            continue;
        }
        const int windowId = held ? chromeDrag->windowId : chromeHits[h]->windowId;
        constexpr float kRate = 0.87f; // radians a second at full deflection (50 degrees)
        layout.adjustTilt(windowId, deadzone(hands[h].stickY) * kRate * dt, deadzone(hands[h].stickX) * kRate * dt);
    }
    // The curvature slider: held, the knob follows the ray along the track (left flat, right bent;
    // it snaps to flat near the left end); on it or held, the thumbstick left/right bends it steadily.
    for (int h = 0; h < 2; ++h) {
        const bool held = chromeDrag && chromeDrag->hand == h && chromeDrag->control == Spatial::FrameControl::Curve;
        const bool hovered = !chromeDrag && chromeHits[h] && chromeHits[h]->control == Spatial::FrameControl::Curve;
        if (!held && !hovered) {
            continue;
        }
        const int windowId = held ? chromeDrag->windowId : chromeHits[h]->windowId;
        const auto placed = layout.windows().find(windowId);
        const auto framed = chrome.find(windowId);
        if (placed == layout.windows().end() || framed == chrome.end()) {
            continue;
        }
        const float before = placed->second.curvature;
        const float stick = deadzone(hands[h].stickX);
        if (stick != 0.0f) {
            constexpr float kRate = 0.6f; // flat to fully bent in under 2 s at full deflection
            layout.adjustCurvature(windowId, stick * kRate * dt);
        } else if (held && chromeHits[h]) {
            layout.setCurvature(windowId, framed->second.frame.curvatureAt(chromeHits[h]->localX));
        }
        const float after = layout.windows().at(windowId).curvature;
        if ((before == 0.0f) != (after == 0.0f)) {
            input.haptic(h, 0.3f, 10'000'000); // a tick into and out of flat
        }
    }
    if (grabHand >= 0) {
        const XrInput::Hand &holding = hands[grabHand];
        layout.updateGrab(holding.aim, head, deadzone(holding.stickY), deadzone(holding.stickX), dt);
    }

    // The Wayland pointer follows the active hand, unless that hand holds a window.
    const int a = activeHand;
    const XrInput::Hand &hand = hands[a];
    HandState &state = handState[a];
    const std::optional<Spatial::Hit> &hit = hits[a];
    const bool pointing = hit && grabHand != a;
    const bool trigger = state.trigger ? hand.trigger > 0.4f : hand.trigger > 0.6f;
    double px = lastX;
    double py = lastY;
    if (pointing) {
        if (trigger && !state.trigger) {
            // The click goes where the ray was when the pull began, if that was on this window, lately and close by.
            const auto &armed = state.armed;
            const bool rewind = armed && armed->windowId == hit->windowId && Clock::now() - armed->at < std::chrono::milliseconds(600)
                && std::acos(std::clamp(armed->direction.dot(hand.aim.forward()), -1.0f, 1.0f)) < Spatial::radians(4.0f);
            double cx = rewind ? armed->x : hit->x;
            double cy = rewind ? armed->y : hit->y;
            const Spatial::Vec3 aimed = rewind ? armed->direction : hand.aim.forward();
            if (lastPress && lastPress->windowId == hit->windowId && Clock::now() - lastPress->at < std::chrono::milliseconds(800)
                && std::acos(std::clamp(lastPress->direction.dot(aimed), -1.0f, 1.0f)) < Spatial::radians(1.5f)) {
                cx = lastPress->x; // the second click of a double click
                cy = lastPress->y;
                const uint32_t now = timeMs - clickTimeShiftMs;
                const uint32_t target = std::max(lastPressMs + 250u, lastReleaseMs + 30u);
                if (now > target && now - target < 2000u) {
                    clickTimeShiftMs += now - target;
                }
            }
            clickLock.press(cx, cy, aimed);
            lastPress = LastPress{hit->windowId, cx, cy, aimed, Clock::now()};
            lastPressMs = timeMs - clickTimeShiftMs;
        }
        std::tie(px, py) = clickLock.filter(hit->x, hit->y, hand.aim.forward(), dt);
        // Between the clicks of a double click the pointer stays where the first went, while the ray stays near it (a
        // second press there is snapped and stamped earlier, so no motion may come between them).
        if (!trigger && lastPress && lastPress->windowId == hit->windowId && Clock::now() - lastPress->at < std::chrono::milliseconds(800)
            && std::acos(std::clamp(lastPress->direction.dot(hand.aim.forward()), -1.0f, 1.0f)) < Spatial::radians(1.5f)) {
            px = lastPress->x;
            py = lastPress->y;
        }
    }
    const auto event = [&](PointerEvent::Type type) {
        PointerEvent e;
        e.type = type;
        e.windowId = hit ? hit->windowId : pointerWindow;
        e.x = px;
        e.y = py;
        e.timeMs = timeMs - clickTimeShiftMs;
        return e;
    };
    if (pointing) {
        if (hit->windowId != pointerWindow || std::fabs(px - lastX) > 0.25 || std::fabs(py - lastY) > 0.25) {
            bridge.sendInput(event(PointerEvent::Type::Motion));
            pointerWindow = hit->windowId;
            lastX = px;
            lastY = py;
        }
        const auto button = [&](uint32_t code, bool pressed) {
            PointerEvent e = event(PointerEvent::Type::Button);
            e.button = code;
            e.pressed = pressed;
            bridge.sendInput(e);
        };
        if (trigger != state.trigger) {
            button(BTN_LEFT, trigger);
            pressedWindow = trigger ? hit->windowId : 0;
            if (trigger) {
                lastClickedWindow = hit->windowId;
            }
            if (trigger) {
                input.haptic(a, 0.35f, 12'000'000);
            } else {
                clickLock.release();
                lastReleaseMs = timeMs - clickTimeShiftMs;
            }
        }
        // Right and middle click: the buttons given them (by default A and the thumbstick presses), a thumbstick's unless
        // the stick was just scrolling (a push that ends in a click).
        const bool stickQuiet = Clock::now() - state.lastDeflect > std::chrono::milliseconds(300);
        const bool right = actionHeld(a, Spatial::ButtonAction::RightClick, state.rightClick || stickQuiet);
        if (right != state.rightClick) {
            button(BTN_RIGHT, right);
            state.rightClick = right;
        }
        const bool middle = actionHeld(a, Spatial::ButtonAction::MiddleClick, state.middleClick || stickQuiet);
        if (middle != state.middleClick) {
            button(BTN_MIDDLE, middle);
            state.middleClick = middle;
        }
        const float sx = deadzone(hand.stickX);
        const float sy = deadzone(hand.stickY);
        if (sx != 0.0f || sy != 0.0f) {
            PointerEvent e = event(PointerEvent::Type::Axis);
            e.axisX = sx * 900.0 * dt; // surface pixels a second at full tilt
            e.axisY = -sy * 900.0 * dt; // stick forward scrolls up
            bridge.sendInput(e);
            state.scrolling = true;
        } else if (state.scrolling) {
            bridge.sendInput(event(PointerEvent::Type::AxisStop));
            state.scrolling = false;
        }
    } else if (pointerWindow && !(bridge.dragging() && trigger)) { // a drag crossing the gap between windows goes on
        if (state.trigger) {
            PointerEvent e = event(PointerEvent::Type::Button);
            e.button = BTN_LEFT;
            e.pressed = false;
            bridge.sendInput(e);
            clickLock.release();
        }
        if (state.rightClick) {
            PointerEvent e = event(PointerEvent::Type::Button);
            e.button = BTN_RIGHT;
            e.pressed = false;
            bridge.sendInput(e);
            state.rightClick = false;
        }
        if (state.middleClick) {
            PointerEvent e = event(PointerEvent::Type::Button);
            e.button = BTN_MIDDLE;
            e.pressed = false;
            bridge.sendInput(e);
            state.middleClick = false;
        }
        bridge.sendInput({PointerEvent::Type::Leave});
        pointerWindow = pressedWindow = 0;
    }
    for (int h = 0; h < 2; ++h) {
        HandState &st = handState[h];
        st.trigger = st.trigger ? hands[h].trigger > 0.4f : hands[h].trigger > 0.6f;
        st.primary = hands[h].primary;
        st.stickClick = hands[h].stickClick;
        if (std::fabs(hands[h].stickX) > 0.15f || std::fabs(hands[h].stickY) > 0.15f) {
            st.lastDeflect = Clock::now();
        }
    }
}

void XrCompositor::Impl::layoutChrome()
{
    // Windows gone: their frames' layers too.
    for (auto it = chrome.begin(); it != chrome.end();) {
        if (layout.windows().contains(it->first)) {
            ++it;
            continue;
        }
        for (int part = 0; part < kChromeParts; ++part) {
            destroyLayer(chromeLayer(it->first, part));
            pendingArt.erase(chromeLayer(it->first, part));
            cancelArt(chromeLayer(it->first, part));
        }
        if (chromeDrag && chromeDrag->windowId == it->first) {
            chromeDrag.reset();
        }
        it = chrome.erase(it);
    }
    for (const auto &[id, window] : layout.windows()) {
        Chrome &c = chrome[id];
        c.surface = layout.surfacePose(window);
        const float cornerRadius = options.cornerRadius / layout.pixelsPerMeter() * window.scale;
        c.frame = Spatial::FrameLayout::compute(layout.widthMeters(window), layout.heightMeters(window), cornerRadius,
                                                (c.surface.position - head.position).length());
    }
}

std::optional<XrCompositor::Impl::ChromeHit> XrCompositor::Impl::chromeHitTest(Spatial::Vec3 origin, Spatial::Vec3 direction)
{
    // Where the ray meets each window's plane near it. Solid things (a window's content, a frame control) hide what is
    // behind them: a frame further away neither shows nor takes the ray (back windows' frames
    // showed through the front ones).
    struct Candidate {
        int windowId;
        float distance;
        Spatial::Vec3 point;
        float localX;
        float localY;
        Spatial::FrameControl control;
        bool nearEdge;
    };
    std::vector<Candidate> candidates;
    float solid = std::numeric_limits<float>::max();
    for (const auto &[id, c] : chrome) {
        if (layout.cinemaWindow() && !shownInCinema(id)) {
            continue;
        }
        if (const auto placed = layout.windows().find(id); placed != layout.windows().end() && placed->second.minimized) {
            continue;
        }
        const auto onPlane = layout.projectOnto(id, origin, direction); // its plane, or its curve
        if (!onPlane) {
            continue;
        }
        const float x = onPlane->localX;
        const float y = onPlane->localY;
        if (!c.frame.near(x, y)) {
            continue;
        }
        Spatial::FrameControl control = c.frame.hit(x, y);
        if (!cylinderLayers && (control == Spatial::FrameControl::Curve || control == Spatial::FrameControl::ResetCurve)) {
            control = Spatial::FrameControl::None; // no curved windows without cylinder layers (the slider is not drawn)
        }
        if (control == Spatial::FrameControl::Resize && layout.windows().contains(id) && layout.windows().at(id).fullscreen) {
            control = Spatial::FrameControl::None; // a fullscreen window's pixels are the screen's (the button is not drawn)
        }
        const bool inside = std::fabs(x) <= c.frame.windowWidth / 2.0f && std::fabs(y) <= c.frame.windowHeight / 2.0f;
        if (inside || control != Spatial::FrameControl::None) {
            solid = std::min(solid, onPlane->distance);
        }
        candidates.push_back({id, onPlane->distance, onPlane->point, x, y, control, c.frame.nearEdge(x, y)});
    }
    std::optional<ChromeHit> best;
    const auto now = Clock::now();
    for (const Candidate &k : candidates) {
        if (k.distance > solid + 1e-4f) {
            continue; // behind a window or a control
        }
        if (k.nearEdge) {
            chrome.at(k.windowId).lastNear = now; // the frame shows while the ray is near the window's edges
        }
        if (k.control != Spatial::FrameControl::None && (!best || k.distance < best->distance)) {
            best = ChromeHit{k.windowId, k.control, k.distance, k.point, k.localX, k.localY};
        }
    }
    return best;
}

void XrCompositor::Impl::beginChromeDrag(int hand, const ChromeHit &hit)
{
    const Spatial::Pose &aim = hands[hand].aim;
    switch (hit.control) {
    case Spatial::FrameControl::Grab:
        if (grabHand >= 0 || !layout.beginGrab(hit.windowId, aim, hit.distance)) {
            return;
        }
        grabHand = hand;
        grabByTrigger = true;
        break;
    case Spatial::FrameControl::Scale:
        if (!layout.beginScale(hit.windowId, aim.position, aim.forward())) {
            return;
        }
        break;
    case Spatial::FrameControl::Tilt:
        if (!layout.beginTilt(hit.windowId, aim.position, aim.forward())) {
            return;
        }
        break;
    case Spatial::FrameControl::Curve:
        break; // the knob follows the ray while held (interact)
    case Spatial::FrameControl::Resize:
        if (const auto placed = layout.windows().find(hit.windowId); placed != layout.windows().end()) {
            ResizeDrag drag;
            drag.width = drag.startWidth = placed->second.widthPx;
            drag.height = drag.startHeight = placed->second.heightPx;
            drag.startX = hit.localX;
            drag.startY = hit.localY;
            drag.sent = Clock::now();
            resizeDrag = drag;
        }
        break;
    default:
        break; // menu buttons act on release
    }
    chromeDrag = ChromeDrag{hand, hit.windowId, hit.control};
    input.haptic(hand, 0.35f, 12'000'000);
    qCInfo(lcXr) << "frame:" << controlName(hit.control) << "pressed on window" << hit.windowId;
}

void XrCompositor::Impl::endChromeDrag(LayerBridge &bridge, int hand)
{
    const ChromeDrag drag = *chromeDrag;
    chromeDrag.reset();
    const auto placed = layout.windows().find(drag.windowId);
    switch (drag.control) {
    case Spatial::FrameControl::Grab:
        if (grabHand == hand) {
            layout.endGrab();
            grabHand = -1;
            grabByTrigger = false;
        }
        qCInfo(lcXr) << "frame: let go of window" << drag.windowId;
        break;
    case Spatial::FrameControl::Resize:
        if (resizeDrag && resizeDrag->moved) {
            // The last size the drag asked for, if not sent yet (not the ray's place at release: the hand moves away).
            const int width = resizeDrag->pendingWidth;
            const int height = resizeDrag->pendingHeight;
            if (width > 0 && height > 0 && (width != resizeDrag->width || height != resizeDrag->height)) {
                PointerEvent e;
                e.type = PointerEvent::Type::Resize;
                e.windowId = drag.windowId;
                e.x = width;
                e.y = height;
                bridge.sendInput(e);
                qCInfo(lcXr) << "frame: resize done on window" << drag.windowId << width << "x" << height;
            }
        }
        resizeDrag.reset();
        break;
    case Spatial::FrameControl::Curve:
        if (placed != layout.windows().end()) {
            qCInfo(lcXr).noquote() << "frame: curve done on window" << drag.windowId
                                   << QStringLiteral("(curvature %1, radius %2 m)")
                                          .arg(placed->second.curvature, 0, 'f', 2)
                                          .arg(layout.curveRadius(placed->second), 0, 'f', 2);
        }
        break;
    case Spatial::FrameControl::Scale:
    case Spatial::FrameControl::Tilt:
        layout.endDrag();
        if (placed != layout.windows().end()) {
            qCInfo(lcXr).noquote() << "frame:" << controlName(drag.control) << "done on window" << drag.windowId
                                   << QStringLiteral("(scale %1, tilt %2 degrees, turn %3 degrees)")
                                          .arg(placed->second.scale, 0, 'f', 2)
                                          .arg(placed->second.tilt * 180.0f / float(M_PI), 0, 'f', 1)
                                          .arg(placed->second.turn * 180.0f / float(M_PI), 0, 'f', 1);
        }
        break;
    default:
        // A button: acts when released on it, or just beside it (the trigger's release moves the ray too: a
        // reset dot was released 1-2 cm off).
        if ((chromeUnder[hand] && chromeUnder[hand]->windowId == drag.windowId && chromeUnder[hand]->control == drag.control)
            || releasedNear(hand, drag)) {
            performMenu(bridge, hand, drag.windowId, drag.control);
        } else {
            qCInfo(lcXr) << "frame:" << controlName(drag.control) << "released off the button: nothing done";
        }
        break;
    }
}

bool XrCompositor::Impl::releasedNear(int hand, const ChromeDrag &drag) const
{
    const auto framed = chrome.find(drag.windowId);
    const auto onPlane = layout.projectOnto(drag.windowId, hands[hand].aim.position, hands[hand].aim.forward());
    if (framed == chrome.end() || !onPlane) {
        return false;
    }
    const Spatial::FrameLayout &f = framed->second.frame;
    const Spatial::FrameRect *rect = drag.control == Spatial::FrameControl::ResetTilt ? &f.resetTilt
        : drag.control == Spatial::FrameControl::ResetCurve                          ? &f.resetCurve
                                                                                     : nullptr;
    for (int i = 0; i < Spatial::kMenuButtons; ++i) {
        if (Spatial::kMenuOrder[i] == drag.control) {
            rect = &f.buttons[i];
        }
    }
    if (!rect) {
        return false;
    }
    return std::hypot(onPlane->localX - rect->x, onPlane->localY - rect->y) < std::max(rect->width, rect->height) / 2.0f + 0.02f * f.unit;
}

void XrCompositor::Impl::makeMain(int windowId, const char *why)
{
    std::map<int, Spatial::Vec3> before;
    for (const auto &[id, window] : layout.windows()) {
        before[id] = window.pose.position;
    }
    layout.makeMain(windowId, head);
    const auto describe = [this](Spatial::Vec3 position) {
        const Spatial::Vec3 relative = position - head.position;
        return QStringLiteral("%1 degrees, %2 m").arg(Spatial::azimuthOf(relative) * 180.0f / float(M_PI), 0, 'f', 1).arg(std::hypot(relative.x, relative.z), 0, 'f', 2);
    };
    QStringList moved;
    for (const auto &[id, window] : layout.windows()) {
        const auto was = before.find(id);
        if (id != windowId && was != before.end() && (window.pose.position - was->second).length() > 0.01f) {
            moved << QStringLiteral("window %1 from %2 to %3").arg(id).arg(describe(was->second), describe(window.pose.position));
        }
    }
    const auto placed = layout.windows().find(windowId);
    qCInfo(lcXr).noquote() << QStringLiteral("window %1 in front (%2) at %3; %4").arg(windowId).arg(QLatin1String(why))
                                  .arg(placed == layout.windows().end() ? QStringLiteral("?") : describe(placed->second.pose.position),
                                       moved.isEmpty() ? QStringLiteral("nothing moved aside") : QStringLiteral("moved aside: ") + moved.join(QStringLiteral(", ")));
}

void XrCompositor::Impl::performMenu(LayerBridge &bridge, int hand, int windowId, Spatial::FrameControl control)
{
    const auto command = [&](PointerEvent::Type type) {
        PointerEvent e;
        e.type = type;
        e.windowId = windowId;
        bridge.sendInput(e);
    };
    switch (control) {
    case Spatial::FrameControl::Follow:
        qCInfo(lcXr) << "frame: follow" << (layout.toggleFollow(windowId, head) ? "on" : "off") << "for window" << windowId;
        break;
    case Spatial::FrameControl::Main:
        makeMain(windowId, "Make Main");
        qCInfo(lcXr) << "frame: window" << windowId << "made main";
        break;
    case Spatial::FrameControl::Pin:
        qCInfo(lcXr) << "frame: window" << windowId << (layout.togglePin(windowId) ? "pinned" : "unpinned");
        break;
    case Spatial::FrameControl::Size: {
        const auto [fullscreen, maximized] = sizeStates[windowId];
        command(fullscreen ? PointerEvent::Type::Restore : maximized ? PointerEvent::Type::Fullscreen : PointerEvent::Type::Maximize);
        qCInfo(lcXr) << "frame: window" << windowId << (fullscreen ? "back to normal size" : maximized ? "fullscreen" : "maximized");
        break;
    }
    case Spatial::FrameControl::Close:
        command(PointerEvent::Type::Close);
        qCInfo(lcXr) << "frame: close asked for window" << windowId;
        break;
    case Spatial::FrameControl::Cinema: {
        const auto sizeCommand = [&](int id, PointerEvent::Type type) {
            PointerEvent e;
            e.type = type;
            e.windowId = id;
            bridge.sendInput(e);
        };
        const auto restore = [&] {
            // The app back to the size it had (cinema made it fullscreen, so its controls hide).
            sizeCommand(cinemaBefore.windowId, cinemaBefore.fullscreen ? PointerEvent::Type::Fullscreen
                                               : cinemaBefore.maximized ? PointerEvent::Type::Maximize
                                                                        : PointerEvent::Type::Restore);
        };
        const int current = layout.cinemaWindow();
        if (current) {
            layout.leaveCinema();
            restore();
            qCInfo(lcXr) << "frame: window" << current << "leaves cinema mode";
        }
        if (current != windowId) {
            const auto [fullscreen, maximized] = sizeStates[windowId];
            cinemaBefore = {windowId, fullscreen, maximized};
            layout.enterCinema(windowId, head);
            command(PointerEvent::Type::Fullscreen);
            qCInfo(lcXr) << "frame: window" << windowId << "in cinema mode (the others hidden, the room dimmed)";
        }
        break;
    }
    case Spatial::FrameControl::ResetTilt:
        layout.resetTilt(windowId);
        qCInfo(lcXr) << "frame: window" << windowId << "back upright (tilt and turn reset)";
        break;
    case Spatial::FrameControl::ResetCurve:
        layout.setCurvature(windowId, 0.0f);
        qCInfo(lcXr) << "frame: window" << windowId << "flat again";
        break;
    case Spatial::FrameControl::Minimize:
        layout.setMinimized(windowId, true);
        qCInfo(lcXr) << "frame: window" << windowId << "minimized (Home's running apps bring it back)";
        break;

    default:
        return;
    }
    input.haptic(hand, 0.5f, 20'000'000);
}

void XrCompositor::Impl::openKeyboard(bool automatic, float azimuth)
{
    // Below the gaze, an arm's length away (or where the user last moved it, relative to the gaze), turned to face the
    // eyes (as a laptop's keyboard is tilted up).
    const Spatial::Vec3 eye = head.position;
    const Spatial::Vec3 position = eye + Spatial::directionOf(azimuth + keyboardAzimuth) * keyboardDistance
        + Spatial::Vec3{0.0f, keyboardHeight, 0.0f};
    keyboardPose = {facingEye(position), position};
    keyboardOpen = true;
    keyboardAuto = automatic;
    qCInfo(lcXr) << "keyboard open" << (automatic ? "(a text field was focused)" : "(window menu)");
}

int XrCompositor::Impl::homeHovered() const
{
    for (int h : {activeHand, 1 - activeHand}) {
        const int cell = cellPressed[h] >= 0 ? cellPressed[h] : (cellHits[h] ? cellHits[h]->cell : -1);
        const std::vector<int> shown = homeApps();
        const size_t slot = size_t(launcherPage * Spatial::LauncherGrid::kPerPage + cell);
        if (cell >= 0 && slot < shown.size()) {
            return shown[slot];
        }
    }
    return -1;
}

void XrCompositor::Impl::openDock(bool open)
{
    if (open) {
        // Below the gaze (above where the keyboard opens), turned to face the eyes; "too much in the
        // face", so a little lower than first (was 26 cm below the eyes). Looking steeply up (lying back, not reclined),
        // "below the gaze" tilts up with the gaze: the dock opens in view, its Recline within reach.
        const bool steep = head.forward().y > std::sin(Spatial::radians(kDockTiltFromDegrees));
        const Spatial::Quat frame = steep ? Spatial::levelView(head).orientation : Spatial::Quat::fromYaw(-Spatial::azimuthOf(head.forward()));
        const Spatial::Vec3 position = head.position + frame.rotate(Spatial::directionOf(dockAzimuth) * dockDistance + Spatial::Vec3{0.0f, dockHeight, 0.0f});
        const Spatial::Vec3 z = (head.position - position).normalized();
        const Spatial::Vec3 x = frame.rotate({0.0f, 1.0f, 0.0f}).cross(z).normalized();
        dockPose = {Spatial::Quat::fromBasis(x, z.cross(x), z), position};
        // Where the user looked before looking down at the dock: what its Recline turns to.
        dockOpenedHead = roomHead;
    }
    for (int h = 0; h < 2; ++h) {
        dockHits[h].reset();
        dockPressed[h] = Spatial::DockItem::None;
    }
    dockOpen = open;
    if (!open) {
        notesOpen = false; // the list goes with the dock
    }
    qCInfo(lcXr) << (open ? "dock open" : "dock closed");
}

std::optional<XrCompositor::Impl::DockHit> XrCompositor::Impl::dockHitTest(Spatial::Vec3 origin, Spatial::Vec3 direction) const
{
    const Spatial::Vec3 normal = dockPose.orientation.rotate({0.0f, 0.0f, 1.0f});
    const float denom = direction.dot(normal);
    if (denom > -1e-4f) {
        return std::nullopt;
    }
    const float t = (dockPose.position - origin).dot(normal) / denom;
    if (t <= 0.0f) {
        return std::nullopt;
    }
    const Spatial::Vec3 point = origin + direction * t;
    const Spatial::Vec3 local = dockPose.inverse().transform(point);
    // Its grab bar under it (easy to hit: wider and taller than drawn).
    const float barY = -Spatial::DockLayout::height() / 2.0f - kDockBarGap;
    if (std::fabs(local.x) <= kDockBarWidth / 2.0f + 0.03f && std::fabs(local.y - barY) <= 0.022f) {
        return DockHit{Spatial::DockItem::None, t, point, true};
    }
    if (!Spatial::DockLayout::contains(local.x, local.y)) {
        return std::nullopt;
    }
    return DockHit{Spatial::DockLayout::itemAt(local.x, local.y), t, point};
}

void XrCompositor::Impl::setRecline(bool on, const Spatial::Pose &viewHead)
{
    reclined = on;
    reclineFrom = recline;
    reclineTarget = on ? Spatial::reclineTo(layout.home(), viewHead) : Spatial::Pose{};
    reclineProgress = 0.0f;
    if (on) {
        const Spatial::Vec3 view = viewHead.forward();
        const bool rolled = std::abs(Spatial::headRoll(viewHead)) >= Spatial::radians(Spatial::kReclineRollFollowDegrees);
        qCInfo(lcXr).noquote() << QStringLiteral("recline: the arrangement turns to the view (%1 degrees up%2)")
                                      .arg(std::asin(std::clamp(view.y, -1.0f, 1.0f)) * 180.0f / float(M_PI), 0, 'f', 0)
                                      .arg(rolled ? QStringLiteral(", turned with the head's roll") : QString());
    } else {
        qCInfo(lcXr) << "recline: upright again";
    }
}

void XrCompositor::Impl::setGameMode(LayerBridge &bridge, bool on)
{
    Q_UNUSED(bridge);
    if (on == gameMode) {
        return;
    }
    if (on) {
        if (!gamepad.open()) {
            return; // logged: no /dev/uinput, game mode stays off
        }
        gameMode = true;
        gameMenuDownAt.reset();
        gameStartFrames = 0;
        if (dockOpen) {
            openDock(false);
        }
        for (int h = 0; h < 2; ++h) {
            input.haptic(h, 0.5f, 60'000'000); // a firm tap: the controllers are a gamepad now
        }
        qCInfo(lcXr) << "game mode on: the controllers are a gamepad (hold the left menu button to stop)";
        showHint(QStringLiteral("Game mode: the controllers are a gamepad"),
                 QStringLiteral("To stop: hold the \u2630 button on the left controller for a second."), 8);
        return;
    }
    gamepad.rest(); // kept: the game keeps its pad for game mode's return
    gameMode = false;
    for (int h = 0; h < 2; ++h) {
        input.haptic(h, 0.3f, 30'000'000);
    }
    qCInfo(lcXr) << "game mode off: the controllers point again (the gamepad stays, nothing pressed)";
    showHint(QStringLiteral("Game mode off"), QStringLiteral("The controllers point again."), 3);
}

void XrCompositor::Impl::showHint(const QString &title, const QString &text, int seconds)
{
    // Where the user looks, a little below the middle (as a notification's card), facing the eyes.
    const Spatial::Vec3 forward = head.forward();
    const float pitch = std::clamp(std::asin(std::clamp(forward.y, -1.0f, 1.0f)) - kNoteCardBelowGaze, -1.2f, 0.8f);
    const Spatial::Vec3 direction = Spatial::directionOf(Spatial::azimuthOf(forward)) * std::cos(pitch) + Spatial::Vec3{0.0f, std::sin(pitch), 0.0f};
    const Spatial::Vec3 position = head.position + direction * kNoteCardDistance;
    hintPose = {facingEye(position), position};
    paintArt(kHintLayer, &FrameArt::notificationCard, FrameArt::NotificationArt{QStringLiteral("Rift Home"), title, text, QImage(), false}, 0,
             options.frameTheme);
    hintUntil = Clock::now() + std::chrono::seconds(seconds);
}

void XrCompositor::Impl::takeNotifications(LayerBridge &bridge)
{
    const uint64_t revision = bridge.notificationRevision();
    if (revision == noteRevision) {
        return;
    }
    noteRevision = revision;
    uint32_t card = 0;
    notes = bridge.notifications(card);
    const uint32_t previous = noteCard;
    noteCard = card; // before noteCardShown() below asks about it
    if (card != 0 && card != previous) {
        // A new one: its card where the user looks, a little below the middle of the view, facing the eyes
        // (placed above the dock's place, it was out of view once the dock had been put low).
        const Spatial::Vec3 forward = head.forward();
        const float pitch = std::clamp(std::asin(std::clamp(forward.y, -1.0f, 1.0f)) - kNoteCardBelowGaze, -1.2f, 0.8f);
        const Spatial::Vec3 direction = Spatial::directionOf(Spatial::azimuthOf(forward)) * std::cos(pitch) + Spatial::Vec3{0.0f, std::sin(pitch), 0.0f};
        const Spatial::Vec3 position = head.position + direction * kNoteCardDistance;
        noteCardPose = {facingEye(position), position};
        if (noteCardShown()) {
            for (int h = 0; h < 2; ++h) {
                input.haptic(h, 0.2f, 15'000'000); // a light tap: something arrived
            }
        }
        qCInfo(lcXr) << "notification card" << card << (noteCardShown() ? "shown" : "not shown (cinema or the list is open)");
    }
    for (int h = 0; h < 2; ++h) {
        notePressed[h].reset(); // rows may have moved
    }
}

int XrCompositor::Impl::noteCardActions() const
{
    for (const Notification &n : notes) {
        if (n.id == noteCard) {
            return std::min(int(n.actions.size()), Spatial::NotificationLayout::kMaxActions);
        }
    }
    return 0;
}

std::vector<int> XrCompositor::Impl::noteListActions() const
{
    std::vector<int> actions;
    for (size_t i = 0; i < std::min(notes.size(), size_t(Spatial::NotificationLayout::kMaxRows)); ++i) {
        actions.push_back(std::min(int(notes[i].actions.size()), Spatial::NotificationLayout::kMaxActions));
    }
    return actions;
}

bool XrCompositor::Impl::noteCardShown() const
{
    // Not over a film (cinema), nor while the list shows it anyway.
    if (noteCard == 0 || layout.cinemaWindow() || (dockOpen && notesOpen)) {
        return false;
    }
    return std::any_of(notes.begin(), notes.end(), [this](const Notification &n) {
        return n.id == noteCard;
    });
}

Spatial::Pose XrCompositor::Impl::noteListPose() const
{
    // Above the dock, over its bell (within the dock's width).
    using Spatial::NotificationLayout;
    const float half = std::max(0.0f, (Spatial::DockLayout::width() - NotificationLayout::listWidth()) / 2.0f);
    const float x = std::clamp(Spatial::DockLayout::rect(Spatial::DockItem::Notifications).x, -half, half);
    const float y = Spatial::DockLayout::height() / 2.0f + NotificationLayout::kGapAbove + NotificationLayout::listHeight(int(notes.size()), noteListActions()) / 2.0f;
    return {dockPose.orientation, dockPose.transform({x, y, 0.0f})};
}

std::optional<XrCompositor::Impl::NoteHit> XrCompositor::Impl::noteHitTest(Spatial::Vec3 origin, Spatial::Vec3 direction) const
{
    std::optional<NoteHit> best;
    const auto test = [&](const Spatial::Pose &pose, bool card) {
        const Spatial::Vec3 normal = pose.orientation.rotate({0.0f, 0.0f, 1.0f});
        const float denom = direction.dot(normal);
        if (denom > -1e-4f) {
            return;
        }
        const float t = (pose.position - origin).dot(normal) / denom;
        if (t <= 0.0f || (best && best->distance <= t)) {
            return;
        }
        const Spatial::Vec3 point = origin + direction * t;
        const Spatial::Vec3 local = pose.inverse().transform(point);
        const int count = int(notes.size());
        if (card) {
            const Spatial::NotificationHit hit = Spatial::NotificationLayout::cardHit(local.x, local.y, noteCardActions());
            if (hit.part != Spatial::NotificationHit::Part::None) {
                best = NoteHit{true, hit, t, point};
            }
        } else if (const std::vector<int> actions = noteListActions(); Spatial::NotificationLayout::listContains(local.x, local.y, count, actions)) {
            best = NoteHit{false, Spatial::NotificationLayout::listHit(local.x, local.y, count, actions), t, point}; // its glass takes the ray too
        }
    };
    if (noteCardShown()) {
        test(noteCardPose, true);
    }
    if (dockOpen && notesOpen) {
        test(noteListPose(), false);
    }
    return best;
}

void XrCompositor::Impl::performNote(LayerBridge &bridge, const NoteHit &hit)
{
    using Part = Spatial::NotificationHit::Part;
    uint32_t id = 0;
    if (hit.card) {
        id = noteCard;
    } else if (hit.hit.row >= 0 && size_t(hit.hit.row) < notes.size()) {
        id = notes[size_t(hit.hit.row)].id;
    }
    PointerEvent event;
    switch (hit.hit.part) {
    case Part::Card:
        event.type = PointerEvent::Type::NotificationActivate;
        break;
    case Part::Close:
        event.type = PointerEvent::Type::NotificationDismiss;
        break;
    case Part::ClearAll:
        event.type = PointerEvent::Type::NotificationDismiss;
        id = 0;
        break;
    case Part::Action:
        event.type = PointerEvent::Type::NotificationAction;
        event.index = hit.hit.action;
        break;
    case Part::None:
        return;
    }
    if (id == 0 && hit.hit.part != Part::ClearAll) {
        return;
    }
    event.button = id;
    bridge.sendInput(event);
    qCInfo(lcXr).noquote() << QStringLiteral("notification %1: %2")
                                  .arg(id)
                                  .arg(hit.hit.part == Part::Card       ? QStringLiteral("tapped")
                                           : hit.hit.part == Part::Close  ? QStringLiteral("closed")
                                           : hit.hit.part == Part::Action ? QStringLiteral("action button %1").arg(hit.hit.action)
                                                                          : QStringLiteral("all cleared"));
}

void XrCompositor::Impl::performDock(LayerBridge &bridge, int hand, Spatial::DockItem item)
{
    const auto command = [&](PointerEvent::Type type) {
        PointerEvent e;
        e.type = type;
        bridge.sendInput(e);
    };
    switch (item) {
    case Spatial::DockItem::Home:
        qCInfo(lcXr) << "dock: home";
        openLauncher(!launcherOpen);
        break;
    case Spatial::DockItem::Keyboard:
        // One keyboard, for the focused window (the one last clicked; its grab bar shows while typing).
        qCInfo(lcXr) << "dock: keyboard";
        if (keyboardOpen) {
            closeKeyboard(bridge);
        } else {
            openKeyboard(false, Spatial::azimuthOf(head.forward()));
        }
        break;
    case Spatial::DockItem::FollowAll: {
        const bool on = !layout.followAll();
        layout.setFollowAll(on, head);
        qCInfo(lcXr) << "dock: follow all" << (on ? "on" : "off");
        break;
    }
    case Spatial::DockItem::Settings: {
        // A toggle, as the dock's keyboard and home (it opened Settings but never closed it): open,
        // Settings closes; else it starts (new windows open in front). Home closes first: it fades the windows out
        // (Settings was once brought in front five times, unseen behind home).
        if (launcherOpen) {
            openLauncher(false);
        }
        const int window = settingsWindow();
        if (window) {
            PointerEvent e;
            e.type = PointerEvent::Type::Close;
            e.windowId = window;
            bridge.sendInput(e);
            qCInfo(lcXr) << "dock: settings closed";
        } else if (options.settingsApp >= 0) {
            PointerEvent e;
            e.type = PointerEvent::Type::Launch;
            e.button = uint32_t(options.settingsApp);
            bridge.sendInput(e);
            settingsToFront = true; // in front of the user when its window appears (new windows open beside the others)
            qCInfo(lcXr) << "dock: settings started";
        }
        break;
    }
    case Spatial::DockItem::Recline:
        // The dock was placed below the gaze before the turn: it closes, X shows it again below the new one.
        openDock(false);
        setRecline(!reclined, dockOpenedHead);
        break;
    case Spatial::DockItem::Gamepad:
        setGameMode(bridge, !gameMode);
        break;
    case Spatial::DockItem::Screenshot:
        // The room without the dock in it: it closes, the picture is taken once it has faded (about a third of a second).
        openDock(false);
        screenshotInFrames = 30;
        qCInfo(lcXr) << "dock: screenshot";
        break;
    case Spatial::DockItem::Notifications:
        notesOpen = !notesOpen;
        noteListArt.reset();
        qCInfo(lcXr) << "dock: notifications" << (notesOpen ? "shown" : "hidden") << notes.size();
        break;
    case Spatial::DockItem::Network: {
        // Settings on its network page: the open window in front (and switched to that page), else Settings starts there.
        if (launcherOpen) {
            openLauncher(false);
        }
        if (const int window = settingsWindow()) {
            makeMain(window, "Settings in front (network)");
        }
        PointerEvent e;
        e.type = PointerEvent::Type::OpenSettings;
        e.button = 0;
        bridge.sendInput(e);
        qCInfo(lcXr) << "dock: network (Settings' network page)";
        break;
    }
    case Spatial::DockItem::VolumeDown:
        qCInfo(lcXr) << "dock: volume down";
        command(PointerEvent::Type::VolumeDown);
        break;
    case Spatial::DockItem::VolumeUp:
        qCInfo(lcXr) << "dock: volume up";
        command(PointerEvent::Type::VolumeUp);
        break;
    case Spatial::DockItem::Volume:
        qCInfo(lcXr) << "dock: mute on/off";
        command(PointerEvent::Type::MuteToggle);
        break;
    default:
        return;
    }
    input.haptic(hand, 0.45f, 18'000'000);
}

void XrCompositor::Impl::typeIntoSearch(LayerBridge &bridge, int key, bool upper)
{
    const Spatial::KeyDef &def = keyboard.keys()[size_t(key)];
    if (def.kind != Spatial::KeyDef::Kind::Key) {
        return; // Shift, Ctrl, Alt: their state only
    }
    switch (def.code) {
    case KEY_BACKSPACE:
        homeQuery.chop(1);
        break;
    case KEY_ESC:
        homeQuery.clear();
        break;
    case KEY_SPACE:
        homeQuery += QLatin1Char(' ');
        break;
    case KEY_ENTER: {
        // The first match starts.
        const std::vector<int> shown = homeApps();
        if (!shown.empty()) {
            PointerEvent e;
            e.type = PointerEvent::Type::Launch;
            e.button = uint32_t(shown.front());
            bridge.sendInput(e);
            qCInfo(lcXr).noquote() << "launcher: starting" << options.apps[size_t(shown.front())].name << "(search)";
            openLauncher(false);
        }
        return;
    }
    default:
        if (def.label.size() == 1) {
            homeQuery += upper ? (def.shiftLabel.isEmpty() ? def.label.toUpper() : def.shiftLabel) : def.label.toLower();
        }
        break;
    }
    launcherPage = 0;
}

void XrCompositor::Impl::openLauncher(bool open)
{
    if (!open) {
        // The search ends with home (and the keyboard it opened).
        homeQuery.clear();
        if (homeSearching && keyboardForSearch && keyboardOpen && bridgeForStatus) {
            closeKeyboard(*bridgeForStatus);
        }
        homeSearching = false;
        keyboardForSearch = false;
    }
    if (open) {
        // In front of where the user looks, a little nearer than the windows, facing them (the windows fade out).
        const float azimuth = Spatial::azimuthOf(head.forward());
        const Spatial::Vec3 position = head.position + Spatial::directionOf(azimuth) * 1.5f + Spatial::Vec3{0.0f, -0.05f, 0.0f};
        launcherPose = {Spatial::Quat::fromYaw(Spatial::facingYaw(position, head.position)), position};
    }
    for (int h = 0; h < 2; ++h) {
        cellHits[h].reset();
        cellPressed[h] = -1;
    }
    if (open && dockOpen) {
        openDock(false); // it would sit in front of home's lower rows and take the rays meant for them
    }
    if (open && !launcherOpen) {
        qCInfo(lcXr) << "home open";
    }
    if (!open && launcherOpen) {
        qCInfo(lcXr).noquote() << QStringLiteral("home closed after %1 frame(s): a ray on home in %2, on an app in %3")
                                      .arg(homeFrames)
                                      .arg(homeFramesOnHome)
                                      .arg(homeFramesOnApp);
    }
    homeFrames = homeFramesOnHome = homeFramesOnApp = 0;
    launcherOpen = open;
    if (open) {
        qCInfo(lcXr).noquote() << QStringLiteral("launcher open: %1 app(s)").arg(options.apps.size());
    }
}

std::optional<XrCompositor::Impl::CellHit> XrCompositor::Impl::launcherHitTest(Spatial::Vec3 origin, Spatial::Vec3 direction) const
{
    const Spatial::Vec3 normal = launcherPose.orientation.rotate({0.0f, 0.0f, 1.0f});
    const float denom = direction.dot(normal);
    if (denom > -1e-4f) {
        return std::nullopt;
    }
    const float t = (launcherPose.position - origin).dot(normal) / denom;
    if (t <= 0.0f) {
        return std::nullopt;
    }
    const Spatial::Vec3 point = origin + direction * t;
    const Spatial::Vec3 local = launcherPose.inverse().transform(point);
    if (!Spatial::LauncherGrid::contains(local.x, local.y)) {
        return std::nullopt;
    }
    int cell = Spatial::LauncherGrid::cellAt(local.x, local.y);
    if (cell >= 0 && launcherPage * Spatial::LauncherGrid::kPerPage + cell >= int(homeApps().size())) {
        cell = -1; // an empty cell on the last page
    }
    CellHit hit{cell, t, point};
    hit.search = Spatial::LauncherGrid::onSearch(local.x, local.y);
    if (cell < 0 && !hit.search) {
        hit.card = Spatial::LauncherGrid::cardAt(local.x, local.y, int(runningWindows.size()), hit.close);
    }
    return hit;
}

Spatial::Quat XrCompositor::Impl::facingEye(Spatial::Vec3 position) const
{
    const Spatial::Vec3 z = (head.position - position).normalized();
    const Spatial::Vec3 x = Spatial::Vec3{0.0f, 1.0f, 0.0f}.cross(z).normalized();
    return Spatial::Quat::fromBasis(x, z.cross(x), z);
}

void XrCompositor::Impl::beginKeyboardGrab(int hand, bool byGrip, const KeyHit &hit)
{
    keyboardGrab = KeyboardGrab{hand, byGrip, hit.distance,
                                keyboardPose.orientation.conjugate().rotate(keyboardPose.position - hit.point)};
    input.haptic(hand, 0.4f, 20'000'000);
}

void XrCompositor::Impl::updateKeyboardGrab(float dt)
{
    const XrInput::Hand &hand = hands[keyboardGrab->hand];
    // The grabbed point stays on the ray; the thumbstick pulls it nearer or pushes it away; it keeps facing the eyes.
    keyboardGrab->distance = std::clamp(keyboardGrab->distance * std::exp(deadzone(hand.stickY) * 1.2f * dt), 0.3f, 2.0f);
    const Spatial::Vec3 target = hand.aim.position + hand.aim.forward() * keyboardGrab->distance;
    Spatial::Vec3 position = target;
    for (int i = 0; i < 3; ++i) {
        position = target + facingEye(position).rotate(keyboardGrab->offsetLocal);
    }
    keyboardPose = {facingEye(position), position};
}

void XrCompositor::Impl::endKeyboardGrab()
{
    // Remembered relative to the gaze: it opens there next time.
    const Spatial::Vec3 relative = keyboardPose.position - head.position;
    keyboardAzimuth = Spatial::angleDifference(Spatial::azimuthOf(relative), Spatial::azimuthOf(head.forward()));
    keyboardDistance = std::max(0.3f, std::sqrt(relative.x * relative.x + relative.z * relative.z));
    keyboardHeight = relative.y;
    keyboardGrab.reset();
    savePending = true; // kept with the placements
    qCInfo(lcXr).noquote() << QStringLiteral("keyboard moved: %1 m away, %2 m below the eyes")
                                  .arg(keyboardDistance, 0, 'f', 2)
                                  .arg(-keyboardHeight, 0, 'f', 2);
}

void XrCompositor::Impl::beginDockGrab(int hand, bool byGrip, const DockHit &hit)
{
    dockGrab = DockGrab{hand, byGrip, hit.distance, dockPose.orientation.conjugate().rotate(dockPose.position - hit.point)};
    input.haptic(hand, 0.4f, 20'000'000);
}

void XrCompositor::Impl::updateDockGrab(float dt)
{
    const XrInput::Hand &hand = hands[dockGrab->hand];
    // As the keyboard: the grabbed point stays on the ray, the thumbstick nearer/farther, facing the eyes.
    dockGrab->distance = std::clamp(dockGrab->distance * std::exp(deadzone(hand.stickY) * 1.2f * dt), kDockMinDistance, kDockMaxDistance);
    const Spatial::Vec3 target = hand.aim.position + hand.aim.forward() * dockGrab->distance;
    Spatial::Vec3 position = target;
    for (int i = 0; i < 3; ++i) {
        position = target + facingEye(position).rotate(dockGrab->offsetLocal);
    }
    // Kept within reach: its distance (horizontal), height and angle from the gaze within the dock's bounds.
    Spatial::Vec3 relative = position - head.position;
    const float gaze = Spatial::azimuthOf(head.forward());
    const float azimuth = gaze + std::clamp(Spatial::angleDifference(Spatial::azimuthOf(relative), gaze), -kDockMaxAzimuth, kDockMaxAzimuth);
    const float horizontal = std::clamp(std::sqrt(relative.x * relative.x + relative.z * relative.z), kDockMinDistance, kDockMaxDistance);
    position = head.position + Spatial::directionOf(azimuth) * horizontal + Spatial::Vec3{0.0f, std::clamp(relative.y, kDockLowest, kDockHighest), 0.0f};
    dockPose = {facingEye(position), position};
}

void XrCompositor::Impl::endDockGrab()
{
    const Spatial::Vec3 relative = dockPose.position - head.position;
    dockAzimuth = std::clamp(Spatial::angleDifference(Spatial::azimuthOf(relative), Spatial::azimuthOf(head.forward())), -kDockMaxAzimuth, kDockMaxAzimuth);
    dockDistance = std::clamp(std::sqrt(relative.x * relative.x + relative.z * relative.z), kDockMinDistance, kDockMaxDistance);
    dockHeight = std::clamp(relative.y, kDockLowest, kDockHighest);
    dockGrab.reset();
    savePending = true; // kept with the placements: it opens there next time, also after a restart
    qCInfo(lcXr).noquote() << QStringLiteral("dock moved: %1 m away, %2 m below the eyes").arg(dockDistance, 0, 'f', 2).arg(-dockHeight, 0, 'f', 2);
}

void XrCompositor::Impl::closeKeyboard(LayerBridge &bridge)
{
    keyboardGrab.reset();
    for (int h = 0; h < 2; ++h) {
        if (keyHeld[h] >= 0) {
            sendKeys(bridge, keyboard.release(keyHeld[h]));
            keyHeld[h] = -1;
        }
        keyHits[h].reset();
    }
    keyboardOpen = false;
    keyboardAuto = false;
    qCInfo(lcXr) << "keyboard closed";
}

std::optional<XrCompositor::Impl::KeyHit> XrCompositor::Impl::keyboardHitTest(Spatial::Vec3 origin, Spatial::Vec3 direction) const
{
    const Spatial::Vec3 normal = keyboardPose.orientation.rotate({0.0f, 0.0f, 1.0f});
    const float denom = direction.dot(normal);
    if (denom > -1e-4f) {
        return std::nullopt;
    }
    const float t = (keyboardPose.position - origin).dot(normal) / denom;
    if (t <= 0.0f) {
        return std::nullopt;
    }
    const Spatial::Vec3 point = origin + direction * t;
    const Spatial::Vec3 local = keyboardPose.inverse().transform(point);
    const float barY = -keyboard.height() / 2.0f - kKeyboardBarGap;
    if (std::fabs(local.x) <= kKeyboardBarWidth / 2.0f + 0.02f && std::fabs(local.y - barY) <= 0.025f) {
        return KeyHit{-1, t, point, true};
    }
    if (!keyboard.contains(local.x, local.y)) {
        return std::nullopt;
    }
    return KeyHit{keyboard.hit(local.x, local.y), t, point, false};
}

void XrCompositor::Impl::sendKeys(LayerBridge &bridge, const std::vector<Spatial::KeyEvent> &events)
{
    for (const Spatial::KeyEvent &k : events) {
        PointerEvent e;
        e.type = PointerEvent::Type::Key;
        e.button = k.code;
        e.pressed = k.pressed;
        bridge.sendInput(e);
    }
}

QString XrCompositor::Impl::labelFor(int windowId, Spatial::FrameControl control) const
{
    const auto placed = layout.windows().find(windowId);
    const auto size = sizeStates.find(windowId);
    const bool fullscreen = size != sizeStates.end() && size->second.first;
    const bool maximized = size != sizeStates.end() && size->second.second;
    switch (control) {
    case Spatial::FrameControl::Follow:
        return placed != layout.windows().end() && placed->second.follow ? QStringLiteral("Stop following") : QStringLiteral("Follow me");
    case Spatial::FrameControl::Main:
        return QStringLiteral("Make main");
    case Spatial::FrameControl::Pin:
        return placed != layout.windows().end() && placed->second.pinned ? QStringLiteral("Unpin") : QStringLiteral("Pin here");
    case Spatial::FrameControl::Size:
        return fullscreen ? QStringLiteral("Normal size") : maximized ? QStringLiteral("Fullscreen") : QStringLiteral("Maximize");

    case Spatial::FrameControl::Cinema:
        return layout.cinemaWindow() == windowId ? QStringLiteral("Leave cinema") : QStringLiteral("Cinema");
    case Spatial::FrameControl::Minimize:
        return QStringLiteral("Minimise");
    case Spatial::FrameControl::Close:
        return QStringLiteral("Close");
    default:
        return {};
    }
}

void XrCompositor::Impl::updateChromeArt()
{
    const int hovered = homeHovered();
    if (hovered >= 0 && hovered != homePlateArt && hovered < int(options.apps.size())) {
        paintArt(kHomePlateLayer, &FrameArt::homePlate, options.apps[size_t(hovered)].icon, options.frameTheme);
        homePlateArt = hovered;
    }
    if (!dragArt) {
        paintArt(kDragLayer, &FrameArt::dragBadge, options.frameTheme);
        dragArt = true;
    }
    if (dockOpen) {
        FrameArt::Dock state;
        state.keyboard = keyboardOpen;
        state.followAll = layout.followAll();
        state.reclined = reclined;
        state.settings = settingsWindow() != 0;
        state.gamepad = gameMode;
        state.notifications = int(notes.size());
        state.notificationsOpen = notesOpen;
        state.network = bridgeForStatus ? int(bridgeForStatus->network()) : 0;
        state.signal = bridgeForStatus ? bridgeForStatus->wifiSignal() : 0;
        state.vpn = bridgeForStatus && bridgeForStatus->vpn();
        state.volume = bridgeForStatus ? bridgeForStatus->volume() : -1;
        state.muted = bridgeForStatus && bridgeForStatus->muted();
        state.time = QTime::currentTime().toString(options.use24Hour ? QStringLiteral("HH:mm") : QStringLiteral("h:mm AP"));
        if (dockArt != state) {
            paintArt(kDockLayer, &FrameArt::dock, state, options.frameTheme);
            dockArt = state;
        }
    }
    // Notifications: the card and the dock's list, drawn again when they or the ray on them change.
    std::pair<int, int> cardHover{0, -1};
    std::pair<int, int> listHover{0, -1};
    for (int h : {1 - activeHand, activeHand}) { // the pointing hand last: it wins
        const std::optional<NoteHit> &on = notePressed[h] ? notePressed[h] : noteHits[h];
        if (on) {
            const int part = on->hit.part == Spatial::NotificationHit::Part::Action ? FrameArt::kHoveredAction + on->hit.action : int(on->hit.part);
            (on->card ? cardHover : listHover) = std::make_pair(part, on->hit.row);
        }
    }
    const auto artOf = [](const Notification &n) {
        return FrameArt::NotificationArt{n.app, n.summary, n.body, n.icon, n.critical, n.actions};
    };
    if (noteCardShown()) {
        const auto key = std::make_tuple(noteRevision, cardHover.first, cardHover.second);
        if (noteCardArt != key) {
            for (const Notification &n : notes) {
                if (n.id == noteCard) {
                    paintArt(kNoteCardLayer, &FrameArt::notificationCard, artOf(n), cardHover.first, options.frameTheme);
                }
            }
            noteCardArt = key;
        }
    }
    if (dockOpen && notesOpen) {
        const auto key = std::make_tuple(noteRevision, listHover.first, listHover.second);
        if (noteListArt != key) {
            QList<FrameArt::NotificationArt> shown;
            for (size_t i = 0; i < std::min(notes.size(), size_t(Spatial::NotificationLayout::kMaxRows)); ++i) {
                shown << artOf(notes[i]);
            }
            paintArt(kNoteListLayer, &FrameArt::notificationList, shown, int(notes.size()), listHover.second, listHover.first, options.frameTheme);
            noteListArt = key;
        }
    }
    QString runningKey = QStringLiteral("%1|%2|").arg(homeQuery).arg(homeSearching);
    for (const RunningWindow &w : runningWindows) {
        runningKey += QStringLiteral("%1:%2:%3:%4;").arg(w.windowId).arg(w.title).arg(w.minimized).arg(w.vrApp);
    }
    if (launcherOpen && (launcherArtPage != launcherPage || launcherArtRunning != runningKey)) {
        launcherArtRunning = runningKey;
        QList<FrameArt::RunningApp> runningApps;
        for (const RunningWindow &w : runningWindows) {
            runningApps.push_back({w.title, w.vr ? QImage() : iconForApp(w.appId), w.minimized, w.vr});
        }
        QStringList names;
        QList<QImage> icons;
        const std::vector<int> shown = homeApps();
        const size_t first = size_t(launcherPage) * Spatial::LauncherGrid::kPerPage;
        for (size_t i = first; i < std::min(shown.size(), first + Spatial::LauncherGrid::kPerPage); ++i) {
            names << options.apps[size_t(shown[i])].name;
            icons << options.apps[size_t(shown[i])].icon;
        }
        paintArt(kLauncherLayer, &FrameArt::launcher, names, icons, launcherPage, Spatial::LauncherGrid::pages(int(shown.size())),
                                                                    runningApps, homeQuery, homeSearching, options.frameTheme);
        launcherArtPage = launcherPage;
    }
    bool barHovered = keyboardGrab.has_value();
    for (int h = 0; h < 2; ++h) {
        barHovered |= keyHits[h] && keyHits[h]->bar;
    }
    const int barState = keyboardGrab ? int(FrameArt::State::Pressed) : barHovered ? int(FrameArt::State::Hovered) : int(FrameArt::State::Shown);
    if (keyboardBarArt != barState) {
        paintArt(kKeyboardBarLayer, &FrameArt::bar, false, kKeyboardBarWidth, FrameArt::State(barState), options.frameTheme);
        keyboardBarArt = barState;
    }
    bool dockBarHovered = dockGrab.has_value();
    for (int h = 0; h < 2; ++h) {
        dockBarHovered |= dockHits[h] && dockHits[h]->bar;
    }
    const int dockBarState = dockGrab ? int(FrameArt::State::Pressed) : dockBarHovered ? int(FrameArt::State::Hovered) : int(FrameArt::State::Shown);
    if (dockOpen && dockBarArt != dockBarState) {
        paintArt(kDockBarLayer, &FrameArt::bar, false, kDockBarWidth, FrameArt::State(dockBarState), options.frameTheme);
        dockBarArt = dockBarState;
    }
    const std::array<bool, 4> keyboardState{keyboard.shift(), keyboard.capsLock(), keyboard.ctrl(), keyboard.alt()};
    if (keyboardArt != keyboardState) {
        paintArt(kKeyboardLayer, &FrameArt::keyboard, keyboard, options.frameTheme);
        keyboardArt = keyboardState;
    }
    layoutChrome(); // after this frame's moves
    const auto now = Clock::now();
    for (auto &[id, c] : chrome) {
        const auto placed = layout.windows().find(id);
        if (placed == layout.windows().end()) {
            continue;
        }
        const Spatial::SpatialWindow &window = placed->second;
        const bool dragged = (chromeDrag && chromeDrag->windowId == id) || layout.grabbedWindow() == id;
        c.hovered = Spatial::FrameControl::None;
        for (int h : {1 - activeHand, activeHand}) { // the pointing hand last: it wins
            if (chromeHits[h] && chromeHits[h]->windowId == id && !(chromeDrag && chromeDrag->hand == h)) {
                c.hovered = chromeHits[h]->control;
            }
        }
        c.visible = dragged || now - c.lastNear < std::chrono::milliseconds(600);
        const auto stateOf = [&](Spatial::FrameControl control) {
            if (chromeDrag && chromeDrag->windowId == id && chromeDrag->control == control) {
                return FrameArt::State::Pressed;
            }
            if (c.hovered == control || (control == Spatial::FrameControl::Grab && layout.grabbedWindow() == id)) {
                return FrameArt::State::Hovered;
            }
            return c.visible ? FrameArt::State::Shown : FrameArt::State::Idle;
        };
        const float u = c.frame.unit;

        FrameArt::Menu menu;
        menu.hovered = menuIndex(c.hovered);
        menu.pressed = chromeDrag && chromeDrag->windowId == id ? menuIndex(chromeDrag->control) : -1;
        menu.follow = window.follow;
        menu.pinned = window.pinned;
        menu.cinema = layout.cinemaWindow() == id;
        const auto size = sizeStates.find(id);
        if (size != sizeStates.end()) {
            menu.size = size->second.first ? FrameArt::SizeAction::Restore
                : size->second.second      ? FrameArt::SizeAction::Fullscreen
                                           : FrameArt::SizeAction::Maximize;
        }
        if (c.menuArt != menu) {
            paintArt(chromeLayer(id, kMenuPart), &FrameArt::menu, menu, options.frameTheme);
            c.menuArt = menu;
        }
        const QString text = labelFor(id, c.hovered);
        if (!text.isEmpty() && text != c.labelArt && menuIndex(c.hovered) >= 0) {
            paintArt(chromeLayer(id, kLabelPart), &FrameArt::label, text, options.frameTheme);
            c.labelArt = text;
        }
        // Bars: drawn at their length (to a hundredth of a unit) and state.
        const auto bar = [&](int part, std::optional<std::pair<int, int>> &art, bool vertical, float length, Spatial::FrameControl control) {
            const std::pair key{int(std::lround(length / u * 100.0f)), int(stateOf(control))};
            if (art != key) {
                paintArt(chromeLayer(id, part), &FrameArt::bar, vertical, key.first / 100.0f, stateOf(control), options.frameTheme);
                art = key;
            }
        };
        bar(kTiltPart, c.tiltArt, true, c.frame.tilt.height, Spatial::FrameControl::Tilt);
        bar(kGrabPart, c.grabArt, false, c.frame.grab.width, Spatial::FrameControl::Grab);
        const std::pair resetKey{0, int(stateOf(Spatial::FrameControl::ResetTilt))};
        if (c.resetArt != resetKey) {
            paintArt(chromeLayer(id, kResetPart), &FrameArt::resetDot, stateOf(Spatial::FrameControl::ResetTilt), options.frameTheme);
            c.resetArt = resetKey;
        }
        if (cylinderLayers) {
            // The curvature slider: its track (lit with the knob), the knob, the flat dot.
            bar(kCurvePart, c.curveArt, false, c.frame.curve.width, Spatial::FrameControl::Curve);
            const std::pair knobKey{0, int(stateOf(Spatial::FrameControl::Curve))};
            if (c.knobArt != knobKey) {
                paintArt(chromeLayer(id, kKnobPart), &FrameArt::knob, stateOf(Spatial::FrameControl::Curve), options.frameTheme);
                c.knobArt = knobKey;
            }
            const std::pair flatKey{0, int(stateOf(Spatial::FrameControl::ResetCurve))};
            if (c.flatArt != flatKey) {
                paintArt(chromeLayer(id, kFlatPart), &FrameArt::flatDot, stateOf(Spatial::FrameControl::ResetCurve), options.frameTheme);
                c.flatArt = flatKey;
            }
        }
        const std::pair resizeKey{0, int(stateOf(Spatial::FrameControl::Resize))};
        if (c.resizeArt != resizeKey) {
            paintArt(chromeLayer(id, kResizePart), &FrameArt::resizeDot, stateOf(Spatial::FrameControl::Resize), options.frameTheme);
            c.resizeArt = resizeKey;
        }
        const std::pair scaleKey{int(std::lround(c.frame.cornerRadius / u * 1000.0f)), int(stateOf(Spatial::FrameControl::Scale))};
        if (c.scaleArt != scaleKey) {
            paintArt(chromeLayer(id, kScalePart), &FrameArt::scaleCorner, scaleKey.first / 1000.0f, stateOf(Spatial::FrameControl::Scale), options.frameTheme);
            c.scaleArt = scaleKey;
        }
    }
}

bool XrCompositor::Impl::selfTestHand(XrInput::Hand &hand)
{
    if (!focused) {
        return false;
    }
    if (!selfTestSeenFocus) {
        selfTestSeenFocus = true;
        focusedAt = Clock::now();
    }
    const float t = std::chrono::duration<float>(Clock::now() - focusedAt).count();
    if (t < 4.0f || t > 130.0f || layout.windows().empty()) {
        return false;
    }
    if (selfTestWindow == 0) {
        selfTestWindow = layout.windows().begin()->first;
    }
    const auto it = layout.windows().find(selfTestWindow);
    if (it == layout.windows().end()) {
        return false;
    }
    const auto step = [&](int n, const QString &what) {
        if (selfTestStep < n) {
            selfTestStep = n;
            qCInfo(lcXr).noquote() << "self-test:" << what;
        }
    };
    // A hand below and right of the head, as a controller is held.
    const Spatial::Vec3 origin = head.transform({0.15f, -0.25f, -0.1f});
    const Spatial::Vec3 toWindow = (it->second.pose.position - origin).normalized();
    Spatial::Vec3 direction = toWindow;
    if (t < 6.0f) {
        step(1, QStringLiteral("point at the centre of window %1").arg(selfTestWindow));
        if (t >= 5.0f && t < 5.15f) {
            step(2, QStringLiteral("click"));
            hand.trigger = 1.0f;
        }
    } else if (t < 8.5f) {
        if (selfTestStep < 3) {
            selfTestGrabDirection = toWindow;
        }
        // To the left: away from the second window (on the right), so its frame stays clear of it.
        step(3, QStringLiteral("grab it and turn the ray 15 degrees to the left"));
        hand.squeeze = 1.0f;
        const float k = std::clamp((t - 6.2f) / 2.0f, 0.0f, 1.0f);
        direction = Spatial::Quat::fromYaw(Spatial::radians(15.0f * k)).rotate(selfTestGrabDirection);
    } else if (t < 9.0f) {
        step(4, QStringLiteral("let go"));
        direction = Spatial::Quat::fromYaw(Spatial::radians(15.0f)).rotate(selfTestGrabDirection);
    } else if (t < 10.0f) {
        step(5, QStringLiteral("point at it again and scroll down"));
        if (t < 9.6f) {
            hand.stickY = -0.6f;
        }
    } else if (t < 12.0f) {
        step(6, QStringLiteral("rest"));
    } else {
        // The window frame: scale handle, tilt bar, the menu's pin and size buttons.
        const auto framed = chrome.find(selfTestWindow);
        if (framed == chrome.end()) {
            return false;
        }
        const Spatial::FrameLayout &f = framed->second.frame;
        const Spatial::Pose &surface = framed->second.surface;
        const auto toward = [&](Spatial::Vec3 local) {
            return (surface.transform(local) - origin).normalized();
        };
        const auto towardOn = [&](float x, float y) { // on its surface, flat or curved
            return (layout.pointOn(it->second, x, y).position - origin).normalized();
        };
        if (t < 13.0f) {
            step(8, QStringLiteral("point at the scale handle's bottom arm (the frame shows)"));
            selfTestLocal = {f.scale.x - f.scale.width / 2.0f + 0.02f * f.unit, -f.windowHeight / 2.0f - (Spatial::FrameLayout::kGap + Spatial::FrameLayout::kBarThickness / 2.0f) * f.unit, 0.0f};
            direction = toward(selfTestLocal);
        } else if (t < 14.5f) {
            step(9, QStringLiteral("drag it outward: 1.25 times as large"));
            hand.trigger = 1.0f;
            const float k = 1.0f + 0.25f * std::clamp((t - 13.2f) / 1.0f, 0.0f, 1.0f);
            direction = toward(selfTestLocal * k);
        } else if (t < 15.0f) {
            step(10, QStringLiteral("let go"));
            direction = toward(selfTestLocal * 1.25f);
        } else if (t < 16.0f) {
            if (selfTestStep < 11) {
                selfTestLocal = {f.tilt.x, f.tilt.y, 0.0f};
            }
            step(11, QStringLiteral("tilt bar: thumbstick up half way for half a second (the face turns up about 12 degrees)"));
            direction = toward(selfTestLocal);
            if (t >= 15.3f && t < 15.8f) {
                hand.stickY = 0.5f;
            }
        } else if (t < 17.0f) {
            step(12, QStringLiteral("click Pin (the thumbstick tilted the window to %1 degrees)").arg(it->second.tilt * 180.0f / float(M_PI), 0, 'f', 1));
            direction = toward({f.buttons[2].x, f.buttons[2].y, 0.0f});
            if (t >= 16.4f && t < 16.6f) {
                hand.trigger = 1.0f;
            }
        } else if (t < 30.0f) {
            step(13, QStringLiteral("hover the size button (frame and its label stay for the captures)"));
            direction = toward({f.buttons[3].x, f.buttons[3].y, 0.0f});
        } else if (t < 31.0f) {
            step(14, QStringLiteral("click the size button (maximize)"));
            direction = toward({f.buttons[3].x, f.buttons[3].y, 0.0f});
            if (t >= 30.3f && t < 30.5f) {
                hand.trigger = 1.0f;
            }
        } else if (t < 32.5f) {
            step(15, QStringLiteral("click Make main"));
            direction = toward({f.buttons[1].x, f.buttons[1].y, 0.0f});
            if (t >= 31.3f && t < 31.5f) {
                hand.trigger = 1.0f;
            }
        } else if (t < 33.5f) {
            step(16, keyboardOpen ? QStringLiteral("the keyboard is open already (Konsole's terminal is a text field)")
                                  : QStringLiteral("open the keyboard (as the dock's button does)"));
            if (!keyboardOpen) {
                openKeyboard(false, Spatial::azimuthOf(head.forward()));
            }
        } else if (t < 45.0f && keyboardOpen) {
            // Type "hi" and Enter (one key a second), then rest on K for the captures.
            const auto keyIndex = [&](const char *label) {
                for (size_t i = 0; i < keyboard.keys().size(); ++i) {
                    if (keyboard.keys()[i].label == QLatin1String(label)) {
                        return int(i);
                    }
                }
                return 0;
            };
            const char *sequence[] = {"h", "i", "Enter"};
            const int slot = int(t - 33.5f);
            const char *label = slot < 3 ? sequence[slot] : "k";
            step(17 + std::min(slot, 3), slot < 3 ? QStringLiteral("type %1 on the keyboard").arg(QLatin1String(label))
                                                  : QStringLiteral("rest on K (the keyboard stays for the captures)"));
            const Spatial::KeyRect &r = keyboard.rects()[size_t(keyIndex(label))];
            direction = (keyboardPose.transform({r.x, r.y, 0.0f}) - origin).normalized();
            const float within = (t - 33.5f) - float(slot);
            if (slot < 3 && within >= 0.4f && within < 0.6f) {
                hand.trigger = 1.0f;
            }
        } else if (t < 46.0f) {
            step(21, QStringLiteral("press the left menu button: the launcher"));
            if (!launcherOpen) {
                openLauncher(true);
            }
        } else if (t < 53.0f && launcherOpen) {
            // Rest on Konsole's cell (captures), then start a second Konsole from it.
            int index = 0;
            for (size_t i = 0; i < options.apps.size(); ++i) {
                if (options.apps[i].name == QLatin1String("Konsole")) {
                    index = int(i);
                }
            }
            launcherPage = index / Spatial::LauncherGrid::kPerPage;
            float x = 0.0f;
            float y = 0.0f;
            Spatial::LauncherGrid::cellCentre(index % Spatial::LauncherGrid::kPerPage, x, y);
            step(22, QStringLiteral("point at %1 in the launcher, then start it").arg(options.apps.empty() ? QString() : options.apps[size_t(index)].name));
            direction = (launcherPose.transform({x, y, 0.0f}) - origin).normalized();
            if (t >= 52.4f && t < 52.6f) {
                hand.trigger = 1.0f;
            }
        } else if (t < 56.0f) {
            step(23, QStringLiteral("press X: the dock"));
            if (!dockOpen) {
                openDock(true);
            }
            if (launcherOpen) {
                openLauncher(false);
            }
        } else if (t < 62.0f) {
            step(24, QStringLiteral("cinema mode for window %1 (as its menu's Cinema button)").arg(selfTestWindow));
            if (layout.cinemaWindow() != selfTestWindow) {
                selfTestMenu = Spatial::FrameControl::Cinema;
            }
        } else if (t < 64.0f) {
            step(25, QStringLiteral("leave cinema mode"));
            if (layout.cinemaWindow() == selfTestWindow) {
                selfTestMenu = Spatial::FrameControl::Cinema;
            }
        } else if (t < 65.0f) {
            // Back from cinema the window is maximized again: its size button makes it fullscreen (done as the button
            // does: a Konsole started from home may stand in front of the button by now).
            if (selfTestStep < 26) {
                selfTestMenu = Spatial::FrameControl::Size;
            }
            step(26, QStringLiteral("the size button (fullscreen)"));
        } else if (t < 67.0f) {
            step(27, QStringLiteral("point at the fullscreen window's scale handle"));
            selfTestLocal = {f.scale.x - f.scale.width / 2.0f + 0.02f * f.unit, -f.windowHeight / 2.0f - (Spatial::FrameLayout::kGap + Spatial::FrameLayout::kBarThickness / 2.0f) * f.unit, 0.0f};
            direction = toward(selfTestLocal);
        } else if (t < 68.5f) {
            // A fullscreen window's scale snapped back every frame.
            step(28, QStringLiteral("drag it inward: 0.8 times as large"));
            hand.trigger = 1.0f;
            const float k = 1.0f - 0.2f * std::clamp((t - 67.2f) / 1.0f, 0.0f, 1.0f);
            direction = toward(selfTestLocal * k);
        } else if (t < 72.0f) {
            step(29, QStringLiteral("let go and wait (the screen must keep its new size)"));
            direction = toward(selfTestLocal * 0.8f);
        } else if (t < 73.0f) {
            // The curvature slider above the window.
            step(30, QStringLiteral("point at the curvature slider's knob"));
            direction = towardOn(f.knobX(it->second.curvature), f.curve.y);
        } else if (t < 74.5f) {
            step(31, QStringLiteral("drag the knob right to 0.6 (the window bends)"));
            hand.trigger = 1.0f;
            const float k = std::clamp((t - 73.2f) / 1.0f, 0.0f, 1.0f);
            direction = towardOn(f.knobX(0.6f * k), f.curve.y);
        } else if (t < 76.0f) {
            step(32, QStringLiteral("let go"));
            direction = towardOn(f.knobX(0.6f), f.curve.y);
        } else if (t < 77.0f) {
            step(33, QStringLiteral("thumbstick right half way for half a second on the slider (about +0.15)"));
            direction = towardOn(f.knobX(it->second.curvature), f.curve.y);
            if (t >= 76.3f && t < 76.8f) {
                hand.stickX = 0.5f;
            }
        } else if (t < 82.0f) {
            step(34, QStringLiteral("rest on the slider (frame and the curved window for the captures)"));
            direction = towardOn(f.knobX(it->second.curvature), f.curve.y);
        } else if (t < 90.0f) {
            // The dock's gear: the Settings app opens in front.
            if (selfTestStep < 35) {
                selfTestDock = Spatial::DockItem::Settings;
            }
            step(35, QStringLiteral("the dock's Settings button (Settings opens)"));
        } else if (t < 92.0f) {
            // Minimize and back through Home's running apps.
            if (selfTestStep < 36) {
                selfTestMenu = Spatial::FrameControl::Minimize;
            }
            step(36, QStringLiteral("minimize window %1 (its menu's Minimise)").arg(selfTestWindow));
        } else if (t < 94.0f) {
            step(37, QStringLiteral("open home (the running apps under the apps)"));
            if (!launcherOpen) {
                openLauncher(true);
            }
        } else if (t < 97.0f && launcherOpen) {
            int card = 0;
            for (size_t i = 0; i < runningWindows.size(); ++i) {
                if (runningWindows[i].windowId == selfTestWindow) {
                    card = int(i);
                }
            }
            float cx, cy, cw, ch;
            Spatial::LauncherGrid::card(card, int(runningWindows.size()), cx, cy, cw, ch);
            step(38, QStringLiteral("point at window %1's card and tap it (back in front)").arg(selfTestWindow));
            direction = (launcherPose.transform({cx - cw * 0.25f, cy, 0.0f}) - origin).normalized();
            if (t >= 96.0f && t < 96.2f) {
                hand.trigger = 1.0f;
            }
        } else if (t < 99.0f) {
            step(39, QStringLiteral("rest"));
        } else if (t < 100.5f) {
            // Search: tap the field, type "kon" and Enter: Konsole starts.
            step(40, QStringLiteral("open home and tap its search field"));
            if (!launcherOpen) {
                openLauncher(true);
            }
            float sx, sy, sw, sh;
            Spatial::LauncherGrid::searchField(sx, sy, sw, sh);
            direction = (launcherPose.transform({sx, sy, 0.0f}) - origin).normalized();
            if (t >= 100.0f && t < 100.2f) {
                hand.trigger = 1.0f;
            }
        } else if (t < 104.5f && keyboardOpen) {
            const char *sequence[] = {"k", "o", "n", "Enter"};
            const int slot = std::min(int(t - 100.5f), 3);
            step(41 + slot, QStringLiteral("type %1 into the search").arg(QLatin1String(sequence[slot])));
            int keyIndex = 0;
            for (size_t i = 0; i < keyboard.keys().size(); ++i) {
                if (keyboard.keys()[i].label == QLatin1String(sequence[slot])) {
                    keyIndex = int(i);
                }
            }
            const Spatial::KeyRect &r = keyboard.rects()[size_t(keyIndex)];
            direction = (keyboardPose.transform({r.x, r.y, 0.0f}) - origin).normalized();
            const float within = (t - 100.5f) - float(slot);
            if (within >= 0.4f && within < 0.6f) {
                hand.trigger = 1.0f;
            }
        } else if (t < 109.0f) {
            step(45, QStringLiteral("rest"));
        } else if (t < 115.0f && settingsWindow() && chrome.contains(settingsWindow())) {
            // The resize handle, on the Settings window (in front by now): 1.2 times as large in pixels.
            const int target = settingsWindow();
            const Spatial::FrameLayout &g = chrome.at(target).frame;
            const Spatial::SpatialWindow &sw = layout.windows().at(target);
            if (t < 110.0f || selfTestStep < 47) {
                step(47, QStringLiteral("point at window %1's resize button (top-right)").arg(target));
                selfTestLocal = {g.resize.x, g.resize.y, 0.0f};
                direction = (layout.pointOn(sw, selfTestLocal.x, selfTestLocal.y).position - origin).normalized();
            } else if (t < 112.0f) {
                step(48, QStringLiteral("drag it outward: 1.2 times the pixels"));
                hand.trigger = 1.0f;
                const float k = 1.0f + 0.2f * std::clamp((t - 110.3f) / 1.2f, 0.0f, 1.0f);
                direction = (layout.pointOn(sw, selfTestLocal.x * k, selfTestLocal.y * k).position - origin).normalized();
            } else {
                // Let go where the drag ended (a hand stays on the handle as the trigger is released).
                step(49, QStringLiteral("let go (the app lays itself out at its new size)"));
                direction = (layout.pointOn(sw, selfTestLocal.x * 1.2f, selfTestLocal.y * 1.2f).position - origin).normalized();
            }
        } else if (t < 118.0f) {
            step(50, QStringLiteral("rest"));
        } else if (t < 126.0f) {
            // Notifications (the test sends two with notify-send): the dock's list, the newest
            // closed with its x, then "Clear all".
            if (selfTestStep < 51) {
                step(51, QStringLiteral("open the dock and its notifications (%1 of them)").arg(notes.size()));
                if (!dockOpen) {
                    openDock(true);
                }
                selfTestDock = Spatial::DockItem::Notifications;
            }
            const int count = int(notes.size());
            const std::vector<int> actions = noteListActions();
            if (t >= 119.0f && t < 120.0f && count > 0 && Spatial::NotificationLayout::actionsOf(actions, 0) > 0) {
                // The newest's first action button (notify-send -A prints the key it gets).
                if (selfTestStep < 52) {
                    step(52, QStringLiteral("the newest notification's first action button"));
                }
                const Spatial::FrameRect r = Spatial::NotificationLayout::actionOf(Spatial::NotificationLayout::listCard(0, count, actions), 0,
                                                                                    Spatial::NotificationLayout::actionsOf(actions, 0));
                direction = (noteListPose().transform({r.x, r.y, 0.0f}) - origin).normalized();
                if (t >= 119.5f && t < 119.7f) {
                    hand.trigger = 1.0f;
                }
            } else if (t >= 120.0f && t < 122.5f && count > 0 && (selfTestStep < 54 || t < 121.3f)) {
                step(53, QStringLiteral("close the newest notification (its x)"));
                const Spatial::FrameRect r = Spatial::NotificationLayout::closeOf(Spatial::NotificationLayout::listCard(0, count, actions));
                direction = (noteListPose().transform({r.x, r.y, 0.0f}) - origin).normalized();
                if (t >= 121.0f && t < 121.2f) {
                    hand.trigger = 1.0f;
                }
            } else if (t >= 123.0f && t < 125.5f && count > 0) {
                step(54, QStringLiteral("clear all"));
                const Spatial::FrameRect r = Spatial::NotificationLayout::clearAll(count, actions);
                direction = (noteListPose().transform({r.x, r.y, 0.0f}) - origin).normalized();
                if (t >= 124.0f && t < 124.2f) {
                    hand.trigger = 1.0f;
                }
            }
        } else {
            step(55, QStringLiteral("done"));
            if (!selfTestReported) {
                selfTestReported = true;
                const Spatial::SpatialWindow &w = it->second;
                qCInfo(lcXr).noquote() << QStringLiteral("self-test result: window %1 scale %2 (%7 m wide, fullscreen %8), tilt %3 degrees, pinned %4, frame %5, keyboard %6, curvature %9")
                                              .arg(selfTestWindow)
                                              .arg(w.scale, 0, 'f', 3)
                                              .arg(w.tilt * 180.0f / float(M_PI), 0, 'f', 1)
                                              .arg(w.pinned ? QStringLiteral("yes") : QStringLiteral("no"))
                                              .arg(framed->second.visible ? QStringLiteral("shown") : QStringLiteral("hidden"))
                                              .arg(keyboardOpen ? QStringLiteral("open") : QStringLiteral("closed"))
                                              .arg(layout.widthMeters(w), 0, 'f', 2)
                                              .arg(w.fullscreen ? QStringLiteral("yes") : QStringLiteral("no"))
                                              .arg(QStringLiteral("%1 (radius %2 m), settings window %3")
                                                       .arg(w.curvature, 0, 'f', 2)
                                                       .arg(layout.curveRadius(w), 0, 'f', 2)
                                                       .arg(settingsWindow() ? QStringLiteral("open") : QStringLiteral("not open"))
                                                   + QStringLiteral(", minimized %1, running apps %2, notifications %3 (list %4)")
                                                         .arg(w.minimized ? QStringLiteral("yes") : QStringLiteral("no"))
                                                         .arg(runningWindows.size())
                                                         .arg(notes.size())
                                                         .arg(notesOpen ? QStringLiteral("shown") : QStringLiteral("hidden")));
            }
        }
    }
    hand.tracked = true;
    hand.aim = {lookAlong(direction), origin};
    return true;
}

void XrCompositor::Impl::saveLayout(bool force)
{
    if (layout.takeDirty()) {
        savePending = true;
    }
    if (!savePending || options.stateFile.isEmpty() || (!force && Clock::now() - lastSave < std::chrono::seconds(1))) {
        return;
    }
    QJsonObject state = layout.save();
    // Where the dock and the keyboard open, relative to the gaze (as the user left them).
    state.insert(QStringLiteral("dock"), QJsonObject{{QStringLiteral("azimuth"), dockAzimuth},
                                                     {QStringLiteral("distance"), dockDistance},
                                                     {QStringLiteral("height"), dockHeight}});
    state.insert(QStringLiteral("keyboard"), QJsonObject{{QStringLiteral("azimuth"), keyboardAzimuth},
                                                         {QStringLiteral("distance"), keyboardDistance},
                                                         {QStringLiteral("height"), keyboardHeight}});
    QByteArray bytes = QJsonDocument(state).toJson();
    if (force) {
        // The last one (Rift Home ending): an earlier one still being written goes first, then this, before returning.
        layoutWriter.reset();
        FileWriter::write(options.stateFile, bytes);
    } else {
        if (!layoutWriter) {
            layoutWriter = std::make_shared<FileWriter>();
        }
        layoutWriter->post(options.stateFile, std::move(bytes));
    }
    savePending = false;
    lastSave = Clock::now();
}

namespace
{

//! The room from @p path, sized for the panel; @p panorama: it is a 360-degree image (else a photo in front). Null when
//! the file cannot be read. Takes a moment (a photo becomes a panorama): never on the frame loop.
RoomImage roomFromFile(const QString &path, float angle, bool equirectLayers, bool &panorama)
{
    if (path.endsWith(QLatin1String(".hdr"), Qt::CaseInsensitive)) {
        // A Radiance HDR panorama: its light as authored (1.0 white, as image viewers and Blender show it; a night or
        // twilight scene stays as dark as it was made, it is not brightened to an average), the highlights (windows,
        // lamps, the sun) above white, rolled off towards 25x (about 2000 nits) so a sun does not set the runtime's peak.
        const auto hdr = Graphics::loadRadianceHdr(path);
        if (!hdr) {
            qCWarning(lcXr).noquote() << "environment" << path
                                      << (QFileInfo::exists(path) ? "is not a Radiance HDR file this reads" : "does not exist") << ": the room stays black";
            return {};
        }
        RoomImage room;
        room.halves = Graphics::toHalfRgba(*hdr, 1.0f, kRoomPanoramaWidth, room.width, room.height, 25.0f);
        room.peak = roomPeak(room.halves);
        panorama = true;
        qCInfo(lcXr).noquote() << "environment" << path << QStringLiteral("%1x%2").arg(room.width).arg(room.height)
                               << "as an HDR panorama around (linear light, 16-bit float; peak"
                               << QString::number(room.peak, 'f', 1) + QStringLiteral("x white, the same wherever one looks)");
        return room;
    }
    QImage image(path);
    if (image.isNull()) {
        qCWarning(lcXr).noquote() << "environment" << path << "could not be read: the room stays black";
        return {};
    }
    // Sized to the panel's sharpness (about 13 pixels a degree; the runtime samples without mip levels). A photo
    // becomes a panorama around the user where the runtime has equirect layers.
    const bool isPanorama = std::fabs(float(image.width()) / float(image.height()) - 2.0f) < 0.02f;
    panorama = isPanorama || equirectLayers;
    const int width = panorama ? kRoomPanoramaWidth : 2048;
    if (panorama && !isPanorama) {
        // From a copy at about the size it is shown at (the photo itself can be 6000 pixels wide).
        const int front = int(width * angle / 360.0f);
        if (image.width() > front) {
            image = image.scaledToWidth(front, Qt::SmoothTransformation);
        }
        image = panoramaFromPhoto(image.convertToFormat(QImage::Format_RGBA8888_Premultiplied), angle, width);
    } else if (image.width() > width) {
        image = image.scaledToWidth(width, Qt::SmoothTransformation);
    }
    image = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    qCInfo(lcXr).noquote() << "environment" << path << QStringLiteral("%1x%2").arg(image.width()).arg(image.height())
                           << (isPanorama ? QStringLiteral("as a 360-degree panorama")
                               : panorama ? QStringLiteral("as a room around: the photo %1 degrees wide in front, its glow all around").arg(angle)
                                          : QStringLiteral("as a photo in front, curved"));
    RoomImage room;
    room.sdr = image;
    room.width = image.width();
    room.height = image.height();
    return room;
}

//! The room's image for its layer: an opaque 8-bit sRGB image, or 16-bit floats described as linear light (BT.709).
/*!
 * The brightness that 0.02 % of an HDR room's pixels reach, relative to white (the runtime's own rule for a layer's
 * peak, so a lone sparkle does not set it); at least white. Every 4th pixel: enough for a panorama.
 */
float roomPeak(const QByteArray &halves)
{
    const auto *h = reinterpret_cast<const qfloat16 *>(halves.constData());
    const size_t pixels = size_t(halves.size()) / 8;
    std::vector<float> values;
    values.reserve(pixels / 4 + 1);
    for (size_t i = 0; i < pixels; i += 4) {
        values.push_back(std::max({float(h[i * 4]), float(h[i * 4 + 1]), float(h[i * 4 + 2])}));
    }
    if (values.empty()) {
        return 1.0f;
    }
    const auto at = values.begin() + std::ptrdiff_t(double(values.size() - 1) * 0.9998);
    std::nth_element(values.begin(), at, values.end());
    return std::max(1.0f, *at);
}

std::unique_ptr<BufferHandoff> roomBuffer(const RoomImage &room)
{
    if (room.halves.isEmpty()) {
        return imageBuffer(room.sdr, true);
    }
    auto buffer = std::make_unique<BufferHandoff>();
    buffer->kind = BufferHandoff::Kind::Shm;
    buffer->drmFormat = DRM_FORMAT_ABGR16161616F;
    buffer->width = room.width;
    buffer->height = room.height;
    buffer->stride = room.width * 8;
    buffer->pixels = room.halves;
    Color::SurfaceColorDescription linear;
    linear.tagged = true;
    linear.transferNamed = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR;
    linear.primariesNamed = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB;
    buffer->plan = Color::planLayer(linear, Color::PixelKind::Float16);
    if (room.peak > 0.0f) {
        const float white = buffer->plan.hdr.referenceWhiteNits > 0.0f ? buffer->plan.hdr.referenceWhiteNits : 203.0f;
        buffer->plan.hdr.maxCllNits = room.peak * white;
    }
    buffer->alpha = false; // opaque: alpha 1 everywhere
    return buffer;
}

} // namespace

void XrCompositor::Impl::loadEnvironment()
{
    // Before the session runs frames: turning a photo into a room takes a moment.
    if (!options.environment.isEmpty()) {
        environmentImage = roomFromFile(options.environment, options.environmentAngle, equirectLayers, environmentPanorama);
    }
}

void XrCompositor::Impl::applySettings(const XrCompositor::LiveSettings &settings)
{
    const bool themeChanged = settings.frameTheme != options.frameTheme;
    options.frameTheme = settings.frameTheme;
    options.keyboardShowOnTextField = settings.keyboardShowOnTextField;
    options.roomBrightness = std::clamp(settings.roomBrightness, 0.2f, 1.0f);
    options.cinemaRoomLight = std::clamp(settings.cinemaRoomLight, 0.0f, 1.0f);
    options.roomTurnDegrees = std::clamp(settings.roomTurnDegrees, -180.0f, 180.0f);
    if (settings.pointerColour != options.pointerColour) {
        options.pointerColour = settings.pointerColour;
        pendingArt[kCursorLayer] = rgbaBuffer(cursorPixels(kCursorSize, options.pointerColour), kCursorSize, kCursorSize, kCursorSize * 4);
        pendingArt[kBeamLayer] = rgbaBuffer(beamPixels(kBeamWidth, kBeamHeight, options.pointerColour), kBeamWidth, kBeamHeight, kBeamWidth * 4);
    }
    options.pointerWidth = std::clamp(settings.pointerWidth, 0.002f, 0.02f);
    options.pointerHideSeconds = std::max(0, settings.pointerHideSeconds);
    options.use24Hour = settings.use24Hour;
    options.windowCurve = std::clamp(settings.windowCurve, 0.0f, 1.0f);
    layout.setArrangementCurve(options.windowCurve);
    options.buttons = settings.buttons;
    if (themeChanged) {
        // Every piece of art again, in the new style.
        for (auto &[id, c] : chrome) {
            c.menuArt.reset();
            c.labelArt.clear();
            c.tiltArt.reset();
            c.grabArt.reset();
            c.scaleArt.reset();
            c.resetArt.reset();
            c.curveArt.reset();
            c.knobArt.reset();
            c.flatArt.reset();
            c.resizeArt.reset();
        }
        dockArt.reset();
        dockBarArt.reset();
        noteCardArt.reset();
        noteListArt.reset();
        dragArt = false;
        keyboardArt.reset();
        keyboardBarArt.reset();
        launcherArtPage = -1;
        homePlateArt = -1;
        paintArt(kKeyHoverLayer, &FrameArt::keyHighlight, false, options.frameTheme);
        paintArt(kKeyPressLayer, &FrameArt::keyHighlight, true, options.frameTheme);
    }
    if (settings.environment != options.environment) {
        options.environment = settings.environment;
        if (options.environment.isEmpty()) {
            retireEnvironmentJob();
            destroyLayer(kEnvironmentLayer); // no room: black
        } else {
            auto job = std::make_shared<EnvironmentJob>();
            job->thread = std::thread([job = job.get(), path = options.environment, angle = options.environmentAngle, equirect = equirectLayers] {
                bool panorama = false;
                RoomImage room = roomFromFile(path, angle, equirect, panorama);
                std::lock_guard lock(job->mutex);
                job->room = std::move(room);
                job->panorama = panorama;
                job->done = true;
                job->finished = true;
            });
            retireEnvironmentJob();
            environmentJob = job;
        }
    }
    qCInfo(lcXr).noquote() << QStringLiteral("settings: frames %1, keyboard %2, room %3 at %4 % (%6 % in cinema, turned %7 degrees), clock %5")
                                  .arg(options.frameTheme == FrameArt::Theme::Light ? QStringLiteral("light") : QStringLiteral("dark"))
                                  .arg(options.keyboardShowOnTextField ? QStringLiteral("by itself") : QStringLiteral("from the dock"))
                                  .arg(options.environment.isEmpty() ? QStringLiteral("none") : options.environment)
                                  .arg(int(std::lround(options.roomBrightness * 100.0f)))
                                  .arg(options.use24Hour ? QStringLiteral("24 h") : QStringLiteral("12 h"))
                                  .arg(int(std::lround(options.cinemaRoomLight * 100.0f)))
                                  .arg(int(std::lround(options.roomTurnDegrees)));
}

void XrCompositor::Impl::uploadVisuals(LayerBridge::Frame &frame)
{
    if (!environmentImage.isNull()) {
        frame.buffers[kEnvironmentLayer] = roomBuffer(environmentImage);
        environmentImage = RoomImage();
    }
    frame.buffers[kFocusDotLayer] = imageBuffer(FrameArt::focusDot());
    frame.buffers[kKeyHoverLayer] = imageBuffer(FrameArt::keyHighlight(false, options.frameTheme));
    frame.buffers[kKeyPressLayer] = imageBuffer(FrameArt::keyHighlight(true, options.frameTheme));
    frame.buffers[kCursorLayer] = rgbaBuffer(cursorPixels(kCursorSize, options.pointerColour), kCursorSize, kCursorSize, kCursorSize * 4);
    frame.buffers[kBeamLayer] = rgbaBuffer(beamPixels(kBeamWidth, kBeamHeight, options.pointerColour), kBeamWidth, kBeamHeight, kBeamWidth * 4);
    visualsUploaded = true;
}

XrCompositor::XrCompositor(LayerBridge &bridge, const Options &options)
    : m_bridge(bridge)
    , m_options(options)
    , m_impl(std::make_unique<Impl>())
{
    m_impl->options = options;
    m_impl->layout = Spatial::SpatialLayout({options.pixelsPerMeter, options.distance, options.height, options.gapDegrees});
    m_impl->layout.setArrangementCurve(options.windowCurve);
}

XrCompositor::~XrCompositor()
{
    stop();
}

void XrCompositor::start()
{
    // Again after a stop (SteamVR had the headset): a new session from the saved places.
    m_stop = false;
    m_thread = std::thread([this] {
        run();
    });
}

void XrCompositor::applySettings(const LiveSettings &settings)
{
    std::lock_guard lock(m_settingsMutex);
    m_pendingSettings = settings;
}

void XrCompositor::stop()
{
    m_stop = true;
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void XrCompositor::run()
{
    if (!m_impl->init()) {
        qCWarning(lcXr) << "XR not started: windows stay in the flat view only";
        m_impl->shutdown();
        return;
    }
    if (const std::shared_ptr<Importer> importer = m_impl->importer) {
        m_bridge.setImportRequest([importer](const BufferHandoff &buffer) {
            importer->request(buffer);
        });
    }
    bool exit = false;
    bool exitRequested = false;
    Clock::time_point deadline;
    m_impl->bindingRecorded = m_bindingRecorded;
    while (!exit) {
        if (const int command = m_recordCommand.exchange(0)) {
            m_impl->recordCommand = command;
        }
        if (m_lensTestCommand.exchange(false)) {
            m_impl->lensTestCommand = true;
        }
        m_impl->pollEvents(exit);
        if (m_stop && !exitRequested) {
            exitRequested = true;
            deadline = Clock::now() + std::chrono::seconds(3);
            if (m_impl->running) {
                xrRequestExitSession(m_impl->session);
            } else {
                break;
            }
        }
        if (exitRequested && Clock::now() > deadline) {
            qCWarning(lcXr) << "the session did not end in 3 s";
            break;
        }
        std::optional<LiveSettings> settings;
        {
            std::lock_guard lock(m_settingsMutex);
            settings.swap(m_pendingSettings);
        }
        if (settings) {
            m_impl->applySettings(*settings);
            m_options = m_impl->options;
        }
        if (m_impl->running) {
            m_impl->frame(m_bridge, m_options);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    m_bridge.setImportRequest({}); // no more asks from the Wayland thread (one running now is waited for)
    m_impl->shutdown();
    qCInfo(lcXr) << "XR session ended";
    if (!exitRequested && m_runtimeClosed) {
        qCInfo(lcXr) << "the runtime ended the session (closed from the Rift panel, or gone): Rift Home quits";
        m_runtimeClosed();
    }
}

} // namespace RiftHome::Xr
