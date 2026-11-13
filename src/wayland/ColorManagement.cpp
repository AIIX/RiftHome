// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "wayland/ColorManagement.hpp"

#include "wayland/wlr.hpp"

#include "color-management-v1-server-protocol.h"

#include <QFile>
#include <QLoggingCategory>
#include <QSet>

#include <cmath>
#include <new>

Q_LOGGING_CATEGORY(lcColor, "rifthome.color")

namespace RiftHome::Wayland
{

namespace
{

constexpr int kVersion = 2;

const uint32_t kTransfers[] = {
    WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886,
    WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22,
    WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA28,
    WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST240,
    WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR,
    WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_LOG_100,
    WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_LOG_316,
    WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ,
    WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG,
    WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4,
};
const uint32_t kPrimaries[] = {
    WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,   WP_COLOR_MANAGER_V1_PRIMARIES_PAL_M,  WP_COLOR_MANAGER_V1_PRIMARIES_PAL,
    WP_COLOR_MANAGER_V1_PRIMARIES_NTSC,   WP_COLOR_MANAGER_V1_PRIMARIES_GENERIC_FILM, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020,
    WP_COLOR_MANAGER_V1_PRIMARIES_DCI_P3, WP_COLOR_MANAGER_V1_PRIMARIES_DISPLAY_P3, WP_COLOR_MANAGER_V1_PRIMARIES_ADOBE_RGB,
};
// Extended target volume: mastering metadata may exceed the primaries' volume (HDR10 films mastered on a P3 display in a
// BT.2020 container, up to 10000 cd/m²); Rift Home never refuses one, and Firefox tags HDR video only when it is offered
// (on KWin, which offers it, Firefox's HDR video is tagged; without it Firefox never asked).
const uint32_t kFeatures[] = {
    WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC,     WP_COLOR_MANAGER_V1_FEATURE_SET_PRIMARIES,
    WP_COLOR_MANAGER_V1_FEATURE_SET_TF_POWER,   WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES,
    WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES, WP_COLOR_MANAGER_V1_FEATURE_EXTENDED_TARGET_VOLUME,
};

template<size_t N>
bool offered(const uint32_t (&list)[N], uint32_t value)
{
    for (uint32_t v : list) {
        if (v == value) {
            return true;
        }
    }
    return false;
}

//! An image description record: immutable, shared by the protocol objects and surface states that refer to it.
struct Record {
    uint64_t identity = 0;
    Color::SurfaceColorDescription description; //!< tagged
    Color::Chromaticities masteringPrimaries; //!< when description.hasMasteringPrimaries
};
using RecordPtr = std::shared_ptr<const Record>;

ColorManagement::Impl *g_impl = nullptr; //!< the one global (the surfaces' addon owner)

int32_t micro(float v)
{
    return int32_t(std::lround(double(v) * 1'000'000.0));
}

Color::Chromaticities chromaticities(int32_t rx, int32_t ry, int32_t gx, int32_t gy, int32_t bx, int32_t by, int32_t wx, int32_t wy)
{
    return {float(rx) / 1e6f, float(ry) / 1e6f, float(gx) / 1e6f, float(gy) / 1e6f, float(bx) / 1e6f, float(by) / 1e6f, float(wx) / 1e6f, float(wy) / 1e6f};
}

} // namespace

struct ColorManagement::Impl {
    wl_display *display = nullptr;
    wl_global *global = nullptr;
    struct Hook { // plain, for wl_container_of
        wl_listener listener;
        Impl *self;
    } displayDestroy{};
    ColorManagement *owner = nullptr;
    RecordPtr preferred;
    uint64_t nextIdentity = 1; //!< never recycled (the protocol's rule)

