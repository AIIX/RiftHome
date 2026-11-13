// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QString>

#include <functional>

namespace RiftHome::App
{

/*!
 * What Settings asks of Rift Home about the controllers, on the session bus Rift Home already serves (service
 * org.rifthome.RiftHome, object /Controls, interface org.rifthome.RiftHome.Controls): Settings cannot see the buttons
 * (Rift Home takes them), so Rift Home records a binding when asked. RecordBinding() starts it; BindingRecorded(binding)
 * is the answer, the binding's settings name ("rightSecondary", "leftGrip+rightSecondary") or empty when cancelled or
 * after 10 s. Main thread.
 */
class ControlsService : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.rifthome.RiftHome.Controls")

public:
    //! @p start and @p cancel act on the compositor (any thread).
    ControlsService(std::function<void()> start, std::function<void()> cancel, QObject *parent = nullptr);

    //! Takes the name and the object on the session bus; false (and a warning) when it cannot.
    bool registerOnBus();
    //! The compositor's answer, from any thread: sent as the signal.
    void recorded(const QString &binding);

public Q_SLOTS:
    Q_SCRIPTABLE void RecordBinding();
    Q_SCRIPTABLE void CancelRecording();

Q_SIGNALS:
    Q_SCRIPTABLE void BindingRecorded(const QString &binding);

private:
    std::function<void()> m_start;
    std::function<void()> m_cancel;
};

} // namespace RiftHome::App
