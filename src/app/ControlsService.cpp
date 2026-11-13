// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "app/ControlsService.hpp"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcControls, "rifthome.controls")

namespace RiftHome::App
{

ControlsService::ControlsService(std::function<void()> start, std::function<void()> cancel, QObject *parent)
    : QObject(parent)
    , m_start(std::move(start))
    , m_cancel(std::move(cancel))
{
}

bool ControlsService::registerOnBus()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCWarning(lcControls) << "no session bus: Settings cannot record a controller binding";
        return false;
    }
    if (!bus.registerObject(QStringLiteral("/Controls"), this, QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals)) {
        qCWarning(lcControls) << "could not export /Controls";
        return false;
    }
    const auto reply = bus.interface()->registerService(QStringLiteral("org.rifthome.RiftHome"), QDBusConnectionInterface::DontQueueService,
                                                        QDBusConnectionInterface::DontAllowReplacement);
    if (!reply.isValid() || reply.value() != QDBusConnectionInterface::ServiceRegistered) {
        qCWarning(lcControls) << "org.rifthome.RiftHome is taken on this bus: Settings cannot record a controller binding";
        bus.unregisterObject(QStringLiteral("/Controls"));
        return false;
    }
    return true;
}

void ControlsService::recorded(const QString &binding)
{
    QMetaObject::invokeMethod(this, [this, binding] {
        Q_EMIT BindingRecorded(binding);
    });
}

void ControlsService::RecordBinding()
{
    m_start();
}

void ControlsService::CancelRecording()
{
    m_cancel();
}

} // namespace RiftHome::App
