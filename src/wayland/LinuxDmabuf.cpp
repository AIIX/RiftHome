// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "wayland/LinuxDmabuf.hpp"

#include "wayland/Logging.hpp"
#include "wayland/wlr.hpp"

#include "linux-dmabuf-v1-server-protocol.h"

#include <QString>

#include <array>
#include <fcntl.h>
#include <set>
#include <sys/mman.h>
#include <sys/stat.h>

namespace RiftHome::Wayland
{

namespace
{

constexpr int kVersion = 4;
constexpr int kMaxPlanes = 4;

//! The formats offered, in the format table's order (format, modifier).
struct Global {
    wl_global *global = nullptr;
    wlr_renderer *renderer = nullptr;
    std::vector<std::pair<uint32_t, uint64_t>> formats;
    int tableFd = -1;
    size_t tableSize = 0;
    dev_t device = 0;
    wl_listener displayDestroy{};
};
Global *g_global = nullptr;

// Buffers made here.

struct DmabufBuffer {
    wlr_buffer base;
    wlr_dmabuf_attributes attributes{};
    std::array<uint64_t, kMaxPlanes> modifiers{};
    wl_resource *resource = nullptr;
    wl_listener release{};
    int releases = 0; //!< wl_buffer.release events sent
};

std::set<DmabufBuffer *> g_live; //!< made and not yet destroyed (Wayland thread)

void bufferDestroy(wlr_buffer *base)
{
    auto *buffer = wl_container_of(base, static_cast<DmabufBuffer *>(nullptr), base);
    if (buffer->resource) {
        wl_resource_set_user_data(buffer->resource, nullptr);
    }
    wl_list_remove(&buffer->release.link);
    wl_list_init(&buffer->release.link);
    // Its destroy event and its addons (wlroots' textures and scene nodes of it, Rift Home's buffer ids), as wlroots
    // requires of every buffer: without it they kept pointers into the freed buffer (heap corruption,
    // crashes at exit).
    wlr_buffer_finish(&buffer->base);
    wlr_dmabuf_attributes_finish(&buffer->attributes);
    g_live.erase(buffer);
    delete buffer;
}

bool bufferGetDmabuf(wlr_buffer *base, wlr_dmabuf_attributes *attributes)
{
    auto *buffer = wl_container_of(base, static_cast<DmabufBuffer *>(nullptr), base);
    if (buffer->attributes.format == DRM_FORMAT_P010) {
        return false; // for wlroots: no GL texture of HDR video (LinuxDmabuf::dmabufOf gives Rift Home its attributes)
    }
    *attributes = buffer->attributes;
    return true;
}

const wlr_buffer_impl kBufferImpl = {
    .destroy = bufferDestroy,
    .get_dmabuf = bufferGetDmabuf,
    .get_shm = nullptr,
    .begin_data_ptr_access = nullptr,
    .end_data_ptr_access = nullptr,
};

const struct wl_buffer_interface kWlBufferImpl = {
    .destroy = [](wl_client *, wl_resource *resource) { wl_resource_destroy(resource); },
};

DmabufBuffer *bufferFromResource(wl_resource *resource)
{
    return static_cast<DmabufBuffer *>(wl_resource_get_user_data(resource));
}

void wlBufferResourceDestroyed(wl_resource *resource)
{
    if (DmabufBuffer *buffer = bufferFromResource(resource)) {
        buffer->resource = nullptr;
        wlr_buffer_drop(&buffer->base);
    }
}

const wlr_buffer_resource_interface kResourceInterface = {
    .name = "rift-home linux-dmabuf",
    .is_instance = [](wl_resource *resource) { return wl_resource_instance_of(resource, &wl_buffer_interface, &kWlBufferImpl) != 0; },
    .from_resource = [](wl_resource *resource) -> wlr_buffer * {
        DmabufBuffer *buffer = bufferFromResource(resource);
        return buffer ? &buffer->base : nullptr;
    },
};

// Parameters being put together.

struct Params {
    std::array<int, kMaxPlanes> fds{-1, -1, -1, -1};
    std::array<uint32_t, kMaxPlanes> offsets{};
    std::array<uint32_t, kMaxPlanes> strides{};
    std::array<uint64_t, kMaxPlanes> modifiers{};
    bool used = false;
};

Params *paramsFrom(wl_resource *resource)
{
    return static_cast<Params *>(wl_resource_get_user_data(resource));
}

void paramsResourceDestroyed(wl_resource *resource)
{
    Params *params = paramsFrom(resource);
    for (int fd : params->fds) {
        if (fd >= 0) {
            ::close(fd);
        }
    }
    delete params;
}

void paramsAdd(wl_client *, wl_resource *resource, int32_t fd, uint32_t plane, uint32_t offset, uint32_t stride, uint32_t modifierHi,
               uint32_t modifierLo)
{
    Params *params = paramsFrom(resource);
    if (params->used) {
        ::close(fd);
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED, "the parameters were already used");
        return;
    }
    if (plane >= uint32_t(kMaxPlanes)) {
        ::close(fd);
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX, "plane %u is past the last (%d)", plane, kMaxPlanes - 1);
        return;
    }
    if (params->fds[plane] >= 0) {
        ::close(fd);
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET, "plane %u was already set", plane);
        return;
    }
    // Each plane's own modifier (wlroots insisted on one for all: NVIDIA's VA-API P010 planes differ).
    params->fds[plane] = fd;
    params->offsets[plane] = offset;
    params->strides[plane] = stride;
    params->modifiers[plane] = (uint64_t(modifierHi) << 32) | modifierLo;
}

