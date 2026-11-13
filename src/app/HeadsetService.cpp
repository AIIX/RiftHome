// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "app/HeadsetService.hpp"

#include <QDBusConnection>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcHeadset, "rifthome.headset")

namespace RiftHome::App
{

HeadsetService::HeadsetService(std::function<void()> showLensTest, QObject *parent)
    : QObject(parent)
    , m_showLensTest(std::move(showLensTest))
{
}

bool HeadsetService::registerOnBus()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCWarning(lcHeadset) << "no session bus: Settings cannot show the lens test";
        return false;
    }
    if (!bus.registerObject(QStringLiteral("/Headset"), this, QDBusConnection::ExportScriptableSlots)) {
        qCWarning(lcHeadset) << "could not export /Headset";
        return false;
    }
    return true;
}

void HeadsetService::ShowLensTest()
{
    m_showLensTest();
}

} // namespace RiftHome::App