    RecordPtr record(const Color::SurfaceColorDescription &description, const Color::Chromaticities &mastering = {})
    {
        auto r = std::make_shared<Record>();
        r->identity = nextIdentity++;
        r->description = description;
        r->description.tagged = true;
        r->masteringPrimaries = mastering;
        return r;
    }
};

namespace
{

// ---- wp_image_description_v1 and its information --------------------------------------------------------------------

struct DescriptionObject {
    RecordPtr record; //!< null: failed (never ready)
    bool allowInfo = false;
};

//! The luminances a description implies: its own (set_luminances) or its transfer function's defaults.
//! What a client asks of colour management, once each by process (Firefox's HDR video once never
//! arrived tagged; this shows how far an app gets).
void note(wl_resource *resource, const QString &what)
{
    static QSet<QString> seen;
    pid_t pid = 0;
    wl_client_get_credentials(wl_resource_get_client(resource), &pid, nullptr, nullptr);
    QFile comm(QStringLiteral("/proc/%1/comm").arg(pid));
    const QString name = comm.open(QIODevice::ReadOnly) ? QString::fromUtf8(comm.readAll()).trimmed() : QStringLiteral("?");
    const QString line = QStringLiteral("%1 (%2): %3").arg(name).arg(pid).arg(what);
    if (!seen.contains(line)) {
        seen.insert(line);
        qInfo().noquote() << "colour management:" << line;
    }
}

void luminancesOf(const Color::SurfaceColorDescription &d, float &minNits, float &maxNits, float &referenceNits)
{
    if (d.hasLuminances) {
        minNits = d.minNits;
        maxNits = d.transferNamed == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ ? d.minNits + 10000.0f : d.maxNits;
        referenceNits = d.referenceNits;
        return;
    }
    switch (d.tfPower > 0.0f ? 0u : d.transferNamed) {
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ:
        minNits = 0.005f, maxNits = 10000.0f, referenceNits = 203.0f;
        break;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG:
        minNits = 0.005f, maxNits = 1000.0f, referenceNits = 203.0f;
        break;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886:
        minNits = 0.01f, maxNits = 100.0f, referenceNits = 100.0f;
        break;
    default:
        minNits = 0.2f, maxNits = 80.0f, referenceNits = 80.0f; // sRGB's
        break;
    }
}

void sendInformation(wl_resource *info, const Record &record)
{
    const Color::SurfaceColorDescription &d = record.description;
    const Color::Chromaticities c = d.hasPrimaries ? d.primaries : Color::namedPrimaries(d.primariesNamed).value_or(Color::bt709());
    wp_image_description_info_v1_send_primaries(info, micro(c.rx), micro(c.ry), micro(c.gx), micro(c.gy), micro(c.bx), micro(c.by), micro(c.wx), micro(c.wy));
    if (!d.hasPrimaries && d.primariesNamed != 0) {
        wp_image_description_info_v1_send_primaries_named(info, d.primariesNamed);
    }
    if (d.tfPower > 0.0f) {
        wp_image_description_info_v1_send_tf_power(info, uint32_t(std::lround(d.tfPower * 10000.0f)));
    } else {
        wp_image_description_info_v1_send_tf_named(info, d.transferNamed);
    }
    float minNits = 0, maxNits = 0, referenceNits = 0;
    luminancesOf(d, minNits, maxNits, referenceNits);
    wp_image_description_info_v1_send_luminances(info, uint32_t(std::lround(minNits * 10000.0f)), uint32_t(std::lround(maxNits)),
                                                 uint32_t(std::lround(referenceNits)));
    const Color::Chromaticities t = d.hasMasteringPrimaries ? record.masteringPrimaries : c;
    wp_image_description_info_v1_send_target_primaries(info, micro(t.rx), micro(t.ry), micro(t.gx), micro(t.gy), micro(t.bx), micro(t.by), micro(t.wx),
                                                        micro(t.wy));
    wp_image_description_info_v1_send_target_luminance(info, uint32_t(std::lround((d.hasMasteringLuminance ? d.masteringMinNits : minNits) * 10000.0f)),
                                                       uint32_t(std::lround(d.hasMasteringLuminance ? d.masteringMaxNits : maxNits)));
    if (d.maxCllNits > 0.0f) {
        wp_image_description_info_v1_send_target_max_cll(info, uint32_t(std::lround(d.maxCllNits)));
    }
    if (d.maxFallNits > 0.0f) {
        wp_image_description_info_v1_send_target_max_fall(info, uint32_t(std::lround(d.maxFallNits)));
    }
}

DescriptionObject *descriptionFrom(wl_resource *resource)
{
    return static_cast<DescriptionObject *>(wl_resource_get_user_data(resource));
}

void descriptionDestroyRequest(wl_client *, wl_resource *resource)
{
    wl_resource_destroy(resource);
}

void descriptionGetInformation(wl_client *client, wl_resource *resource, uint32_t id)
{
    note(resource, QStringLiteral("get a description's information"));
    const DescriptionObject *o = descriptionFrom(resource);
    if (!o->record) {
        wl_resource_post_error(resource, WP_IMAGE_DESCRIPTION_V1_ERROR_NOT_READY, "the image description is not ready");
        return;
    }
    if (!o->allowInfo) {
        wl_resource_post_error(resource, WP_IMAGE_DESCRIPTION_V1_ERROR_NO_INFORMATION, "get_information is not allowed for this image description");
        return;
    }
    wl_resource *info = wl_resource_create(client, &wp_image_description_info_v1_interface, wl_resource_get_version(resource), id);
    if (!info) {
        wl_client_post_no_memory(client);
        return;
    }
    sendInformation(info, *o->record);
    wp_image_description_info_v1_send_done(info);
    wl_resource_destroy(info);
}

const struct wp_image_description_v1_interface kDescriptionImpl = {
    .destroy = descriptionDestroyRequest,
    .get_information = descriptionGetInformation,
};

void descriptionResourceDestroyed(wl_resource *resource)
{
    delete descriptionFrom(resource);
}

//! A new image description object: ready with @p record, or failed (@p record null) for @p cause.
void makeDescription(wl_client *client, int version, uint32_t id, RecordPtr record, bool allowInfo,
                     uint32_t cause = WP_IMAGE_DESCRIPTION_V1_CAUSE_UNSUPPORTED, const char *message = "")
{
    wl_resource *resource = wl_resource_create(client, &wp_image_description_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    auto *o = new DescriptionObject{std::move(record), allowInfo};
    wl_resource_set_implementation(resource, &kDescriptionImpl, o, descriptionResourceDestroyed);
    if (!o->record) {
        wp_image_description_v1_send_failed(resource, cause, message);
        return;
    }
    const uint64_t identity = o->record->identity;
    if (version >= 2) {
        wp_image_description_v1_send_ready2(resource, uint32_t(identity >> 32), uint32_t(identity & 0xffffffffu));
    } else {
        wp_image_description_v1_send_ready(resource, uint32_t(identity));
    }
}

// ---- wp_image_description_creator_params_v1 -------------------------------------------------------------------------

struct Params {
    Color::SurfaceColorDescription d;
    Color::Chromaticities mastering;
    bool tf = false;
    bool primaries = false;
    bool luminances = false;
    bool masteringPrimaries = false;
    bool masteringLuminance = false;
    bool cll = false;
    bool fall = false;
};

Params *paramsFrom(wl_resource *resource)
{
    return static_cast<Params *>(wl_resource_get_user_data(resource));
}

bool alreadySet(wl_resource *resource, bool set, const char *what)
{
    if (set) {
        wl_resource_post_error(resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET, "%s already set", what);
    }
    return set;
}

void paramsCreate(wl_client *client, wl_resource *resource, uint32_t id)
{
    note(resource, QStringLiteral("create a description"));
    Params *p = paramsFrom(resource);
    if (!p->tf || !p->primaries) {
        wl_resource_post_error(resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INCOMPLETE_SET,
                               "a transfer function and primaries are needed");
        return;
    }
    if (p->cll && p->fall && p->d.maxFallNits > p->d.maxCllNits) {
        wl_resource_post_error(resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE, "max_fall is greater than max_cll");
        return;
    }
    // Every complete set of what is offered is shown (planLayer decodes them all).
    makeDescription(client, wl_resource_get_version(resource), id, g_impl ? g_impl->record(p->d, p->mastering) : RecordPtr(), false,
                    WP_IMAGE_DESCRIPTION_V1_CAUSE_OPERATING_SYSTEM, "the compositor is going away");
    wl_resource_destroy(resource);
}

void paramsSetTfNamed(wl_client *, wl_resource *resource, uint32_t tf)
{
    note(resource, QStringLiteral("transfer function %1").arg(tf));
    Params *p = paramsFrom(resource);
    if (alreadySet(resource, p->tf, "transfer function")) {
        return;
    }
    if (!offered(kTransfers, tf)) {
        wl_resource_post_error(resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF, "transfer function %u is not offered", tf);
        return;
    }
    p->tf = true;
    p->d.transferNamed = tf;
}

void paramsSetTfPower(wl_client *, wl_resource *resource, uint32_t eexp)
{
    note(resource, QStringLiteral("power transfer %1").arg(eexp));
    Params *p = paramsFrom(resource);
    if (alreadySet(resource, p->tf, "transfer function")) {
        return;
    }
    if (eexp < 10000 || eexp > 100000) {
        wl_resource_post_error(resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF, "the exponent must be 1.0 to 10.0");
        return;
    }
    p->tf = true;
    p->d.tfPower = float(eexp) / 10000.0f;
}

void paramsSetPrimariesNamed(wl_client *, wl_resource *resource, uint32_t primaries)
{
    note(resource, QStringLiteral("primaries %1").arg(primaries));
    Params *p = paramsFrom(resource);
    if (alreadySet(resource, p->primaries, "primaries")) {
        return;
    }
    if (!offered(kPrimaries, primaries)) {
        wl_resource_post_error(resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_PRIMARIES_NAMED, "primaries %u are not offered", primaries);
        return;
    }
    p->primaries = true;
    p->d.primariesNamed = primaries;
}

void paramsSetPrimaries(wl_client *, wl_resource *resource, int32_t rx, int32_t ry, int32_t gx, int32_t gy, int32_t bx, int32_t by, int32_t wx,
                        int32_t wy)
{
    Params *p = paramsFrom(resource);
    if (alreadySet(resource, p->primaries, "primaries")) {
        return;
    }
    p->primaries = true;
    p->d.hasPrimaries = true;
    p->d.primaries = chromaticities(rx, ry, gx, gy, bx, by, wx, wy);
}

void paramsSetLuminances(wl_client *, wl_resource *resource, uint32_t minLum, uint32_t maxLum, uint32_t referenceLum)
{
    Params *p = paramsFrom(resource);
    if (alreadySet(resource, p->luminances, "luminances")) {
        return;
    }
    const float minNits = float(minLum) / 10000.0f;
    if (float(maxLum) <= minNits || float(referenceLum) <= minNits) {
        wl_resource_post_error(resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE,
                               "max_lum and reference_lum must be greater than min_lum");
        return;
    }
    p->luminances = true;
    p->d.hasLuminances = true;
    p->d.minNits = minNits;
    p->d.maxNits = float(maxLum);
    p->d.referenceNits = float(referenceLum);
}

void paramsSetMasteringPrimaries(wl_client *, wl_resource *resource, int32_t rx, int32_t ry, int32_t gx, int32_t gy, int32_t bx, int32_t by,
                                 int32_t wx, int32_t wy)
{
    Params *p = paramsFrom(resource);
    if (alreadySet(resource, p->masteringPrimaries, "mastering display primaries")) {
        return;
    }
    p->masteringPrimaries = true;
    p->d.hasMasteringPrimaries = true;
    p->mastering = chromaticities(rx, ry, gx, gy, bx, by, wx, wy);
}

void paramsSetMasteringLuminance(wl_client *, wl_resource *resource, uint32_t minLum, uint32_t maxLum)
{
    note(resource, QStringLiteral("mastering luminance %1-%2").arg(minLum).arg(maxLum));
    Params *p = paramsFrom(resource);
    if (alreadySet(resource, p->masteringLuminance, "mastering luminance")) {
        return;
    }
    const float minNits = float(minLum) / 10000.0f;
    if (float(maxLum) <= minNits) {
        wl_resource_post_error(resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE, "max L must be greater than min L");
        return;
    }
    p->masteringLuminance = true;
    p->d.hasMasteringLuminance = true;
    p->d.masteringMinNits = minNits;
    p->d.masteringMaxNits = float(maxLum);
}

void paramsSetMaxCll(wl_client *, wl_resource *resource, uint32_t maxCll)
{
    note(resource, QStringLiteral("MaxCLL %1").arg(maxCll));
    Params *p = paramsFrom(resource);
    if (alreadySet(resource, p->cll, "max_cll")) {
        return;
    }
    p->cll = true;
    p->d.maxCllNits = float(maxCll);
}

void paramsSetMaxFall(wl_client *, wl_resource *resource, uint32_t maxFall)
{
    Params *p = paramsFrom(resource);
    if (alreadySet(resource, p->fall, "max_fall")) {
        return;
    }
    p->fall = true;
    p->d.maxFallNits = float(maxFall);
}

const struct wp_image_description_creator_params_v1_interface kParamsImpl = {
    .create = paramsCreate,
    .set_tf_named = paramsSetTfNamed,
    .set_tf_power = paramsSetTfPower,
    .set_primaries_named = paramsSetPrimariesNamed,
    .set_primaries = paramsSetPrimaries,
    .set_luminances = paramsSetLuminances,
    .set_mastering_display_primaries = paramsSetMasteringPrimaries,
    .set_mastering_luminance = paramsSetMasteringLuminance,
    .set_max_cll = paramsSetMaxCll,
    .set_max_fall = paramsSetMaxFall,
};

void paramsResourceDestroyed(wl_resource *resource)
{
    delete paramsFrom(resource);
}

// ---- wp_color_management_surface_v1 (one a wl_surface; its description double-buffered) ----------------------------

struct SurfaceState {
    RecordPtr record; //!< null: no description (untagged)
    uint32_t intent = 0;
};

const wlr_surface_synced_impl kSyncedImpl = {
    .state_size = sizeof(SurfaceState),
    .init_state = [](void *state) { new (state) SurfaceState(); },
    .finish_state = [](void *state) { static_cast<SurfaceState *>(state)->~SurfaceState(); },
    // Copied, not moved: what was set stays pending until changed (as wlroots' own manager does with memcpy).
    .move_state = [](void *dst, void *src) { *static_cast<SurfaceState *>(dst) = *static_cast<SurfaceState *>(src); },
    .commit = nullptr,
};

struct ColorSurface {
    wl_resource *resource = nullptr;
    wlr_surface *surface = nullptr;
    wlr_addon addon{};
    wlr_surface_synced synced{};
    // Raw storage: wlr_surface_synced_init clears it and constructs the states in place (kSyncedImpl.init_state).
    alignas(SurfaceState) unsigned char pendingStorage[sizeof(SurfaceState)];
    alignas(SurfaceState) unsigned char currentStorage[sizeof(SurfaceState)];

    SurfaceState &pending()
    {
        return *std::launder(reinterpret_cast<SurfaceState *>(pendingStorage));
    }
    SurfaceState &current()
    {
        return *std::launder(reinterpret_cast<SurfaceState *>(currentStorage));
    }
};

void destroyColorSurface(ColorSurface *cs)
{
    wlr_surface_synced_finish(&cs->synced);
    wlr_addon_finish(&cs->addon);
    if (cs->resource) {
        wl_resource_set_user_data(cs->resource, nullptr); // inert
    }
    delete cs;
}

const wlr_addon_interface kAddonImpl = {
    .name = "rift_home_color_management_surface",
    .destroy = [](wlr_addon *addon) {
        ColorSurface *cs = wl_container_of(addon, cs, addon);
        destroyColorSurface(cs);
    },
};

void surfaceDestroyRequest(wl_client *, wl_resource *resource)
{
    wl_resource_destroy(resource);
}

void surfaceSetDescription(wl_client *, wl_resource *resource, wl_resource *descriptionResource, uint32_t intent)
{
    note(resource, QStringLiteral("set a surface's description (intent %1)").arg(intent));
    auto *cs = static_cast<ColorSurface *>(wl_resource_get_user_data(resource));
    if (!cs) {
        wl_resource_post_error(resource, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_INERT, "the surface is gone");
        return;
    }
    const DescriptionObject *o = descriptionFrom(descriptionResource);
    if (!o->record) {
        wl_resource_post_error(resource, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_IMAGE_DESCRIPTION, "the image description is not ready");
        return;
    }
    if (intent != WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL) {
        wl_resource_post_error(resource, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_RENDER_INTENT, "rendering intent %u is not offered", intent);
        return;
    }
    cs->pending().record = o->record;
    cs->pending().intent = intent;
}

void surfaceUnsetDescription(wl_client *, wl_resource *resource)
{
    note(resource, QStringLiteral("unset a surface's description"));
    auto *cs = static_cast<ColorSurface *>(wl_resource_get_user_data(resource));
    if (!cs) {
        wl_resource_post_error(resource, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_INERT, "the surface is gone");
        return;
    }
    cs->pending().record.reset();
}

const struct wp_color_management_surface_v1_interface kSurfaceImpl = {
    .destroy = surfaceDestroyRequest,
    .set_image_description = surfaceSetDescription,
    .unset_image_description = surfaceUnsetDescription,
};

void surfaceResourceDestroyed(wl_resource *resource)
{
    // As unset_image_description, and the surface may get another color management surface.
    if (auto *cs = static_cast<ColorSurface *>(wl_resource_get_user_data(resource))) {
        cs->resource = nullptr;
        destroyColorSurface(cs);
    }
}

// ---- wp_color_management_surface_feedback_v1 ------------------------------------------------------------------------

struct Feedback {
    wlr_surface *surface = nullptr; //!< null: inert
    wl_listener surfaceDestroy{};
};

void feedbackGetPreferred(wl_client *client, wl_resource *resource, uint32_t id)
{
    note(resource, QStringLiteral("get the preferred description"));
    auto *fb = static_cast<Feedback *>(wl_resource_get_user_data(resource));
    if (!fb->surface) {
        wl_resource_post_error(resource, WP_COLOR_MANAGEMENT_SURFACE_FEEDBACK_V1_ERROR_INERT, "the surface is gone");
        return;
    }
    makeDescription(client, wl_resource_get_version(resource), id, g_impl ? g_impl->preferred : RecordPtr(), true,
                    WP_IMAGE_DESCRIPTION_V1_CAUSE_OPERATING_SYSTEM, "the compositor is going away");
}

const struct wp_color_management_surface_feedback_v1_interface kFeedbackImpl = {
    .destroy = [](wl_client *, wl_resource *resource) { wl_resource_destroy(resource); },
    .get_preferred = feedbackGetPreferred,
    .get_preferred_parametric = feedbackGetPreferred, // the preferred one is parametric
};

void feedbackResourceDestroyed(wl_resource *resource)
{
    auto *fb = static_cast<Feedback *>(wl_resource_get_user_data(resource));
    if (fb->surface) {
        wl_list_remove(&fb->surfaceDestroy.link);
    }
    delete fb;
}

// ---- wp_color_management_output_v1 ----------------------------------------------------------------------------------

struct ColorOutput {
    wlr_output *output = nullptr; //!< null: inert (the output went away)
    wl_listener outputDestroy{};
};

void outputGetDescription(wl_client *client, wl_resource *resource, uint32_t id)
{
    note(resource, QStringLiteral("get the output's description"));
    auto *co = static_cast<ColorOutput *>(wl_resource_get_user_data(resource));
    if (!co->output) {
        makeDescription(client, wl_resource_get_version(resource), id, nullptr, true, WP_IMAGE_DESCRIPTION_V1_CAUSE_NO_OUTPUT, "the output is gone");
        return;
    }
    // What Rift Home's outputs take: its preferred encoding (the headset is the real display).
    makeDescription(client, wl_resource_get_version(resource), id, g_impl ? g_impl->preferred : RecordPtr(), true,
                    WP_IMAGE_DESCRIPTION_V1_CAUSE_OPERATING_SYSTEM, "the compositor is going away");
}

const struct wp_color_management_output_v1_interface kOutputImpl = {
    .destroy = [](wl_client *, wl_resource *resource) { wl_resource_destroy(resource); },
    .get_image_description = outputGetDescription,
};

void outputResourceDestroyed(wl_resource *resource)
{
    auto *co = static_cast<ColorOutput *>(wl_resource_get_user_data(resource));
    if (co->output) {
        wl_list_remove(&co->outputDestroy.link);
    }
    delete co;
}

// ---- wp_color_manager_v1 --------------------------------------------------------------------------------------------

void managerGetOutput(wl_client *client, wl_resource *manager, uint32_t id, wl_resource *outputResource)
{
    note(manager, QStringLiteral("get an output"));
    wl_resource *resource = wl_resource_create(client, &wp_color_management_output_v1_interface, wl_resource_get_version(manager), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    auto *co = new ColorOutput;
    co->output = wlr_output_from_resource(outputResource);
    if (co->output) {
        co->outputDestroy.notify = [](wl_listener *listener, void *) {
            ColorOutput *o = wl_container_of(listener, o, outputDestroy);
            wl_list_remove(&o->outputDestroy.link);
            o->output = nullptr;
        };
        wl_signal_add(&co->output->events.destroy, &co->outputDestroy);
    }
    wl_resource_set_implementation(resource, &kOutputImpl, co, outputResourceDestroyed);
}

void managerGetSurface(wl_client *client, wl_resource *manager, uint32_t id, wl_resource *surfaceResource)
{
    note(manager, QStringLiteral("get a surface"));
    wlr_surface *surface = wlr_surface_from_resource(surfaceResource);
    if (wlr_addon_find(&surface->addons, g_impl, &kAddonImpl)) {
        wl_resource_post_error(manager, WP_COLOR_MANAGER_V1_ERROR_SURFACE_EXISTS, "the surface has a color management surface already");
        return;
    }
    auto *cs = new ColorSurface;
    cs->surface = surface;
    if (!wlr_surface_synced_init(&cs->synced, surface, &kSyncedImpl, cs->pendingStorage, cs->currentStorage)) {
        delete cs;
        wl_client_post_no_memory(client);
        return;
    }
    cs->resource = wl_resource_create(client, &wp_color_management_surface_v1_interface, wl_resource_get_version(manager), id);
    if (!cs->resource) {
        wlr_surface_synced_finish(&cs->synced);
        delete cs;
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(cs->resource, &kSurfaceImpl, cs, surfaceResourceDestroyed);
    wlr_addon_init(&cs->addon, &surface->addons, g_impl, &kAddonImpl);
}

void managerGetFeedback(wl_client *client, wl_resource *manager, uint32_t id, wl_resource *surfaceResource)
{
    note(manager, QStringLiteral("get a surface's feedback"));
    wl_resource *resource = wl_resource_create(client, &wp_color_management_surface_feedback_v1_interface, wl_resource_get_version(manager), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    auto *fb = new Feedback;
    fb->surface = wlr_surface_from_resource(surfaceResource);
    fb->surfaceDestroy.notify = [](wl_listener *listener, void *) {
        Feedback *f = wl_container_of(listener, f, surfaceDestroy);
        wl_list_remove(&f->surfaceDestroy.link);
        f->surface = nullptr;
    };
    wl_signal_add(&fb->surface->events.destroy, &fb->surfaceDestroy);
    wl_resource_set_implementation(resource, &kFeedbackImpl, fb, feedbackResourceDestroyed);
    // The preferred description never changes while Rift Home runs: no preferred_changed2 to send.
}

void managerCreateParametric(wl_client *client, wl_resource *manager, uint32_t id)
{
    note(manager, QStringLiteral("create a parametric creator"));
    wl_resource *resource = wl_resource_create(client, &wp_image_description_creator_params_v1_interface, wl_resource_get_version(manager), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &kParamsImpl, new Params, paramsResourceDestroyed);
}

void managerUnsupported(wl_resource *manager, const char *what)
{
    note(manager, QStringLiteral("REFUSED: %1").arg(QLatin1String(what)));
    wl_resource_post_error(manager, WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE, "%s is not offered", what);
}

const struct wp_color_manager_v1_interface kManagerImpl = {
    .destroy = [](wl_client *, wl_resource *resource) { wl_resource_destroy(resource); },
    .get_output = managerGetOutput,
    .get_surface = managerGetSurface,
    .get_surface_feedback = managerGetFeedback,
    .create_icc_creator = [](wl_client *, wl_resource *manager, uint32_t) { managerUnsupported(manager, "ICC"); },
    .create_parametric_creator = managerCreateParametric,
    .create_windows_scrgb = [](wl_client *, wl_resource *manager, uint32_t) { managerUnsupported(manager, "Windows-scRGB"); },
    .get_image_description =
        [](wl_client *client, wl_resource *manager, uint32_t id, wl_resource *) {
            // Image description references come from other protocols, none of which Rift Home offers.
            makeDescription(client, wl_resource_get_version(manager), id, nullptr, false, WP_IMAGE_DESCRIPTION_V1_CAUSE_UNSUPPORTED,
                            "no image description references here");
        },
    .create_windows_bt2100 = [](wl_client *, wl_resource *manager, uint32_t) { managerUnsupported(manager, "Windows-BT.2100"); },
};

void managerBind(wl_client *client, void *, uint32_t version, uint32_t id)
{
    wl_resource *resource = wl_resource_create(client, &wp_color_manager_v1_interface, int(version), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &kManagerImpl, nullptr, nullptr);
    note(resource, QStringLiteral("bound the manager, version %1").arg(version));
    wp_color_manager_v1_send_supported_intent(resource, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
    for (uint32_t feature : kFeatures) {
        wp_color_manager_v1_send_supported_feature(resource, feature);
    }
    for (uint32_t tf : kTransfers) {
        wp_color_manager_v1_send_supported_tf_named(resource, tf);
    }
    for (uint32_t primaries : kPrimaries) {
        wp_color_manager_v1_send_supported_primaries_named(resource, primaries);
    }
    wp_color_manager_v1_send_done(resource);
}

} // namespace

ColorManagement::ColorManagement(std::unique_ptr<Impl> impl)
    : d(std::move(impl))
{
}

ColorManagement *ColorManagement::create(wl_display *display, Preferred preferred)
{
    auto impl = std::make_unique<Impl>();
    impl->display = display;
    Color::SurfaceColorDescription p;
    p.transferNamed = preferred.transfer;
    p.primariesNamed = preferred.primaries;
    impl->preferred = impl->record(p);
    impl->global = wl_global_create(display, &wp_color_manager_v1_interface, kVersion, impl.get(), managerBind);
    if (!impl->global) {
        return nullptr;
    }
    Impl *raw = impl.get();
    auto *manager = new ColorManagement(std::move(impl));
    raw->owner = manager;
    raw->displayDestroy.self = raw;
    raw->displayDestroy.listener.notify = [](wl_listener *listener, void *) {
        Impl::Hook *hook = wl_container_of(listener, hook, listener);
        Impl *self = hook->self;
        wl_list_remove(&self->displayDestroy.listener.link);
        wl_global_destroy(self->global);
        if (g_impl == self) {
            g_impl = nullptr;
        }
        delete self->owner; // and this Impl with it
    };
    wl_display_add_destroy_listener(display, &raw->displayDestroy.listener);
    g_impl = raw;
    return manager;
}

Color::SurfaceColorDescription ColorManagement::descriptionOf(wlr_surface *surface)
{
    if (!g_impl || !surface) {
        return {};
    }
    wlr_addon *addon = wlr_addon_find(&surface->addons, g_impl, &kAddonImpl);
    if (!addon) {
        return {};
    }
    ColorSurface *cs = wl_container_of(addon, cs, addon);
    const RecordPtr &record = cs->current().record;
    return record ? record->description : Color::SurfaceColorDescription{};
}

QStringList ColorManagement::offeredTransfers()
{
    QStringList names;
    for (uint32_t tf : kTransfers) {
        names << Color::transferName(tf);
    }
    return names;
}

QStringList ColorManagement::offeredPrimaries()
{
    QStringList names;
    for (uint32_t p : kPrimaries) {
        names << Color::primariesName(p);
    }
    return names;
}

} // namespace RiftHome::Wayland
