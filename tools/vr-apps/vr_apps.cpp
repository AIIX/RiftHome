// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
// The VR apps connected to RiftLinux's runtime, and switching between them (through libmonado as Rift
// Home does): rift-vr-apps            lists them (SHOWN marks the one in front)
//                 rift-vr-apps switch NAME   brings the first app whose name contains NAME in front
#include "xr/RuntimeApps.hpp"

#include <QCoreApplication>
#include <QLoggingCategory>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

Q_LOGGING_CATEGORY(lcXr, "rifthome.xr")

namespace
{

void list(const std::vector<RiftHome::Xr::VrApp> &apps)
{
    for (const auto &app : apps) {
        std::printf("%u\t%s%s%s\n", app.id, app.name.c_str(), app.primary ? "\tSHOWN" : "", app.focused ? "\tfocused" : "");
    }
    if (apps.empty()) {
        std::printf("(no VR app with a session)\n");
    }
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    RiftHome::Xr::RuntimeApps apps("");
    apps.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    if (!apps.connected()) {
        std::fprintf(stderr, "no runtime (or no libmonado)\n");
        return 1;
    }
    if (argc > 2 && std::string(argv[1]) == "switch") {
        for (const auto &vr : apps.apps()) {
            if (vr.name.find(argv[2]) != std::string::npos) {
                apps.makePrimary(vr.id);
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                list(apps.apps());
                return 0;
            }
        }
        std::fprintf(stderr, "no VR app named like %s\n", argv[2]);
        list(apps.apps());
        return 1;
    }
    list(apps.apps());
    return 0;
}
