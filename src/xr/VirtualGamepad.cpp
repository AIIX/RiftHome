// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "xr/VirtualGamepad.hpp"

#include <QLoggingCategory>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

Q_DECLARE_LOGGING_CATEGORY(lcXr)

namespace RiftHome::Xr
{

namespace
{

constexpr int kButtons[] = {BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR};

bool setupAxis(int fd, int code, int minimum, int maximum, int fuzz, int flat)
{
    uinput_abs_setup setup{};
    setup.code = uint16_t(code);
    setup.absinfo.minimum = minimum;
    setup.absinfo.maximum = maximum;
    setup.absinfo.fuzz = fuzz;
    setup.absinfo.flat = flat;
    return ioctl(fd, UI_SET_ABSBIT, code) == 0 && ioctl(fd, UI_ABS_SETUP, &setup) == 0;
}

} // namespace

VirtualGamepad::~VirtualGamepad()
{
    close();
}

bool VirtualGamepad::open()
{
    if (m_fd >= 0) {
        return true;
    }
    const int fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        qCWarning(lcXr) << "game mode: no virtual gamepad (/dev/uinput:" << strerror(errno) << ")";
        return false;
    }
    bool ok = ioctl(fd, UI_SET_EVBIT, EV_KEY) == 0 && ioctl(fd, UI_SET_EVBIT, EV_ABS) == 0 && ioctl(fd, UI_SET_EVBIT, EV_SYN) == 0;
    for (int button : kButtons) {
        ok = ok && ioctl(fd, UI_SET_KEYBIT, button) == 0;
    }
    // As the xpad driver presents an Xbox 360 pad: sticks -32768..32767, triggers 0..255, the d-pad as a hat.
    for (int axis : {ABS_X, ABS_Y, ABS_RX, ABS_RY}) {
        ok = ok && setupAxis(fd, axis, -32768, 32767, 16, 128);
    }
    ok = ok && setupAxis(fd, ABS_Z, 0, 255, 0, 0) && setupAxis(fd, ABS_RZ, 0, 255, 0, 0);
    ok = ok && setupAxis(fd, ABS_HAT0X, -1, 1, 0, 0) && setupAxis(fd, ABS_HAT0Y, -1, 1, 0, 0);
    uinput_setup setup{};
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x045e; // Microsoft
    setup.id.product = 0x028e; // Xbox 360 Controller
    setup.id.version = 0x0114;
    std::strncpy(setup.name, "Rift Home gamepad (Touch controllers)", UINPUT_MAX_NAME_SIZE - 1);
    ok = ok && ioctl(fd, UI_DEV_SETUP, &setup) == 0 && ioctl(fd, UI_DEV_CREATE) == 0;
    if (!ok) {
        qCWarning(lcXr) << "game mode: the virtual gamepad could not be made:" << strerror(errno);
        ::close(fd);
        return false;
    }
    m_fd = fd;
    m_last = {};
    qCInfo(lcXr) << "game mode: virtual gamepad made (an Xbox 360 pad to games)";
    return true;
}

void VirtualGamepad::rest()
{
    send({});
}

void VirtualGamepad::close()
{
    if (m_fd < 0) {
        return;
    }
    rest(); // everything let go first
    ioctl(m_fd, UI_DEV_DESTROY);
    ::close(m_fd);
    m_fd = -1;
    qCInfo(lcXr) << "game mode: virtual gamepad removed";
}

void VirtualGamepad::writeEvent(int type, int code, int value)
{
    input_event event{};
    event.type = uint16_t(type);
    event.code = uint16_t(code);
    event.value = value;
    if (::write(m_fd, &event, sizeof(event)) < 0 && errno != EAGAIN) {
        qCWarning(lcXr) << "game mode: gamepad write failed:" << strerror(errno);
    }
}

void VirtualGamepad::send(const Spatial::GamepadState &state)
{
    if (m_fd < 0 || state == m_last) {
        return;
    }
    const auto axis = [&](int code, int now, int before) {
        if (now != before) {
            writeEvent(EV_ABS, code, now);
        }
    };
    const auto key = [&](int code, bool now, bool before) {
        if (now != before) {
            writeEvent(EV_KEY, code, now ? 1 : 0);
        }
    };
    axis(ABS_X, state.lx, m_last.lx);
    axis(ABS_Y, state.ly, m_last.ly);
    axis(ABS_RX, state.rx, m_last.rx);
    axis(ABS_RY, state.ry, m_last.ry);
    axis(ABS_Z, state.lt, m_last.lt);
    axis(ABS_RZ, state.rt, m_last.rt);
    key(BTN_A, state.a, m_last.a);
    key(BTN_B, state.b, m_last.b);
    key(BTN_X, state.x, m_last.x);
    key(BTN_Y, state.y, m_last.y);
    key(BTN_TL, state.lb, m_last.lb);
    key(BTN_TR, state.rb, m_last.rb);
    key(BTN_START, state.start, m_last.start);
    key(BTN_THUMBL, state.thumbl, m_last.thumbl);
    key(BTN_THUMBR, state.thumbr, m_last.thumbr);
    writeEvent(EV_SYN, SYN_REPORT, 0);
    m_last = state;
}

} // namespace RiftHome::Xr
