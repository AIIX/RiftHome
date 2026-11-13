// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
/*
 * rift-color-check: checks a compositor's color-management-v1 against the protocol (Rift Home's own
 * implementation): each case on a connection of its own, as a protocol error ends the connection. Prints PASS or FAIL
 * a case and exits with the number of failures.
 *
 * Usage: rift-color-check (WAYLAND_DISPLAY: the compositor to check)
 */

#include "color-management-v1-client-protocol.h"

#include <wayland-client.h>

#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <tuple>
#include <vector>

namespace
{

struct Connection {
    wl_display *display = nullptr;
    wl_compositor *compositor = nullptr;
    wp_color_manager_v1 *manager = nullptr;
    std::vector<uint32_t> transfers;
    std::vector<uint32_t> primaries;
    std::vector<uint32_t> features;
    bool done = false;

    bool open()
    {
        display = wl_display_connect(nullptr);
        if (!display) {
            return false;
        }
        wl_registry *registry = wl_display_get_registry(display);
        static const wl_registry_listener listener{
            [](void *data, wl_registry *r, uint32_t name, const char *interface, uint32_t version) {
                auto *c = static_cast<Connection *>(data);
                if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
                    c->compositor = static_cast<wl_compositor *>(wl_registry_bind(r, name, &wl_compositor_interface, 4));
                } else if (std::strcmp(interface, wp_color_manager_v1_interface.name) == 0) {
                    c->manager = static_cast<wp_color_manager_v1 *>(wl_registry_bind(r, name, &wp_color_manager_v1_interface, version < 2 ? version : 2));
                    static const wp_color_manager_v1_listener cm{
                        [](void *, wp_color_manager_v1 *, uint32_t) {},
                        [](void *d, wp_color_manager_v1 *, uint32_t f) { static_cast<Connection *>(d)->features.push_back(f); },
                        [](void *d, wp_color_manager_v1 *, uint32_t tf) { static_cast<Connection *>(d)->transfers.push_back(tf); },
                        [](void *d, wp_color_manager_v1 *, uint32_t p) { static_cast<Connection *>(d)->primaries.push_back(p); },
                        [](void *d, wp_color_manager_v1 *) { static_cast<Connection *>(d)->done = true; },
                    };
                    wp_color_manager_v1_add_listener(c->manager, &cm, c);
                }
            },
            [](void *, wl_registry *, uint32_t) {},
        };
        wl_registry_add_listener(registry, &listener, this);
        wl_display_roundtrip(display);
        wl_display_roundtrip(display);
        return compositor && manager && done;
    }
    ~Connection()
    {
        if (display) {
            wl_display_disconnect(display);
        }
    }
    //! The protocol error the compositor sent (code, interface name), or code -1 for none.
    int error(std::string &interface) const
    {
        wl_display_roundtrip(display);
        if (wl_display_get_error(display) != EPROTO) {
            return -1;
        }
        const wl_interface *iface = nullptr;
        uint32_t id = 0;
        const int code = int(wl_display_get_protocol_error(display, &iface, &id));
        interface = iface ? iface->name : "";
        return code;
    }
};

struct Description {
    int state = 0; // 0 waiting, 1 ready, -1 failed
    uint64_t identity = 0;
    uint32_t cause = 0;
};

const wp_image_description_v1_listener kDescriptionListener{
    [](void *d, wp_image_description_v1 *, uint32_t cause, const char *) {
        auto *s = static_cast<Description *>(d);
        s->state = -1;
        s->cause = cause;
    },
    [](void *d, wp_image_description_v1 *, uint32_t identity) {
        auto *s = static_cast<Description *>(d);
        s->state = 1;
        s->identity = identity;
    },
    [](void *d, wp_image_description_v1 *, uint32_t hi, uint32_t lo) {
        auto *s = static_cast<Description *>(d);
        s->state = 1;
        s->identity = uint64_t(hi) << 32 | lo;
    },
};

