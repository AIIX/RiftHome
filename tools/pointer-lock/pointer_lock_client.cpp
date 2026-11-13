// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
// A window that locks the pointer as a game's mouse look does: on pointer enter it asks for a lock and
// relative motion, and prints what it gets: LOCKED / UNLOCKED, RELATIVE dx dy, MOTION x y. For Rift Home's headless
// checks (--test-click moves the controllers' pointer onto it). Usage: rift-pointer-lock-test [seconds]
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <wayland-client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace
{

struct App {
    wl_compositor *compositor = nullptr;
    wl_shm *shm = nullptr;
    wl_seat *seat = nullptr;
    xdg_wm_base *wm = nullptr;
    zwp_pointer_constraints_v1 *constraints = nullptr;
    zwp_relative_pointer_manager_v1 *relative = nullptr;
    wl_surface *surface = nullptr;
    wl_pointer *pointer = nullptr;
    zwp_locked_pointer_v1 *lock = nullptr;
    zwp_relative_pointer_v1 *relativePointer = nullptr;
    bool configured = false;
};

void global(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t)
{
    auto *app = static_cast<App *>(data);
    if (!strcmp(interface, wl_compositor_interface.name)) {
        app->compositor = static_cast<wl_compositor *>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        app->shm = static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (!strcmp(interface, wl_seat_interface.name)) {
        app->seat = static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, 1)); // v1: the five pointer events below
    } else if (!strcmp(interface, xdg_wm_base_interface.name)) {
        app->wm = static_cast<xdg_wm_base *>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
    } else if (!strcmp(interface, zwp_pointer_constraints_v1_interface.name)) {
        app->constraints = static_cast<zwp_pointer_constraints_v1 *>(wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1));
    } else if (!strcmp(interface, zwp_relative_pointer_manager_v1_interface.name)) {
        app->relative = static_cast<zwp_relative_pointer_manager_v1 *>(wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1));
    }
}

void globalRemove(void *, wl_registry *, uint32_t)
{
}

const wl_registry_listener registryListener{global, globalRemove};

void ping(void *, xdg_wm_base *wm, uint32_t serial)
{
    xdg_wm_base_pong(wm, serial);
}

const xdg_wm_base_listener wmListener{ping};

void surfaceConfigure(void *data, xdg_surface *surface, uint32_t serial)
{
    xdg_surface_ack_configure(surface, serial);
    static_cast<App *>(data)->configured = true;
}

const xdg_surface_listener xdgSurfaceListener{surfaceConfigure};

void locked(void *, zwp_locked_pointer_v1 *)
{
    std::printf("LOCKED\n");
    std::fflush(stdout);
}

void unlocked(void *, zwp_locked_pointer_v1 *)
{
    std::printf("UNLOCKED\n");
    std::fflush(stdout);
}

const zwp_locked_pointer_v1_listener lockListener{locked, unlocked};

void relativeMotion(void *, zwp_relative_pointer_v1 *, uint32_t, uint32_t, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t, wl_fixed_t)
{
    std::printf("RELATIVE %.1f %.1f\n", wl_fixed_to_double(dx), wl_fixed_to_double(dy));
    std::fflush(stdout);
}

const zwp_relative_pointer_v1_listener relativeListener{relativeMotion};

void enter(void *data, wl_pointer *pointer, uint32_t, wl_surface *surface, wl_fixed_t, wl_fixed_t)
{
    auto *app = static_cast<App *>(data);
    std::printf("ENTER\n");
    if (!app->lock && app->constraints) {
        app->lock = zwp_pointer_constraints_v1_lock_pointer(app->constraints, surface, pointer, nullptr,
                                                            ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        zwp_locked_pointer_v1_add_listener(app->lock, &lockListener, app);
    }
    if (!app->relativePointer && app->relative) {
        app->relativePointer = zwp_relative_pointer_manager_v1_get_relative_pointer(app->relative, pointer);
        zwp_relative_pointer_v1_add_listener(app->relativePointer, &relativeListener, app);
    }
    std::fflush(stdout);
}

void leave(void *, wl_pointer *, uint32_t, wl_surface *)
{
}

void motion(void *, wl_pointer *, uint32_t, wl_fixed_t x, wl_fixed_t y)
{
    std::printf("MOTION %.1f %.1f\n", wl_fixed_to_double(x), wl_fixed_to_double(y));
    std::fflush(stdout);
}

void button(void *, wl_pointer *, uint32_t, uint32_t, uint32_t, uint32_t)
{
}

void axis(void *, wl_pointer *, uint32_t, uint32_t, wl_fixed_t)
{
}

const wl_pointer_listener pointerListener{enter, leave, motion, button, axis, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

wl_buffer *makeBuffer(wl_shm *shm, int width, int height)
{
    const int stride = width * 4;
    const int size = stride * height;
    const int fd = memfd_create("pointer-lock-test", 0);
    if (fd < 0 || ftruncate(fd, size) < 0) {
        return nullptr;
    }
    auto *pixels = static_cast<uint32_t *>(mmap(nullptr, size_t(size), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    for (int i = 0; i < width * height; ++i) {
        pixels[i] = 0xff2a6fdb; // a blue square
    }
    munmap(pixels, size_t(size));
    wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

} // namespace

int main(int argc, char **argv)
{
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 10;
    wl_display *display = wl_display_connect(nullptr);
    if (!display) {
        std::fprintf(stderr, "no Wayland display\n");
        return 1;
    }
    App app;
    wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registryListener, &app);
    wl_display_roundtrip(display);
    if (!app.compositor || !app.shm || !app.wm || !app.seat) {
        std::fprintf(stderr, "missing globals\n");
        return 1;
    }
    std::printf("constraints %s, relative pointer %s\n", app.constraints ? "offered" : "MISSING", app.relative ? "offered" : "MISSING");
    xdg_wm_base_add_listener(app.wm, &wmListener, &app);
    app.pointer = wl_seat_get_pointer(app.seat);
    wl_pointer_add_listener(app.pointer, &pointerListener, &app);
    app.surface = wl_compositor_create_surface(app.compositor);
    xdg_surface *xdgSurface = xdg_wm_base_get_xdg_surface(app.wm, app.surface);
    xdg_surface_add_listener(xdgSurface, &xdgSurfaceListener, &app);
    xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdgSurface);
    xdg_toplevel_set_title(toplevel, "pointer-lock-test");
    xdg_toplevel_set_app_id(toplevel, "rift-pointer-lock-test");
    wl_surface_commit(app.surface);
    while (!app.configured && wl_display_dispatch(display) >= 0) {
    }
    wl_surface_attach(app.surface, makeBuffer(app.shm, 300, 300), 0, 0);
    wl_surface_commit(app.surface);
    std::fflush(stdout);
    const time_t end = time(nullptr) + seconds;
    while (time(nullptr) < end) {
        wl_display_flush(display);
        wl_display_dispatch_pending(display);
        if (wl_display_prepare_read(display) == 0) {
            usleep(10000);
            wl_display_read_events(display);
        }
        wl_display_dispatch_pending(display);
    }
    wl_display_disconnect(display);
    return 0;
}
