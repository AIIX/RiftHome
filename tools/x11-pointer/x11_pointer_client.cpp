// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
// An X11 window of a given size that prints what the X server tells it: SCREEN w h (the X screen), SIZE w h (its own,
// after Rift Home configured it), PRESS button x y (window coordinates). For Rift Home's headless checks of its X11
// screen (at a 1920x1080 X screen the lower and right parts of Steam's 2683x2078 window got no pointer).
// Usage: rift-x11-pointer-test [width height [seconds [fullscreen]]] (fullscreen: asked as SDL and Wine games ask, the
// _NET_WM_STATE property before mapping and a _NET_WM_STATE message to the window manager after)
#include <xcb/xcb.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

int main(int argc, char **argv)
{
    const int width = argc > 2 ? std::atoi(argv[1]) : 2683;
    const int height = argc > 2 ? std::atoi(argv[2]) : 2078;
    const int seconds = argc > 3 ? std::atoi(argv[3]) : 30;
    const bool fullscreen = argc > 4 && std::strcmp(argv[4], "fullscreen") == 0;

    xcb_connection_t *connection = xcb_connect(nullptr, nullptr);
    if (xcb_connection_has_error(connection)) {
        std::fprintf(stderr, "no X display\n");
        return 1;
    }
    xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    std::printf("SCREEN %d %d\n", screen->width_in_pixels, screen->height_in_pixels);
    std::fflush(stdout);

    const xcb_window_t window = xcb_generate_id(connection);
    const uint32_t values[2] = {screen->white_pixel,
                                XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_STRUCTURE_NOTIFY};
    xcb_create_window(connection, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0, uint16_t(width), uint16_t(height), 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
    const char title[] = "rift-x11-pointer-test";
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, std::strlen(title), title);
    const char wmClass[] = "rift-x11-pointer-test\0rift-x11-pointer-test";
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof(wmClass), wmClass);
    const auto atom = [connection](const char *name) {
        xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(connection, xcb_intern_atom(connection, 0, std::strlen(name), name), nullptr);
        const xcb_atom_t value = reply ? reply->atom : xcb_atom_t(XCB_ATOM_NONE);
        std::free(reply);
        return value;
    };
    const xcb_atom_t state = atom("_NET_WM_STATE");
    const xcb_atom_t full = atom("_NET_WM_STATE_FULLSCREEN");
    if (fullscreen) {
        xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, state, XCB_ATOM_ATOM, 32, 1, &full);
    }
    xcb_map_window(connection, window);
    xcb_flush(connection);
    if (fullscreen) {
        xcb_client_message_event_t message{};
        message.response_type = XCB_CLIENT_MESSAGE;
        message.format = 32;
        message.window = window;
        message.type = state;
        message.data.data32[0] = 1; // _NET_WM_STATE_ADD
        message.data.data32[1] = full;
        message.data.data32[3] = 1; // from a normal application
        xcb_send_event(connection, 0, screen->root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                       reinterpret_cast<const char *>(&message));
        xcb_flush(connection);
    }

    const time_t end = std::time(nullptr) + seconds;
    while (std::time(nullptr) < end) {
        xcb_generic_event_t *event = xcb_poll_for_event(connection);
        if (!event) {
            if (xcb_connection_has_error(connection)) {
                break;
            }
            timespec pause{0, 20 * 1000 * 1000};
            nanosleep(&pause, nullptr);
            continue;
        }
        switch (event->response_type & ~0x80) {
        case XCB_CONFIGURE_NOTIFY: {
            const auto *configure = reinterpret_cast<xcb_configure_notify_event_t *>(event);
            if (configure->window == window) {
                std::printf("SIZE %d %d at %d,%d\n", configure->width, configure->height, configure->x, configure->y);
            }
            break;
        }
        case XCB_BUTTON_PRESS: {
            const auto *press = reinterpret_cast<xcb_button_press_event_t *>(event);
            std::printf("PRESS %d %d %d\n", press->detail, press->event_x, press->event_y);
            break;
        }
        default:
            break;
        }
        std::fflush(stdout);
        std::free(event);
    }
    xcb_disconnect(connection);
    return 0;
}
