// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QString>

#include <memory>

namespace RiftHome::Settings
{

/*!
 * Settings' own password prompt: a
 * polkit authentication agent (libpolkit-agent-1, part of polkit; PAM checks the password through polkit's helper)
 * registered for this process only. polkit asks the agent registered for the calling process before the session's, so
 * Settings' own requests (time zone, automatic time) prompt here, in the headset, with or without Plasma. Without
 * libpolkit-agent-1 at build time it is not available and the session's agent (if any) answers.
 */
class Authentication : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool pending READ pending NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString user READ user NOTIFY changed)
    Q_PROPERTY(QString prompt READ prompt NOTIFY changed)
    Q_PROPERTY(bool echo READ echo NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)

public:
    //! @p sessionWide: the agent of the whole login session (Rift Home's own session), not only Settings'.
    explicit Authentication(bool sessionWide = false, QObject *parent = nullptr);
    ~Authentication() override;

    bool available() const;
    bool pending() const
    {
        return m_pending;
    }
    QString message() const
    {
        return m_message;
    }
    QString user() const
    {
        return m_user;
    }
    QString prompt() const
    {
        return m_prompt;
    }
    bool echo() const
    {
        return m_echo;
    }
    QString error() const
    {
        return m_error;
    }

    //! The password (or other answer) for the prompt shown.
    Q_INVOKABLE void respond(const QString &answer);
    Q_INVOKABLE void cancel();

    struct Private;

Q_SIGNALS:
    void changed();

private:
    friend struct Private;
    std::unique_ptr<Private> d;
    bool m_pending = false;
    QString m_message;
    QString m_user;
    QString m_prompt;
    bool m_echo = false;
    QString m_error;
};

} // namespace RiftHome::Settings