wp_image_description_v1 *parametric(Connection &c, uint32_t tf, uint32_t primaries, Description &state)
{
    wp_image_description_creator_params_v1 *p = wp_color_manager_v1_create_parametric_creator(c.manager);
    wp_image_description_creator_params_v1_set_tf_named(p, tf);
    wp_image_description_creator_params_v1_set_primaries_named(p, primaries);
    wp_image_description_v1 *d = wp_image_description_creator_params_v1_create(p);
    wp_image_description_v1_add_listener(d, &kDescriptionListener, &state);
    wl_display_roundtrip(c.display);
    return d;
}

struct Info {
    uint32_t tf = 0;
    uint32_t primariesNamed = 0;
    bool primaries = false, luminances = false, targetPrimaries = false, targetLuminance = false, done = false;
    uint32_t reference = 0;
};

int failures = 0;

void report(bool pass, const char *name, const std::string &detail = {})
{
    std::printf("%s %s%s%s\n", pass ? "PASS" : "FAIL", name, detail.empty() ? "" : ": ", detail.c_str());
    failures += pass ? 0 : 1;
}

//! A case that must end with protocol error @p code on @p interface.
void expectError(const char *name, const char *interface, int code, const std::function<void(Connection &)> &run)
{
    Connection c;
    if (!c.open()) {
        report(false, name, "no compositor or no color manager");
        return;
    }
    run(c);
    std::string got;
    const int error = c.error(got);
    // An error on a creator's create (a destructor request) reaches the client for an object it has already let go:
    // libwayland reports no interface then.
    const bool destroyed = got.empty() && std::strcmp(interface, "wp_image_description_creator_params_v1") == 0;
    report(error == code && (got == interface || destroyed), name,
           "got " + std::to_string(error) + " on " + (got.empty() ? std::string("the destroyed creator") : got));
}

bool has(const std::vector<uint32_t> &list, uint32_t v)
{
    for (uint32_t x : list) {
        if (x == v) {
            return true;
        }
    }
    return false;
}

} // namespace