//! The buffer for @p params, or null with why in @p why (not a protocol error: the client is told it failed).
DmabufBuffer *makeBuffer(Params *params, int32_t width, int32_t height, uint32_t format, QString &why)
{
    int planes = 0;
    while (planes < kMaxPlanes && params->fds[planes] >= 0) {
        ++planes;
    }
    if (planes == 0) {
        why = QStringLiteral("no planes");
        return nullptr;
    }
    for (int i = planes; i < kMaxPlanes; ++i) {
        if (params->fds[i] >= 0) {
            why = QStringLiteral("a gap in the planes");
            return nullptr;
        }
    }
    if (width <= 0 || height <= 0) {
        why = QStringLiteral("no size");
        return nullptr;
    }
    auto *buffer = new DmabufBuffer;
    buffer->attributes.width = width;
    buffer->attributes.height = height;
    buffer->attributes.format = format;
    buffer->attributes.modifier = params->modifiers[0];
    buffer->attributes.n_planes = planes;
    bool sameModifiers = true;
    for (int i = 0; i < planes; ++i) {
        buffer->attributes.fd[i] = params->fds[i];
        buffer->attributes.offset[i] = params->offsets[i];
        buffer->attributes.stride[i] = params->strides[i];
        buffer->modifiers[size_t(i)] = params->modifiers[size_t(i)];
        sameModifiers = sameModifiers && params->modifiers[size_t(i)] == params->modifiers[0];
        params->fds[size_t(i)] = -1; // the buffer has them now
    }
    if (!sameModifiers) {
        static int noted = 0;
        if (noted++ < 3) {
            qCInfo(lcWayland).noquote() << QStringLiteral("linux-dmabuf: a %1x%2 buffer with a modifier for each plane: %3 / %4 (taken)")
                                               .arg(width)
                                               .arg(height)
                                               .arg(params->modifiers[0], 0, 16)
                                               .arg(params->modifiers[1], 0, 16);
        }
    }
    // As wlroots checks a buffer: imported into the renderer once. P010 is the XR side's (Vulkan), whatever its modifiers.
    if (format != DRM_FORMAT_P010) {
        wlr_texture *texture = sameModifiers && g_global ? wlr_texture_from_dmabuf(g_global->renderer, &buffer->attributes) : nullptr;
        if (!texture) {
            why = sameModifiers ? QStringLiteral("the renderer cannot import it") : QStringLiteral("planes with different modifiers");
            wlr_dmabuf_attributes_finish(&buffer->attributes);
            delete buffer;
            return nullptr;
        }
        wlr_texture_destroy(texture);
    }
    wlr_buffer_init(&buffer->base, &kBufferImpl, width, height);
    g_live.insert(buffer);
    buffer->release.notify = [](wl_listener *listener, void *) {
        auto *b = wl_container_of(listener, static_cast<DmabufBuffer *>(nullptr), release);
        ++b->releases;
        if (b->resource) {
            wl_buffer_send_release(b->resource);
        }
    };
    wl_signal_add(&buffer->base.events.release, &buffer->release);
    return buffer;
}

