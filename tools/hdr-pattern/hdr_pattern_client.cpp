// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
/*
 * rift-hdr-pattern: the pattern of RiftLinux's riftlinux-openxr-hdr-test as a Wayland window, described with
 * color-management-v1, so Rift Home's path (Wayland window -> OpenXR layer with XR_MNDX_composition_layer_hdr) can be
 * compared capture for capture with the test application's own layer.
 *
 * The pixels are the test application's, bit for bit: 1024x768 cells of 64 px in pseudo-random colours (linear light,
 * white 1.0 = 203 nits, every fourth cell four times as bright), PQ encoded, as RGBA half floats (wl_shm
 * ABGR16161616F). Described as PQ, BT.709 primaries, MaxCLL N; the test application's pq:N mode describes its layer the
 * same way (reference white 203 nits, which is also the protocol's default for PQ).
 *
 * It also prints the pointer input it gets (enter, button, scroll), for Rift Home's controller checks.
 *
 * Even colours instead, for checking how Rift Home decodes colours:
 * - sdr:TF:V: grey of 8-bit code V, transfer TF with sRGB primaries (or untagged);
 * - color:TF:PRIMARIES:R,G,B: an 8-bit colour, transfer TF (gamma22, gamma28, bt1886, srgb = compound_power_2_4, st240,
 *   hlg, untagged) and PRIMARIES (srgb, bt2020, display_p3, dci_p3, adobe_rgb, pal, ntsc);
 * - pqgrey:N: grey at N cd/m², PQ in half floats, BT.2020, MaxCLL 1000 (as HLG's display, for comparing).
 *
 * Usage: rift-hdr-pattern [pq:N, default pq:400 | sdr:TF:V | color:TF:PRIMARIES:R,G,B | pqgrey:N] [seconds, 0 = until stopped]
 */

#include "color-management-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <wayland-client.h>

#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{

constexpr uint32_t kWidth = 1024;
constexpr uint32_t kHeight = 768;
constexpr uint32_t kCell = 64;
constexpr float kWhiteNits = 203.0f;

volatile sig_atomic_t g_stop = 0;

// pq_encode(), half() and the pattern below are riftlinux-openxr-hdr-test's (RiftLinux, BSL-1.0), unchanged, so both
// paths show the same values.

//! SMPTE ST 2084 encoding of nits.
float pq_encode(float nits)
{
    const float m1 = 2610.0f / 16384.0f, m2 = 2523.0f / 4096.0f * 128.0f;
    const float c1 = 3424.0f / 4096.0f, c2 = 2413.0f / 4096.0f * 32.0f, c3 = 2392.0f / 4096.0f * 32.0f;
    const float y = powf(fmaxf(nits, 0.0f) / 10000.0f, m1);
    return powf((c1 + c2 * y) / (1.0f + c3 * y), m2);
}

//! IEEE half from float (round to nearest, no NaN handling needed for this pattern).
uint16_t half(float f)
{
    uint32_t x;
    memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int32_t exp = (int32_t)((x >> 23) & 0xff) - 127 + 15;
    uint32_t mant = x & 0x7fffffu;
    if (exp <= 0) {
        return (uint16_t)sign;
    }
    if (exp >= 31) {
        return (uint16_t)(sign | 0x7bffu);
    }
    mant += 0x1000u; // round
    if (mant & 0x800000u) {
        mant = 0;
        exp++;
        if (exp >= 31) {
            return (uint16_t)(sign | 0x7bffu);
        }
    }
    return (uint16_t)(sign | ((uint32_t)exp << 10) | (mant >> 13));
}

//! The test application's pattern in PQ mode, RGBA half floats (DRM ABGR16161616F: R, G, B, A in memory).
void make_pattern(uint16_t *px)
{
    uint32_t seed = 12345u;
    auto rnd = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return (float)(seed >> 8) / 16777216.0f;
    };
    const uint32_t cols = kWidth / kCell, rows = kHeight / kCell;
    std::vector<float> cell((size_t)cols * rows * 3);
    for (uint32_t i = 0; i < cols * rows; i++) {
        const float gain = (i % 4 == 0) ? 4.0f : 1.0f;
        for (int c = 0; c < 3; c++) {
            cell[i * 3 + c] = (0.05f + 0.95f * rnd()) * gain;
        }
    }
    for (uint32_t y = 0; y < kHeight; y++) {
        for (uint32_t x = 0; x < kWidth; x++) {
            const uint32_t i = (y / kCell) * cols + (x / kCell);
            for (int c = 0; c < 3; c++) {
                px[((size_t)y * kWidth + x) * 4 + c] = half(pq_encode(cell[i * 3 + c] * kWhiteNits));
            }
            px[((size_t)y * kWidth + x) * 4 + 3] = half(1.0f);
        }
    }
}