int main()
{
    // What is offered.
    {
        Connection c;
        if (!c.open()) {
            std::fprintf(stderr, "rift-color-check: no compositor with wp_color_manager_v1\n");
            return 100;
        }
        report(has(c.transfers, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG) && has(c.transfers, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ)
                   && has(c.transfers, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22) && !has(c.transfers, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB),
               "offers HLG, PQ and gamma 2.2, not the deprecated sRGB", std::to_string(c.transfers.size()) + " transfer functions");
        report(has(c.primaries, WP_COLOR_MANAGER_V1_PRIMARIES_DISPLAY_P3) && has(c.primaries, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020),
               "offers Display P3 and BT.2020", std::to_string(c.primaries.size()) + " primaries");
        report(has(c.features, WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC) && !has(c.features, WP_COLOR_MANAGER_V1_FEATURE_ICC_V2_V4), "parametric, no ICC");

        // Descriptions that must become ready.
        for (const auto &[tf, primaries, name] : std::vector<std::tuple<uint32_t, uint32_t, const char *>>{
                 {WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020, "HLG BT.2020 ready"},
                 {WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4, WP_COLOR_MANAGER_V1_PRIMARIES_DISPLAY_P3, "sRGB curve Display P3 ready"},
                 {WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ, WP_COLOR_MANAGER_V1_PRIMARIES_DCI_P3, "PQ DCI-P3 ready"},
                 {WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA28, WP_COLOR_MANAGER_V1_PRIMARIES_PAL, "gamma 2.8 PAL ready"},
             }) {
            Description s;
            wp_image_description_v1 *d = parametric(c, tf, primaries, s);
            report(s.state == 1 && s.identity != 0, name, "identity " + std::to_string(s.identity));
            wp_image_description_v1_destroy(d);
        }
        // A power curve and chromaticities of its own, luminances, mastering metadata.
        {
            wp_image_description_creator_params_v1 *p = wp_color_manager_v1_create_parametric_creator(c.manager);
            wp_image_description_creator_params_v1_set_tf_power(p, 26000);
            wp_image_description_creator_params_v1_set_primaries(p, 680000, 320000, 265000, 690000, 150000, 60000, 312700, 329000);
            wp_image_description_creator_params_v1_set_luminances(p, 2000, 100, 100);
            wp_image_description_creator_params_v1_set_mastering_display_primaries(p, 680000, 320000, 265000, 690000, 150000, 60000, 312700, 329000);
            wp_image_description_creator_params_v1_set_mastering_luminance(p, 50, 1000);
            wp_image_description_creator_params_v1_set_max_cll(p, 800);
            wp_image_description_creator_params_v1_set_max_fall(p, 300);
            Description s;
            wp_image_description_v1 *d = wp_image_description_creator_params_v1_create(p);
            wp_image_description_v1_add_listener(d, &kDescriptionListener, &s);
            wl_display_roundtrip(c.display);
            report(s.state == 1, "power curve, own primaries, luminances, mastering metadata ready");
            // Set on a surface and committed: no error.
            wl_surface *surface = wl_compositor_create_surface(c.compositor);
            wp_color_management_surface_v1 *cs = wp_color_manager_v1_get_surface(c.manager, surface);
            wp_color_management_surface_v1_set_image_description(cs, d, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
            wl_surface_commit(surface);
            wp_color_management_surface_v1_unset_image_description(cs);
            wl_surface_commit(surface);
            wp_color_management_surface_v1_destroy(cs);
            // A second color management surface once the first is gone.
            cs = wp_color_manager_v1_get_surface(c.manager, surface);
            wp_color_management_surface_v1_destroy(cs);
            wl_surface_destroy(surface);
            std::string got;
            const int error = c.error(got);
            report(error == -1, "set, unset, destroy and again on a surface", error == -1 ? "" : "error " + std::to_string(error) + " on " + got);
            if (error != -1) {
                return failures;
            }
            wp_image_description_v1_destroy(d);
        }
        // The preferred description and its information.
        {
            wl_surface *surface = wl_compositor_create_surface(c.compositor);
            wp_color_management_surface_feedback_v1 *fb = wp_color_manager_v1_get_surface_feedback(c.manager, surface);
            Description s;
            wp_image_description_v1 *d = wp_color_management_surface_feedback_v1_get_preferred(fb);
            wp_image_description_v1_add_listener(d, &kDescriptionListener, &s);
            wl_display_roundtrip(c.display);
            Info info;
            wp_image_description_info_v1 *i = wp_image_description_v1_get_information(d);
            static const wp_image_description_info_v1_listener infoListener{
                [](void *d, wp_image_description_info_v1 *) { static_cast<Info *>(d)->done = true; },
                [](void *, wp_image_description_info_v1 *, int32_t fd, uint32_t) { close(fd); },
                [](void *d, wp_image_description_info_v1 *, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) {
                    static_cast<Info *>(d)->primaries = true;
                },
                [](void *d, wp_image_description_info_v1 *, uint32_t p) { static_cast<Info *>(d)->primariesNamed = p; },
                [](void *, wp_image_description_info_v1 *, uint32_t) {},
                [](void *d, wp_image_description_info_v1 *, uint32_t tf) { static_cast<Info *>(d)->tf = tf; },
                [](void *d, wp_image_description_info_v1 *, uint32_t, uint32_t, uint32_t reference) {
                    static_cast<Info *>(d)->luminances = true;
                    static_cast<Info *>(d)->reference = reference;
                },
                [](void *d, wp_image_description_info_v1 *, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) {
                    static_cast<Info *>(d)->targetPrimaries = true;
                },
                [](void *d, wp_image_description_info_v1 *, uint32_t, uint32_t) { static_cast<Info *>(d)->targetLuminance = true; },
                [](void *, wp_image_description_info_v1 *, uint32_t) {},
                [](void *, wp_image_description_info_v1 *, uint32_t) {},
            };
            wp_image_description_info_v1_add_listener(i, &infoListener, &info);
            wl_display_roundtrip(c.display);
            report(s.state == 1 && info.done && info.primaries && info.luminances && info.targetPrimaries && info.targetLuminance && info.tf != 0,
                   "preferred description and its information",
                   "tf " + std::to_string(info.tf) + ", primaries " + std::to_string(info.primariesNamed) + ", white " + std::to_string(info.reference));
            wp_image_description_v1_destroy(d);
            wp_color_management_surface_feedback_v1_destroy(fb);
            wl_surface_destroy(surface);
        }
    }

    // Protocol errors the specification demands.
    expectError("a second color management surface: surface_exists", "wp_color_manager_v1", WP_COLOR_MANAGER_V1_ERROR_SURFACE_EXISTS, [](Connection &c) {
        wl_surface *s = wl_compositor_create_surface(c.compositor);
        wp_color_manager_v1_get_surface(c.manager, s);
        wp_color_manager_v1_get_surface(c.manager, s);
    });
    expectError("ICC creator: unsupported_feature", "wp_color_manager_v1", WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE,
                [](Connection &c) { wp_color_manager_v1_create_icc_creator(c.manager); });
    expectError("create without primaries: incomplete_set", "wp_image_description_creator_params_v1",
                WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INCOMPLETE_SET, [](Connection &c) {
                    wp_image_description_creator_params_v1 *p = wp_color_manager_v1_create_parametric_creator(c.manager);
                    wp_image_description_creator_params_v1_set_tf_named(p, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
                    wp_image_description_creator_params_v1_create(p);
                });
    expectError("deprecated sRGB transfer: invalid_tf", "wp_image_description_creator_params_v1", WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF,
                [](Connection &c) {
                    wp_image_description_creator_params_v1 *p = wp_color_manager_v1_create_parametric_creator(c.manager);
                    wp_image_description_creator_params_v1_set_tf_named(p, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB);
                });
    expectError("primaries twice: already_set", "wp_image_description_creator_params_v1", WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET,
                [](Connection &c) {
                    wp_image_description_creator_params_v1 *p = wp_color_manager_v1_create_parametric_creator(c.manager);
                    wp_image_description_creator_params_v1_set_primaries_named(p, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
                    wp_image_description_creator_params_v1_set_primaries_named(p, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
                });
    expectError("power exponent 0.5: invalid_tf", "wp_image_description_creator_params_v1", WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF,
                [](Connection &c) {
                    wp_image_description_creator_params_v1 *p = wp_color_manager_v1_create_parametric_creator(c.manager);
                    wp_image_description_creator_params_v1_set_tf_power(p, 5000);
                });
    expectError("max_fall above max_cll: invalid_luminance", "wp_image_description_creator_params_v1",
                WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE, [](Connection &c) {
                    wp_image_description_creator_params_v1 *p = wp_color_manager_v1_create_parametric_creator(c.manager);
                    wp_image_description_creator_params_v1_set_tf_named(p, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
                    wp_image_description_creator_params_v1_set_primaries_named(p, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
                    wp_image_description_creator_params_v1_set_max_cll(p, 400);
                    wp_image_description_creator_params_v1_set_max_fall(p, 1000);
                    wp_image_description_creator_params_v1_create(p);
                });
    expectError("information of a client's own description: no_information", "wp_image_description_v1", WP_IMAGE_DESCRIPTION_V1_ERROR_NO_INFORMATION,
                [](Connection &c) {
                    Description s;
                    wp_image_description_v1 *d = parametric(c, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB, s);
                    wp_image_description_v1_get_information(d);
                });
    expectError("an unoffered rendering intent: render_intent", "wp_color_management_surface_v1", WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_RENDER_INTENT,
                [](Connection &c) {
                    Description s;
                    wp_image_description_v1 *d = parametric(c, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB, s);
                    wl_surface *surface = wl_compositor_create_surface(c.compositor);
                    wp_color_management_surface_v1 *cs = wp_color_manager_v1_get_surface(c.manager, surface);
                    wp_color_management_surface_v1_set_image_description(cs, d, WP_COLOR_MANAGER_V1_RENDER_INTENT_ABSOLUTE);
                });
    expectError("a request on a surface's object after the surface: inert", "wp_color_management_surface_v1", WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_INERT,
                [](Connection &c) {
                    wl_surface *surface = wl_compositor_create_surface(c.compositor);
                    wp_color_management_surface_v1 *cs = wp_color_manager_v1_get_surface(c.manager, surface);
                    wl_surface_destroy(surface);
                    wl_display_roundtrip(c.display);
                    wp_color_management_surface_v1_unset_image_description(cs);
                });

    std::printf("%d failure(s)\n", failures);
    return failures;
}