void createBuffer(wl_client *client, wl_resource *paramsResource, uint32_t id, int32_t width, int32_t height, uint32_t format,
                  uint32_t flags, bool immediate)
{
    Params *params = paramsFrom(paramsResource);
    if (params->used) {
        wl_resource_post_error(paramsResource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED, "the parameters were already used");
        return;
    }
    params->used = true;
    QString why;
    DmabufBuffer *buffer = flags == 0 ? makeBuffer(params, width, height, format, why) : nullptr;
    if (flags != 0) {
        why = QStringLiteral("flags %1 are not supported").arg(flags);
    }
    if (!buffer) {
        static int warned = 0;
        if (warned++ < 8) {
            qCWarning(lcWayland).noquote() << QStringLiteral("linux-dmabuf: a %1x%2 buffer refused: %3").arg(width).arg(height).arg(why);
        }
        if (immediate) {
            wl_resource_post_error(paramsResource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER, "%s", qPrintable(why));
        } else {
            zwp_linux_buffer_params_v1_send_failed(paramsResource);
        }
        return;
    }
    wl_resource *resource = wl_resource_create(client, &wl_buffer_interface, 1, immediate ? id : 0);
    if (!resource) {
        wlr_buffer_drop(&buffer->base);
        wl_client_post_no_memory(client);
        return;
    }
    buffer->resource = resource;
    wl_resource_set_implementation(resource, &kWlBufferImpl, buffer, wlBufferResourceDestroyed);
    if (!immediate) {
        zwp_linux_buffer_params_v1_send_created(paramsResource, resource);
    }
}

const struct zwp_linux_buffer_params_v1_interface kParamsImpl = {
    .destroy = [](wl_client *, wl_resource *resource) { wl_resource_destroy(resource); },
    .add = paramsAdd,
    .create = [](wl_client *client, wl_resource *resource, int32_t width, int32_t height, uint32_t format, uint32_t flags) {
        createBuffer(client, resource, 0, width, height, format, flags, false);
    },
    .create_immed = [](wl_client *client, wl_resource *resource, uint32_t id, int32_t width, int32_t height, uint32_t format,
                       uint32_t flags) { createBuffer(client, resource, id, width, height, format, flags, true); },
    .set_sampling_device = nullptr, // version 6; version 4 is offered
};

// Feedback: one tranche of every format, on the renderer's device.

void sendFeedback(wl_resource *feedback)
{
    if (!g_global) {
        return;
    }
    zwp_linux_dmabuf_feedback_v1_send_format_table(feedback, g_global->tableFd, uint32_t(g_global->tableSize));
    wl_array device;
    wl_array_init(&device);
    if (auto *d = static_cast<dev_t *>(wl_array_add(&device, sizeof(dev_t)))) {
        *d = g_global->device;
    }
    zwp_linux_dmabuf_feedback_v1_send_main_device(feedback, &device);
    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(feedback, &device);
    wl_array_release(&device);
    wl_array indices;
    wl_array_init(&indices);
    for (size_t i = 0; i < g_global->formats.size(); ++i) {
        if (auto *index = static_cast<uint16_t *>(wl_array_add(&indices, sizeof(uint16_t)))) {
            *index = uint16_t(i);
        }
    }
    zwp_linux_dmabuf_feedback_v1_send_tranche_formats(feedback, &indices);
    wl_array_release(&indices);
    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(feedback, 0);
    zwp_linux_dmabuf_feedback_v1_send_tranche_done(feedback);
    zwp_linux_dmabuf_feedback_v1_send_done(feedback);
}