struct App {
    wl_compositor *compositor = nullptr;
    wl_seat *seat = nullptr;
    wl_pointer *pointer = nullptr;
    double pointerX = 0.0;
    double pointerY = 0.0;
    double scrolled = 0.0;
    wl_shm *shm = nullptr;
    xdg_wm_base *wmBase = nullptr;
    wp_color_manager_v1 *colorManager = nullptr;
    bool halfFloatShm = false;
    bool parametric = false;
    bool pq = false;
    bool srgbPrimaries = false;
    bool configured = false;
    int descriptionState = 0; // 0 waiting, 1 ready, -1 failed
};

void say(const char *format, double a = 0, double b = 0)
{
    std::printf(format, a, b);
    std::printf("\n");
    std::fflush(stdout);
}

const wl_pointer_listener kPointerListener{
    [](void *d, wl_pointer *, uint32_t, wl_surface *, wl_fixed_t x, wl_fixed_t y) {
        auto *app = static_cast<App *>(d);
        app->pointerX = wl_fixed_to_double(x);
        app->pointerY = wl_fixed_to_double(y);
        say("rift-hdr-pattern: pointer entered at %.1f,%.1f", app->pointerX, app->pointerY);
    },
    [](void *, wl_pointer *, uint32_t, wl_surface *) { say("rift-hdr-pattern: pointer left"); },
    [](void *d, wl_pointer *, uint32_t, wl_fixed_t x, wl_fixed_t y) {
        auto *app = static_cast<App *>(d);
        app->pointerX = wl_fixed_to_double(x);
        app->pointerY = wl_fixed_to_double(y);
    },
    [](void *d, wl_pointer *, uint32_t, uint32_t, uint32_t button, uint32_t state) {
        auto *app = static_cast<App *>(d);
        std::printf("rift-hdr-pattern: button %u %s at %.1f,%.1f\n", button,
                    state == WL_POINTER_BUTTON_STATE_PRESSED ? "pressed" : "released", app->pointerX, app->pointerY);
        std::fflush(stdout);
    },
    [](void *d, wl_pointer *, uint32_t, uint32_t axis, wl_fixed_t value) {
        if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
            static_cast<App *>(d)->scrolled += wl_fixed_to_double(value);
        }
    },
    [](void *, wl_pointer *) {},
    [](void *, wl_pointer *, uint32_t) {},
    [](void *d, wl_pointer *, uint32_t, uint32_t axis) {
        if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
            say("rift-hdr-pattern: scrolled %.1f px down, then stopped", static_cast<App *>(d)->scrolled);
            static_cast<App *>(d)->scrolled = 0.0;
        }
    },
    [](void *, wl_pointer *, uint32_t, int32_t) {},
    [](void *, wl_pointer *, uint32_t, int32_t) {},
    [](void *, wl_pointer *, uint32_t, uint32_t) {},
};

const wl_seat_listener kSeatListener{
    [](void *d, wl_seat *seat, uint32_t capabilities) {
        auto *app = static_cast<App *>(d);
        if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !app->pointer) {
            app->pointer = wl_seat_get_pointer(seat);
            wl_pointer_add_listener(app->pointer, &kPointerListener, app);
        }
    },
    [](void *, wl_seat *, const char *) {},
};

void onGlobal(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    auto *app = static_cast<App *>(data);
    if (std::strcmp(interface, wl_seat_interface.name) == 0) {
        app->seat = static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, version < 5 ? version : 5));
        wl_seat_add_listener(app->seat, &kSeatListener, app);
    } else if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        app->compositor = static_cast<wl_compositor *>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
        app->shm = static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
        static const wl_shm_listener shmListener{[](void *d, wl_shm *, uint32_t format) {
            if (format == WL_SHM_FORMAT_ABGR16161616F) {
                static_cast<App *>(d)->halfFloatShm = true;
            }
        }};
        wl_shm_add_listener(app->shm, &shmListener, app);
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
        app->wmBase = static_cast<xdg_wm_base *>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
        static const xdg_wm_base_listener wmListener{[](void *, xdg_wm_base *base, uint32_t serial) {
            xdg_wm_base_pong(base, serial);
        }};
        xdg_wm_base_add_listener(app->wmBase, &wmListener, app);
    } else if (std::strcmp(interface, wp_color_manager_v1_interface.name) == 0) {
        app->colorManager = static_cast<wp_color_manager_v1 *>(
            wl_registry_bind(registry, name, &wp_color_manager_v1_interface, version < 2 ? version : 2));
        static const wp_color_manager_v1_listener cmListener{
            [](void *, wp_color_manager_v1 *, uint32_t) {},
            [](void *d, wp_color_manager_v1 *, uint32_t feature) {
                if (feature == WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC) {
                    static_cast<App *>(d)->parametric = true;
                }
            },
            [](void *d, wp_color_manager_v1 *, uint32_t tf) {
                if (tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ) {
                    static_cast<App *>(d)->pq = true;
                }
            },
            [](void *d, wp_color_manager_v1 *, uint32_t primaries) {
                if (primaries == WP_COLOR_MANAGER_V1_PRIMARIES_SRGB) {
                    static_cast<App *>(d)->srgbPrimaries = true;
                }
            },
            [](void *, wp_color_manager_v1 *) {},
        };
        wp_color_manager_v1_add_listener(app->colorManager, &cmListener, app);
    }
}

} // namespace

