// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "settings/Authentication.hpp"

#include <QLoggingCategory>
#include <QRegularExpression>

Q_LOGGING_CATEGORY(lcAuth, "rifthome.settings.auth")

#ifdef RIFT_HOME_POLKIT

#define POLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE
// GLib's headers name a field "signals", which is a Qt keyword macro.
#pragma push_macro("signals")
#undef signals
#include <polkitagent/polkitagent.h>
#pragma pop_macro("signals")

#include <pwd.h>
#include <unistd.h>

namespace RiftHome::Settings
{

struct Authentication::Private {
    Authentication *q = nullptr;
    PolkitAgentListener *listener = nullptr;
    gpointer registration = nullptr;
    GTask *task = nullptr; //!< the request being answered
    PolkitIdentity *identity = nullptr;
    QString cookie;
    PolkitAgentSession *session = nullptr;
    bool cancelled = false;
    bool answered = false;

    void begin(GTask *request, const gchar *message, const gchar *cookie, GList *identities, GCancellable *cancellable);
    void startSession();
    void endSession();
    void finish(bool gained);
};

namespace
{

Authentication::Private *g_agent = nullptr; //!< the one agent in this process (the listener's callbacks find it here)

} // namespace

} // namespace RiftHome::Settings

// The listener: a PolkitAgentListener subclass whose requests go to the Authentication object.
extern "C" {
typedef struct {
    PolkitAgentListener parent;
} RiftListener;
typedef struct {
    PolkitAgentListenerClass parent;
} RiftListenerClass;
G_DEFINE_TYPE(RiftListener, rift_listener, POLKIT_AGENT_TYPE_LISTENER)

static void rift_listener_initiate(PolkitAgentListener *listener, const gchar *, const gchar *message, const gchar *, PolkitDetails *,
                                   const gchar *cookie, GList *identities, GCancellable *cancellable, GAsyncReadyCallback callback,
                                   gpointer userData)
{
    using RiftHome::Settings::g_agent;
    GTask *task = g_task_new(listener, cancellable, callback, userData);
    if (!g_agent || g_agent->task) {
        g_task_return_new_error(task, POLKIT_ERROR, POLKIT_ERROR_FAILED, "another authentication is in progress");
        g_object_unref(task);
        return;
    }
    g_agent->begin(task, message, cookie, identities, cancellable);
}

static gboolean rift_listener_initiate_finish(PolkitAgentListener *, GAsyncResult *result, GError **error)
{
    return g_task_propagate_boolean(G_TASK(result), error);
}

static void rift_listener_init(RiftListener *)
{
}

static void rift_listener_class_init(RiftListenerClass *klass)
{
    PolkitAgentListenerClass *listener = POLKIT_AGENT_LISTENER_CLASS(klass);
    listener->initiate_authentication = rift_listener_initiate;
    listener->initiate_authentication_finish = rift_listener_initiate_finish;
}
}