const struct zwp_linux_dmabuf_feedback_v1_interface kFeedbackImpl = {
    .destroy = [](wl_client *, wl_resource *resource) { wl_resource_destroy(resource); },
};

void makeFeedback(wl_client *client, wl_resource *dmabuf, uint32_t id)
{
    wl_resource *feedback = wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface, wl_resource_get_version(dmabuf), id);
    if (!feedback) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(feedback, &kFeedbackImpl, nullptr, nullptr);
    sendFeedback(feedback);
}

const struct zwp_linux_dmabuf_v1_interface kDmabufImpl = {
    .destroy = [](wl_client *, wl_resource *resource) { wl_resource_destroy(resource); },
    .create_params =
        [](wl_client *client, wl_resource *dmabuf, uint32_t id) {
            wl_resource *resource = wl_resource_create(client, &zwp_linux_buffer_params_v1_interface, wl_resource_get_version(dmabuf), id);
            if (!resource) {
                wl_client_post_no_memory(client);
                return;
            }
            wl_resource_set_implementation(resource, &kParamsImpl, new Params, paramsResourceDestroyed);
        },
    .get_default_feedback = [](wl_client *client, wl_resource *dmabuf, uint32_t id) { makeFeedback(client, dmabuf, id); },
    .get_surface_feedback = [](wl_client *client, wl_resource *dmabuf, uint32_t id, wl_resource *) { makeFeedback(client, dmabuf, id); },
};

void bind(wl_client *client, void *, uint32_t version, uint32_t id)
{
    wl_resource *resource = wl_resource_create(client, &zwp_linux_dmabuf_v1_interface, int(version), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &kDmabufImpl, nullptr, nullptr);
    if (version < 4 && g_global) {
        // Older clients learn the formats from events (version 3: with modifiers).
        for (const auto &[format, modifier] : g_global->formats) {
            if (version >= 3) {
                zwp_linux_dmabuf_v1_send_modifier(resource, format, uint32_t(modifier >> 32), uint32_t(modifier & 0xffffffffu));
            } else if (modifier == DRM_FORMAT_MOD_INVALID || modifier == DRM_FORMAT_MOD_LINEAR) {
                zwp_linux_dmabuf_v1_send_format(resource, format);
            }
        }
    }
}

} // namespace