int main(int argc, char **argv)
{
    const std::string mode = argc > 1 ? argv[1] : "pq:400";
    const double seconds = argc > 2 ? std::atof(argv[2]) : 0.0;
    // Even colours: sdr:TF:V, color:TF:PRIMARIES:R,G,B, pqgrey:N.
    const auto field = [&mode](int index) {
        size_t start = 0;
        for (int i = 0; i < index; ++i) {
            start = mode.find(':', start);
            if (start == std::string::npos) {
                return std::string();
            }
            ++start;
        }
        const size_t end = mode.find(':', start);
        return mode.substr(start, end == std::string::npos ? std::string::npos : end - start);
    };
    const auto transferNamed = [](const std::string &n) -> int {
        static const std::pair<const char *, uint32_t> names[] = {
            {"gamma22", WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22}, {"gamma28", WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA28},
            {"bt1886", WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886}, {"srgb", WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4},
            {"st240", WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST240}, {"hlg", WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG}, {"untagged", 0u}};
        for (const auto &[name, tf] : names) {
            if (n == name) {
                return int(tf);
            }
        }
        return -1;
    };
    const auto primariesNamed = [](const std::string &n) -> int {
        static const std::pair<const char *, uint32_t> names[] = {
            {"srgb", WP_COLOR_MANAGER_V1_PRIMARIES_SRGB}, {"bt2020", WP_COLOR_MANAGER_V1_PRIMARIES_BT2020},
            {"display_p3", WP_COLOR_MANAGER_V1_PRIMARIES_DISPLAY_P3}, {"dci_p3", WP_COLOR_MANAGER_V1_PRIMARIES_DCI_P3},
            {"adobe_rgb", WP_COLOR_MANAGER_V1_PRIMARIES_ADOBE_RGB}, {"pal", WP_COLOR_MANAGER_V1_PRIMARIES_PAL},
            {"ntsc", WP_COLOR_MANAGER_V1_PRIMARIES_NTSC}};
        for (const auto &[name, p] : names) {
            if (n == name) {
                return int(p);
            }
        }
        return -1;
    };
    const bool sdr = mode.rfind("sdr:", 0) == 0 || mode.rfind("color:", 0) == 0;
    const bool pqGrey = mode.rfind("pqgrey:", 0) == 0;
    int sdrTransfer = 0; // 0: untagged
    int sdrPrimaries = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB;
    uint32_t rgb[3] = {128, 128, 128};
    if (mode.rfind("sdr:", 0) == 0) {
        sdrTransfer = transferNamed(field(1));
        const std::string v = field(2);
        rgb[0] = rgb[1] = rgb[2] = v.empty() ? 128u : uint32_t(std::atoi(v.c_str())) & 0xffu;
    } else if (mode.rfind("color:", 0) == 0) {
        sdrTransfer = transferNamed(field(1));
        sdrPrimaries = primariesNamed(field(2));
        if (std::sscanf(field(3).c_str(), "%u,%u,%u", &rgb[0], &rgb[1], &rgb[2]) != 3) {
            sdrPrimaries = -1;
        }
    } else if (!pqGrey && (mode.rfind("pq:", 0) != 0 || std::atof(mode.c_str() + 3) <= 0)) {
        std::fprintf(stderr, "usage: rift-hdr-pattern [pq:N | sdr:TF:V | color:TF:PRIMARIES:R,G,B | pqgrey:N] [seconds]\n");
        return 2;
    }
    if (sdr && (sdrTransfer < 0 || sdrPrimaries < 0)) {
        std::fprintf(stderr, "rift-hdr-pattern: unknown transfer, primaries or colour in %s\n", mode.c_str());
        return 2;
    }
    const uint32_t maxCll = sdr || pqGrey ? 0u : uint32_t(std::atof(mode.c_str() + 3));
    std::signal(SIGTERM, [](int) { g_stop = 1; });
    std::signal(SIGINT, [](int) { g_stop = 1; });

    wl_display *display = wl_display_connect(nullptr);
    if (!display) {
        std::fprintf(stderr, "rift-hdr-pattern: no Wayland display\n");
        return 1;
    }
    App app;
    wl_registry *registry = wl_display_get_registry(display);
    static const wl_registry_listener registryListener{onGlobal, [](void *, wl_registry *, uint32_t) {}};
    wl_registry_add_listener(registry, &registryListener, &app);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);
    if (!app.compositor || !app.shm || !app.wmBase || !app.colorManager) {
        std::fprintf(stderr, "rift-hdr-pattern: the compositor lacks wl_compositor, wl_shm, xdg_wm_base or wp_color_manager_v1\n");
        return 1;
    }
    if (!sdr && !pqGrey && (!app.halfFloatShm || !app.parametric || !app.pq || !app.srgbPrimaries)) {
        std::fprintf(stderr, "rift-hdr-pattern: needs half-float shm (%d), parametric descriptions (%d), PQ (%d), sRGB primaries (%d)\n",
                     app.halfFloatShm, app.parametric, app.pq, app.srgbPrimaries);
        return 1;
    }

    wl_surface *surface = wl_compositor_create_surface(app.compositor);
    xdg_surface *xdgSurface = xdg_wm_base_get_xdg_surface(app.wmBase, surface);
    static const xdg_surface_listener xdgSurfaceListener{[](void *d, xdg_surface *s, uint32_t serial) {
        xdg_surface_ack_configure(s, serial);
        static_cast<App *>(d)->configured = true;
    }};
    xdg_surface_add_listener(xdgSurface, &xdgSurfaceListener, &app);
    xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdgSurface);
    static const xdg_toplevel_listener toplevelListener{
        [](void *, xdg_toplevel *, int32_t, int32_t, wl_array *) {},
        [](void *, xdg_toplevel *) { g_stop = 1; },
        [](void *, xdg_toplevel *, int32_t, int32_t) {},
        [](void *, xdg_toplevel *, wl_array *) {},
    };
    xdg_toplevel_add_listener(toplevel, &toplevelListener, &app);
    xdg_toplevel_set_title(toplevel, ("HDR pattern " + mode).c_str());
    xdg_toplevel_set_app_id(toplevel, "rift-hdr-pattern");
    wl_surface_commit(surface);
    while (!app.configured && wl_display_dispatch(display) >= 0) {
    }

    if (sdr || pqGrey) {
        // An even colour, described with the transfer and primaries asked for (or not at all).
        if (pqGrey || sdrTransfer != 0) {
            wp_color_management_surface_v1 *cm = wp_color_manager_v1_get_surface(app.colorManager, surface);
            wp_image_description_creator_params_v1 *params = wp_color_manager_v1_create_parametric_creator(app.colorManager);
            wp_image_description_creator_params_v1_set_tf_named(params, pqGrey ? uint32_t(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ) : uint32_t(sdrTransfer));
            wp_image_description_creator_params_v1_set_primaries_named(params, pqGrey ? uint32_t(WP_COLOR_MANAGER_V1_PRIMARIES_BT2020) : uint32_t(sdrPrimaries));
            if (pqGrey) {
                wp_image_description_creator_params_v1_set_max_cll(params, 1000); // as HLG's 1000 cd/m² display, for comparing
            }
            wp_image_description_v1 *d = wp_image_description_creator_params_v1_create(params);
            wl_display_roundtrip(display);
            wp_color_management_surface_v1_set_image_description(cm, d, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
        }
        const size_t sdrStride = kWidth * (pqGrey ? 8 : 4);
        const int sdrFd = memfd_create("rift-hdr-pattern", MFD_CLOEXEC);
        if (sdrFd < 0 || ftruncate(sdrFd, off_t(sdrStride * kHeight)) != 0) {
            std::perror("rift-hdr-pattern: memfd");
            return 1;
        }
        void *mapped = mmap(nullptr, sdrStride * kHeight, PROT_READ | PROT_WRITE, MAP_SHARED, sdrFd, 0);
        if (pqGrey) {
            const uint16_t v = half(pq_encode(float(std::atof(mode.c_str() + 7))));
            auto *px = static_cast<uint16_t *>(mapped);
            for (size_t i = 0; i < size_t(kWidth) * kHeight; ++i) {
                px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = v;
                px[i * 4 + 3] = half(1.0f);
            }
        } else {
            auto *px32 = static_cast<uint32_t *>(mapped);
            for (size_t i = 0; i < size_t(kWidth) * kHeight; ++i) {
                px32[i] = 0xff000000u | (rgb[0] & 0xffu) << 16 | (rgb[1] & 0xffu) << 8 | (rgb[2] & 0xffu);
            }
        }
        wl_shm_pool *sdrPool = wl_shm_create_pool(app.shm, sdrFd, int32_t(sdrStride * kHeight));
        wl_buffer *sdrBuffer = wl_shm_pool_create_buffer(sdrPool, 0, kWidth, kHeight, int32_t(sdrStride),
                                                         pqGrey ? WL_SHM_FORMAT_ABGR16161616F : WL_SHM_FORMAT_XRGB8888);
        wl_surface_attach(surface, sdrBuffer, 0, 0);
        wl_surface_damage_buffer(surface, 0, 0, kWidth, kHeight);
        wl_surface_commit(surface);
        std::printf("rift-hdr-pattern: %ux%u, %s\n", kWidth, kHeight, mode.c_str());
        std::fflush(stdout);
        const auto sdrStart = std::chrono::steady_clock::now();
        while (!g_stop && wl_display_dispatch_pending(display) >= 0) {
            if (seconds > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - sdrStart).count() > seconds) {
                break;
            }
            wl_display_flush(display);
            pollfd pfd{wl_display_get_fd(display), POLLIN, 0};
            if (::poll(&pfd, 1, 100) > 0) {
                wl_display_dispatch(display);
            }
        }
        wl_display_disconnect(display);
        return 0;
    }

    // The description: PQ, BT.709 (sRGB) primaries, MaxCLL N.
    wp_color_management_surface_v1 *cmSurface = wp_color_manager_v1_get_surface(app.colorManager, surface);
    wp_image_description_creator_params_v1 *creator = wp_color_manager_v1_create_parametric_creator(app.colorManager);
    wp_image_description_creator_params_v1_set_tf_named(creator, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
    wp_image_description_creator_params_v1_set_primaries_named(creator, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
    wp_image_description_creator_params_v1_set_max_cll(creator, maxCll);
    wp_image_description_v1 *description = wp_image_description_creator_params_v1_create(creator);
    static const wp_image_description_v1_listener descriptionListener{
        [](void *d, wp_image_description_v1 *, uint32_t, const char *message) {
            std::fprintf(stderr, "rift-hdr-pattern: image description failed: %s\n", message);
            static_cast<App *>(d)->descriptionState = -1;
        },
        [](void *d, wp_image_description_v1 *, uint32_t) { static_cast<App *>(d)->descriptionState = 1; },
        [](void *d, wp_image_description_v1 *, uint32_t, uint32_t) { static_cast<App *>(d)->descriptionState = 1; },
    };
    wp_image_description_v1_add_listener(description, &descriptionListener, &app);
    while (app.descriptionState == 0 && wl_display_dispatch(display) >= 0) {
    }
    if (app.descriptionState < 0) {
        return 1;
    }
    wp_color_management_surface_v1_set_image_description(cmSurface, description, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);

    const size_t stride = kWidth * 8;
    const size_t size = stride * kHeight;
    const int fd = memfd_create("rift-hdr-pattern", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, off_t(size)) != 0) {
        std::perror("rift-hdr-pattern: memfd");
        return 1;
    }
    void *pixels = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    make_pattern(static_cast<uint16_t *>(pixels));
    wl_shm_pool *pool = wl_shm_create_pool(app.shm, fd, int32_t(size));
    wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, kWidth, kHeight, int32_t(stride), WL_SHM_FORMAT_ABGR16161616F);
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, kWidth, kHeight);
    wl_surface_commit(surface);
    std::printf("rift-hdr-pattern: %ux%u, PQ, BT.709, MaxCLL %u nits\n", kWidth, kHeight, maxCll);
    std::fflush(stdout);

    const auto start = std::chrono::steady_clock::now();
    while (!g_stop) {
        if (seconds > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > seconds) {
            break;
        }
        wl_display_flush(display);
        pollfd p{wl_display_get_fd(display), POLLIN, 0};
        if (poll(&p, 1, 100) > 0 && wl_display_dispatch(display) < 0) {
            break;
        }
    }

    wl_buffer_destroy(buffer);
    wl_shm_pool_destroy(pool);
    munmap(pixels, size);
    close(fd);
    wp_image_description_v1_destroy(description);
    wp_color_management_surface_v1_destroy(cmSurface);
    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(xdgSurface);
    wl_surface_destroy(surface);
    wl_display_disconnect(display);
    return 0;
}