namespace RiftHome::Settings
{

void Authentication::Private::begin(GTask *request, const gchar *message, const gchar *cookieText, GList *identities, GCancellable *cancellable)
{
    // Who answers: this user if polkit accepts them, else the first one it names (an administrator).
    PolkitIdentity *chosen = nullptr;
    for (GList *l = identities; l; l = l->next) {
        auto *candidate = POLKIT_IDENTITY(l->data);
        if (POLKIT_IS_UNIX_USER(candidate) && polkit_unix_user_get_uid(POLKIT_UNIX_USER(candidate)) == gint(getuid())) {
            chosen = candidate;
            break;
        }
    }
    if (!chosen && identities) {
        chosen = POLKIT_IDENTITY(identities->data);
    }
    if (!chosen) {
        g_task_return_new_error(request, POLKIT_ERROR, POLKIT_ERROR_FAILED, "nobody to authenticate as");
        g_object_unref(request);
        return;
    }
    task = request;
    identity = POLKIT_IDENTITY(g_object_ref(chosen));
    cookie = QString::fromUtf8(cookieText);
    cancelled = false;
    q->m_message = QString::fromUtf8(message);
    q->m_user.clear();
    if (POLKIT_IS_UNIX_USER(chosen)) {
        if (const passwd *pw = getpwuid(uid_t(polkit_unix_user_get_uid(POLKIT_UNIX_USER(chosen))))) {
            q->m_user = QString::fromLocal8Bit(pw->pw_name);
        }
    }
    q->m_error.clear();
    q->m_prompt.clear();
    q->m_pending = true;
    Q_EMIT q->changed();
    qCInfo(lcAuth).noquote() << "password asked for:" << q->m_message;
    if (cancellable) {
        g_cancellable_connect(cancellable, G_CALLBACK(+[](GCancellable *, gpointer) {
                                  if (g_agent) {
                                      g_agent->q->cancel();
                                  }
                              }),
                              nullptr, nullptr);
    }
    startSession();
}

void Authentication::Private::startSession()
{
    endSession();
    answered = false;
    session = polkit_agent_session_new(identity, cookie.toUtf8().constData());
    g_signal_connect(session, "request", G_CALLBACK(+[](PolkitAgentSession *, const gchar *request, gboolean echoOn, gpointer) {
                         if (g_agent) {
                             // PAM's prompt ("Password: "), shown without its colon.
                             g_agent->q->m_prompt = QString::fromUtf8(request).trimmed().remove(QRegularExpression(QStringLiteral(":$")));
                             g_agent->q->m_echo = echoOn;
                             Q_EMIT g_agent->q->changed();
                         }
                     }),
                     nullptr);
    g_signal_connect(session, "show-error", G_CALLBACK(+[](PolkitAgentSession *, const gchar *text, gpointer) {
                         if (g_agent) {
                             g_agent->q->m_error = QString::fromUtf8(text);
                             Q_EMIT g_agent->q->changed();
                         }
                     }),
                     nullptr);
    g_signal_connect(session, "completed", G_CALLBACK(+[](PolkitAgentSession *, gboolean gained, gpointer) {
                         if (!g_agent) {
                             return;
                         }
                         if (gained || g_agent->cancelled) {
                             g_agent->finish(gained);
                         } else {
                             // Not accepted: say so and ask again (as other desktops' prompts do).
                             g_agent->q->m_error = g_agent->answered ? QStringLiteral("That password was not accepted. Try again.") : g_agent->q->m_error;
                             Q_EMIT g_agent->q->changed();
                             g_agent->startSession();
                         }
                     }),
                     nullptr);
    polkit_agent_session_initiate(session);
}

void Authentication::Private::endSession()
{
    if (session) {
        g_signal_handlers_disconnect_matched(session, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, nullptr);
        g_object_unref(session);
        session = nullptr;
    }
}

void Authentication::Private::finish(bool gained)
{
    endSession();
    if (task) {
        if (gained) {
            g_task_return_boolean(task, TRUE);
        } else {
            g_task_return_new_error(task, POLKIT_ERROR, POLKIT_ERROR_CANCELLED, "cancelled");
        }
        g_object_unref(task);
        task = nullptr;
    }
    if (identity) {
        g_object_unref(identity);
        identity = nullptr;
    }
    qCInfo(lcAuth) << (gained ? "authenticated" : "authentication cancelled");
    q->m_pending = false;
    q->m_prompt.clear();
    Q_EMIT q->changed();
}

Authentication::Authentication(bool sessionWide, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>())
{
    d->q = this;
    g_agent = d.get();
    // Registered for this process (polkit asks it before the session's agent for this process's requests), or, in Rift
    // Home's own session where no desktop brings one, for the whole session: every app's password prompts show here.
    PolkitSubject *subject = nullptr;
    if (sessionWide) {
        GError *sessionError = nullptr;
        subject = polkit_unix_session_new_for_process_sync(getpid(), nullptr, &sessionError);
        if (!subject) {
            qCWarning(lcAuth) << "no login session for the session-wide agent:" << (sessionError ? sessionError->message : "?");
            g_clear_error(&sessionError);
        }
    }
    if (!subject) {
        subject = polkit_unix_process_new_for_owner(getpid(), 0, getuid());
    }
    d->listener = POLKIT_AGENT_LISTENER(g_object_new(rift_listener_get_type(), nullptr));
    GError *error = nullptr;
    d->registration = polkit_agent_listener_register(d->listener, POLKIT_AGENT_REGISTER_FLAGS_NONE, subject,
                                                     "/org/rifthome/PolicyKit1/AuthenticationAgent", nullptr, &error);
    g_object_unref(subject);
    if (d->registration) {
        qCInfo(lcAuth) << (sessionWide ? "polkit agent registered for the whole session (every app's password prompts show here)"
                                       : "polkit agent registered for Settings (its password prompts show here)");
    } else {
        qCWarning(lcAuth) << "polkit agent not registered:" << (error ? error->message : "unknown");
        if (error) {
            g_error_free(error);
        }
    }
}

Authentication::~Authentication()
{
    if (d->task) {
        d->cancelled = true;
        d->finish(false);
    }
    if (d->registration) {
        polkit_agent_listener_unregister(d->registration);
    }
    if (d->listener) {
        g_object_unref(d->listener);
    }
    g_agent = nullptr;
}

bool Authentication::available() const
{
    return d->registration != nullptr;
}

void Authentication::respond(const QString &answer)
{
    if (d->session) {
        d->answered = true;
        m_error.clear();
        Q_EMIT changed();
        polkit_agent_session_response(d->session, answer.toUtf8().constData());
    }
}

void Authentication::cancel()
{
    if (!d->task) {
        return;
    }
    d->cancelled = true;
    if (d->session) {
        polkit_agent_session_cancel(d->session); // "completed" (not gained) follows
    } else {
        d->finish(false);
    }
}

} // namespace RiftHome::Settings

#else // no libpolkit-agent-1: the session's agent (if any) answers

namespace RiftHome::Settings
{

struct Authentication::Private {
};

Authentication::Authentication(bool sessionWide, QObject *parent)
    : QObject(parent)
{
    Q_UNUSED(sessionWide);
    qCInfo(lcAuth) << "built without libpolkit-agent-1: password prompts come from the session's agent";
}

Authentication::~Authentication() = default;

bool Authentication::available() const
{
    return false;
}

void Authentication::respond(const QString &)
{
}

void Authentication::cancel()
{
}

} // namespace RiftHome::Settings

#endif
