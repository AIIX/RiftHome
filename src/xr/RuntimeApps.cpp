// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "xr/RuntimeApps.hpp"

#include <QLoggingCategory>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <dlfcn.h>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

Q_DECLARE_LOGGING_CATEGORY(lcXr)

namespace RiftHome::Xr
{

namespace
{

// libmonado's C API (RiftLinux: /usr/lib/riftlinux/native/monado.h, API 1.8), declared here: it is loaded at run time.
struct MndRoot;
using MndResult = int; // MND_SUCCESS 0, MND_ERROR_CONNECTING_FAILED -3
constexpr uint32_t kPrimaryApp = 1u << 0u;
constexpr uint32_t kSessionActive = 1u << 1u;
constexpr uint32_t kSessionVisible = 1u << 2u;
constexpr uint32_t kSessionFocused = 1u << 3u;
constexpr uint32_t kSessionOverlay = 1u << 4u;

struct Api {
    void (*getVersion)(uint32_t *, uint32_t *, uint32_t *) = nullptr;
    MndResult (*create)(MndRoot **) = nullptr;
    void (*destroy)(MndRoot **) = nullptr;
    MndResult (*updateClientList)(MndRoot *) = nullptr;
    MndResult (*numberOfClients)(MndRoot *, uint32_t *) = nullptr;
    MndResult (*clientIdAt)(MndRoot *, uint32_t, uint32_t *) = nullptr;
    MndResult (*clientName)(MndRoot *, uint32_t, const char **) = nullptr;
    MndResult (*clientState)(MndRoot *, uint32_t, uint32_t *) = nullptr;
    MndResult (*setPrimary)(MndRoot *, uint32_t) = nullptr;
    MndResult (*takeScreenshot)(MndRoot *, const char *, uint32_t) = nullptr; //!< API 1.9 on; optional
    MndResult (*setLensPattern)(MndRoot *, bool) = nullptr; //!< API 1.10 on; optional
};

//! A screenshot asked of the runtime: taken on the thread unless the caller gave up waiting meanwhile.
struct Shot {
    std::string path;
    std::promise<bool> done;
    std::atomic<bool> cancelled = false;
};

QString describe(const std::vector<VrApp> &apps)
{
    QStringList parts;
    for (const VrApp &app : apps) {
        parts << QString::fromStdString(app.name) + (app.primary ? QStringLiteral(" (shown)") : QString());
    }
    return parts.isEmpty() ? QStringLiteral("none besides Rift Home") : parts.join(QStringLiteral(", "));
}

} // namespace

struct RuntimeApps::Impl {
    std::string ownName;
    void *library = nullptr;
    Api api;
    MndRoot *root = nullptr;
    std::thread thread;
    mutable std::mutex mutex;
    std::condition_variable wake;
    bool running = false;
    bool connected = false;
    std::vector<VrApp> apps;
    std::deque<uint32_t> requests;
    std::deque<std::shared_ptr<Shot>> shots;
    std::optional<bool> lensPattern; //!< the lens test's cross asked on or off, not yet told the runtime
    bool lensPatternRefused = false; //!< the runtime cannot draw it (not a CV1)
    bool loadWarned = false;
    bool connectWarned = false;
    int failures = 0; //!< connection attempts failed in a row (libmonado prints a banner at each: tried less often)

