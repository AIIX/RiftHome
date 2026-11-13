// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>

#include <functional>

namespace RiftHome::App
{

/*!
 * What Settings asks of Rift Home about the headset, on the session bus Rift Home already serves (service
 * org.rifthome.RiftHome, taken by ControlsService; object /Headset, interface org.rifthome.RiftHome.Headset):
 * ShowLensTest() shows the lens test in the headset until any button is pressed. Main thread.
 */
class HeadsetService : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.rifthome.RiftHome.Headset")

public:
    //! @p showLensTest acts on the compositor (any thread).
    explicit HeadsetService(std::function<void()> showLensTest, QObject *parent = nullptr);

    //! Puts the object on the session bus (before the service name is taken); false (and a warning) when it cannot.
    bool registerOnBus();

public Q_SLOTS:
    Q_SCRIPTABLE void ShowLensTest();

private:
    std::function<void()> m_showLensTest;
};

} // namespace RiftHome::App
