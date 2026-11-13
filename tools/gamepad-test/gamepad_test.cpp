// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
// Rift Home's virtual gamepad on its own: made, listed by the kernel, A pressed and let go, removed.
// Prints what it finds; exits 0 when the kernel listed it. Usage: rift-gamepad-test
#include "xr/VirtualGamepad.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QLoggingCategory>
#include <QThread>

#include <cstdio>

Q_LOGGING_CATEGORY(lcXr, "rifthome.xr")

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    RiftHome::Xr::VirtualGamepad pad;
    if (!pad.open()) {
        return 1;
    }
    QThread::msleep(300);
    QFile devices(QStringLiteral("/proc/bus/input/devices"));
    const bool listed = devices.open(QIODevice::ReadOnly) && devices.readAll().contains("Rift Home gamepad");
    std::printf("listed by the kernel: %s\n", listed ? "yes" : "NO");
    RiftHome::Spatial::GamepadState state;
    state.a = true;
    state.lx = 20000;
    pad.send(state);
    QThread::msleep(100);
    pad.send({});
    pad.close();
    return listed ? 0 : 2;
}