    bool load()
    {
        if (library) {
            return true;
        }
        const char *chosen = std::getenv("RIFT_HOME_LIBMONADO");
        for (const char *path : {chosen, "/usr/lib/riftlinux/native/libmonado.so.25", "libmonado.so.25"}) {
            if (path && *path && (library = dlopen(path, RTLD_NOW | RTLD_LOCAL))) {
                break;
            }
        }
        if (!library) {
            if (!loadWarned) {
                loadWarned = true;
                qCInfo(lcXr) << "VR apps: no libmonado (an older RiftLinux): Home lists no other VR apps";
            }
            return false;
        }
        const auto symbol = [this](const char *name) {
            return dlsym(library, name);
        };
        api.getVersion = reinterpret_cast<decltype(api.getVersion)>(symbol("mnd_api_get_version"));
        api.create = reinterpret_cast<decltype(api.create)>(symbol("mnd_root_create"));
        api.destroy = reinterpret_cast<decltype(api.destroy)>(symbol("mnd_root_destroy"));
        api.updateClientList = reinterpret_cast<decltype(api.updateClientList)>(symbol("mnd_root_update_client_list"));
        api.numberOfClients = reinterpret_cast<decltype(api.numberOfClients)>(symbol("mnd_root_get_number_clients"));
        api.clientIdAt = reinterpret_cast<decltype(api.clientIdAt)>(symbol("mnd_root_get_client_id_at_index"));
        api.clientName = reinterpret_cast<decltype(api.clientName)>(symbol("mnd_root_get_client_name"));
        api.clientState = reinterpret_cast<decltype(api.clientState)>(symbol("mnd_root_get_client_state"));
        api.setPrimary = reinterpret_cast<decltype(api.setPrimary)>(symbol("mnd_root_set_client_primary"));
        api.takeScreenshot = reinterpret_cast<decltype(api.takeScreenshot)>(symbol("mnd_root_take_screenshot"));
        api.setLensPattern = reinterpret_cast<decltype(api.setLensPattern)>(symbol("mnd_root_set_lens_pattern"));
        if (!api.create || !api.destroy || !api.updateClientList || !api.numberOfClients || !api.clientIdAt || !api.clientName
            || !api.clientState || !api.setPrimary) {
            qCWarning(lcXr) << "VR apps: libmonado lacks the calls needed: Home lists no other VR apps";
            dlclose(library);
            library = nullptr;
            loadWarned = true;
            return false;
        }
        uint32_t major = 0, minor = 0, patch = 0;
        if (api.getVersion) {
            api.getVersion(&major, &minor, &patch);
        }
        qCInfo(lcXr).noquote() << QStringLiteral("VR apps: libmonado %1.%2.%3 loaded%4%5").arg(major).arg(minor).arg(patch)
                                      .arg(api.takeScreenshot ? QStringLiteral(" (the runtime takes screenshots)") : QString())
                                      .arg(api.setLensPattern ? QStringLiteral(" (and draws the lens test's cross)") : QString());
        return true;
    }

    //! One look at the runtime's clients (the thread), and the requests waiting.
    void poll()
    {
        if (!load()) {
            return;
        }
        if (!root) {
            if (api.create(&root) != 0 || !root) {
                root = nullptr;
                ++failures;
                if (!connectWarned) {
                    connectWarned = true;
                    qCInfo(lcXr) << "VR apps: the runtime is not reachable through libmonado yet";
                }
                setConnected(false);
                return;
            }
            connectWarned = false;
            failures = 0;
            qCInfo(lcXr) << "VR apps: connected to the runtime";
        }
        if (api.updateClientList(root) != 0) {
            qCInfo(lcXr) << "VR apps: the runtime went away";
            api.destroy(&root);
            root = nullptr;
            setConnected(false);
            return;
        }
        std::vector<VrApp> found;
        uint32_t count = 0;
        api.numberOfClients(root, &count);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t id = 0;
            const char *name = nullptr;
            uint32_t flags = 0;
            if (api.clientIdAt(root, i, &id) != 0 || api.clientName(root, id, &name) != 0 || api.clientState(root, id, &flags) != 0) {
                continue;
            }
            const std::string appName = name ? name : "";
            // Its own session only; libmonado's own connections (this one) are not apps.
            if (!(flags & kSessionActive) || appName.rfind("libmonado", 0) == 0 || (!ownName.empty() && appName == ownName)) {
                continue;
            }
            found.push_back({id, appName.empty() ? std::string("VR app") : appName, bool(flags & kPrimaryApp), bool(flags & kSessionVisible),
                             bool(flags & kSessionFocused), bool(flags & kSessionOverlay)});
        }
        std::deque<uint32_t> asked;
        {
            std::lock_guard lock(mutex);
            asked.swap(requests);
            if (found != apps) {
                qCInfo(lcXr).noquote() << "VR apps:" << describe(found);
            }
            apps = found;
            connected = true;
        }
        for (uint32_t id : asked) {
            const MndResult result = api.setPrimary(root, id);
            qCInfo(lcXr).noquote() << QStringLiteral("VR apps: client %1 brought in front%2").arg(id).arg(result == 0 ? QString() : QStringLiteral(" FAILED (%1)").arg(result));
        }
        takeShots();
        tellLensPattern();
    }

    //! The lens test's cross as last asked, told the runtime.
    void tellLensPattern()
    {
        std::optional<bool> asked;
        {
            std::lock_guard lock(mutex);
            asked.swap(lensPattern);
        }
        if (!asked || !api.setLensPattern || !root) {
            return;
        }
        const MndResult result = api.setLensPattern(root, *asked);
        qCInfo(lcXr).noquote() << QStringLiteral("lens test: the runtime's cross %1%2").arg(*asked ? QStringLiteral("on") : QStringLiteral("off"))
                                      .arg(result == 0 ? QString() : QStringLiteral(" FAILED (%1): Rift Home draws its own").arg(result));
        if (result != 0 && *asked) {
            std::lock_guard lock(mutex);
            lensPatternRefused = true;
        }
    }