bool LinuxDmabuf::create(wl_display *display, wlr_renderer *renderer)
{
    const wlr_drm_format_set *texture = wlr_renderer_get_texture_formats(renderer, WLR_BUFFER_CAP_DMABUF);
    const int drmFd = wlr_renderer_get_drm_fd(renderer);
    struct stat st{};
    if (!texture || drmFd < 0 || fstat(drmFd, &st) != 0) {
        return false;
    }
    auto *g = new Global;
    g->renderer = renderer;
    g->device = st.st_rdev;
    bool nv12 = false;
    for (size_t i = 0; i < texture->len; ++i) {
        const wlr_drm_format &f = texture->formats[i];
        nv12 = nv12 || f.format == DRM_FORMAT_NV12;
        for (size_t m = 0; m < f.len; ++m) {
            g->formats.emplace_back(f.format, f.modifiers[m]);
        }
    }
    if (nv12) {
        g->formats.emplace_back(DRM_FORMAT_P010, DRM_FORMAT_MOD_LINEAR); // linear: software decoders map it
    }
    // The format table: 16 bytes an entry (format, padding, modifier), in a sealed memfd shared with every client.
    g->tableSize = g->formats.size() * 16;
    g->tableFd = memfd_create("rift-home-dmabuf-formats", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (g->tableFd < 0 || ftruncate(g->tableFd, off_t(g->tableSize)) != 0) {
        delete g;
        return false;
    }
    if (void *map = mmap(nullptr, g->tableSize, PROT_WRITE, MAP_SHARED, g->tableFd, 0); map != MAP_FAILED) {
        auto *bytes = static_cast<uint8_t *>(map);
        for (size_t i = 0; i < g->formats.size(); ++i) {
            const uint32_t format = g->formats[i].first;
            const uint32_t pad = 0;
            const uint64_t modifier = g->formats[i].second;
            memcpy(bytes + i * 16, &format, 4);
            memcpy(bytes + i * 16 + 4, &pad, 4);
            memcpy(bytes + i * 16 + 8, &modifier, 8);
        }
        munmap(map, g->tableSize);
    }
    fcntl(g->tableFd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
    g->global = wl_global_create(display, &zwp_linux_dmabuf_v1_interface, kVersion, nullptr, bind);
    if (!g->global) {
        ::close(g->tableFd);
        delete g;
        return false;
    }
    g->displayDestroy.notify = [](wl_listener *listener, void *) {
        auto *self = wl_container_of(listener, static_cast<Global *>(nullptr), displayDestroy);
        wl_list_remove(&self->displayDestroy.link);
        wl_global_destroy(self->global);
        ::close(self->tableFd);
        if (g_global == self) {
            g_global = nullptr;
        }
        delete self;
    };
    wl_display_add_destroy_listener(display, &g->displayDestroy);
    g_global = g;
    static bool registered = false;
    if (!registered) {
        wlr_buffer_register_resource_interface(&kResourceInterface);
        registered = true;
    }
    qCInfo(lcWayland).noquote() << QStringLiteral("linux-dmabuf (Rift Home's own, version %1): %2 format/modifier pairs%3, each plane its own modifier")
                                       .arg(kVersion)
                                       .arg(g->formats.size())
                                       .arg(nv12 ? QStringLiteral(" and P010 (HDR video)") : QString());
    return true;
}

std::vector<uint64_t> LinuxDmabuf::planeModifiers(wlr_buffer *buffer)
{
    if (!buffer || buffer->impl != &kBufferImpl) {
        return {};
    }
    auto *b = wl_container_of(buffer, static_cast<DmabufBuffer *>(nullptr), base);
    return std::vector<uint64_t>(b->modifiers.begin(), b->modifiers.begin() + b->attributes.n_planes);
}

bool LinuxDmabuf::dmabufOf(wlr_buffer *buffer, wlr_dmabuf_attributes *attributes)
{
    if (buffer && buffer->impl == &kBufferImpl) {
        const DmabufBuffer *own = wl_container_of(buffer, static_cast<DmabufBuffer *>(nullptr), base);
        *attributes = own->attributes;
        return true;
    }
    return buffer && wlr_buffer_get_dmabuf(buffer, attributes);
}

QString LinuxDmabuf::heldSummary()
{
    // Alive until the client destroys its wl_buffer and no one here holds it (locks).
    int p010 = 0, dropped = 0, kept = 0, keptLocked = 0, keptNeverReleased = 0;
    for (const DmabufBuffer *b : g_live) {
        p010 += b->attributes.format == DRM_FORMAT_P010;
        if (b->base.dropped) {
            ++dropped; // destroyed by the client, still locked here
        } else if (b->base.n_locks > 0) {
            ++keptLocked;
        } else {
            ++kept;
            keptNeverReleased += b->releases == 0;
        }
    }
    return QStringLiteral("%1 dmabufs (%2 P010; %3 destroyed by the client but locked here, %4 locked, %5 idle with the client, %6 of them never released)")
        .arg(g_live.size())
        .arg(p010)
        .arg(dropped)
        .arg(keptLocked)
        .arg(kept)
        .arg(keptNeverReleased);
}

} // namespace RiftHome::Wayland