    //! The screenshots asked for (the runtime renders one from its next frame and returns once the file is written).
    void takeShots()
    {
        std::deque<std::shared_ptr<Shot>> asked;
        {
            std::lock_guard lock(mutex);
            asked.swap(shots);
        }
        for (const std::shared_ptr<Shot> &shot : asked) {
            if (shot->cancelled) {
                shot->done.set_value(false);
                continue;
            }
            const MndResult result = api.takeScreenshot && root ? api.takeScreenshot(root, shot->path.c_str(), 0) : -1;
            qCInfo(lcXr).noquote() << "screenshot: the runtime's" << QString::fromStdString(shot->path)
                                   << (result == 0 ? QStringLiteral("written") : QStringLiteral("FAILED (%1)").arg(result));
            shot->done.set_value(result == 0);
        }
    }

    //! Screenshots still waiting when the thread stops: not taken.
    void dropShots()
    {
        std::lock_guard lock(mutex);
        for (const std::shared_ptr<Shot> &shot : shots) {
            shot->done.set_value(false);
        }
        shots.clear();
    }

    void setConnected(bool on)
    {
        std::lock_guard lock(mutex);
        connected = on;
        if (!on) {
            apps.clear();
        }
    }

    void loop()
    {
        std::unique_lock lock(mutex);
        while (running) {
            lock.unlock();
            poll();
            lock.lock();
            // Every second, at once for a request; less often while no runtime answers (3, 6, 12, then every 24 s).
            const auto pause = connected || !library ? std::chrono::seconds(1) : std::chrono::seconds(3 << std::min(failures, 3));
            wake.wait_for(lock, pause, [this] {
                return !running || !requests.empty() || !shots.empty() || lensPattern.has_value();
            });
        }
    }
};

RuntimeApps::RuntimeApps(std::string ownName)
    : d(std::make_unique<Impl>())
{
    d->ownName = std::move(ownName);
}

RuntimeApps::~RuntimeApps()
{
    stop();
    if (d->root && d->api.destroy) {
        d->api.destroy(&d->root);
    }
    if (d->library) {
        dlclose(d->library);
    }
}

void RuntimeApps::start()
{
    if (d->thread.joinable()) {
        return;
    }
    {
        std::lock_guard lock(d->mutex);
        d->running = true;
    }
    d->thread = std::thread([this] {
        d->loop();
    });
}

void RuntimeApps::stop()
{
    if (!d->thread.joinable()) {
        return;
    }
    {
        std::lock_guard lock(d->mutex);
        d->running = false;
    }
    d->wake.notify_all();
    d->thread.join();
    d->dropShots();
}

bool RuntimeApps::connected() const
{
    std::lock_guard lock(d->mutex);
    return d->connected;
}

std::vector<VrApp> RuntimeApps::apps() const
{
    std::lock_guard lock(d->mutex);
    return d->apps;
}

void RuntimeApps::makePrimary(uint32_t id)
{
    {
        std::lock_guard lock(d->mutex);
        d->requests.push_back(id);
    }
    d->wake.notify_all();
}

bool RuntimeApps::canTakeScreenshot() const
{
    std::lock_guard lock(d->mutex);
    return d->connected && d->api.takeScreenshot;
}

bool RuntimeApps::canDrawLensPattern() const
{
    std::lock_guard lock(d->mutex);
    return d->connected && d->api.setLensPattern && !d->lensPatternRefused;
}

void RuntimeApps::setLensPattern(bool on)
{
    {
        std::lock_guard lock(d->mutex);
        d->lensPattern = on;
    }
    d->wake.notify_all();
}

bool RuntimeApps::takeScreenshot(const std::string &pngPath, std::chrono::milliseconds timeout)
{
    auto shot = std::make_shared<Shot>();
    shot->path = pngPath;
    std::future<bool> done = shot->done.get_future();
    {
        std::lock_guard lock(d->mutex);
        if (!d->running || !d->connected || !d->api.takeScreenshot) {
            return false;
        }
        d->shots.push_back(shot);
    }
    d->wake.notify_all();
    if (done.wait_for(timeout) != std::future_status::ready) {
        shot->cancelled = true; // not started yet: never; started: its file is the caller's to ignore
        return false;
    }
    return done.get();
}

} // namespace RiftHome::Xr
