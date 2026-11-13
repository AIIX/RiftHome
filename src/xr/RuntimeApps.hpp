// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace RiftHome::Xr
{

//! A VR app connected to the runtime (a libmonado client), as Home shows it.
struct VrApp {
    uint32_t id = 0;
    std::string name;
    bool primary = false; //!< the one the runtime shows
    bool visible = false;
    bool focused = false;
    bool overlay = false;
    bool operator==(const VrApp &) const = default;
};

/*!
 * The VR apps connected to RiftLinux's runtime besides Rift Home: read through
 * libmonado, loaded at run time (absent: no list, Rift Home works as before), on a thread of its own every second (its IPC calls
 * never wait in the headset's frame). makePrimary() brings one in front: the runtime shows it, Rift Home waits SYNCHRONIZED until the user switches back (the Rift panel's Switch
 * application) or the app ends (the runtime then shows the one before).
 */
class RuntimeApps
{
public:
    //! @p ownName: the application name to leave out (Rift Home's own); empty: every app.
    explicit RuntimeApps(std::string ownName);
    ~RuntimeApps();
    RuntimeApps(const RuntimeApps &) = delete;
    RuntimeApps &operator=(const RuntimeApps &) = delete;

    void start();
    void stop();
    //! libmonado loaded and the runtime reached.
    bool connected() const;
    //! The others with an active session, in the runtime's order.
    std::vector<VrApp> apps() const;
    //! Asks the runtime to show @p id (done on the thread; logged).
    void makePrimary(uint32_t id);
    //! The runtime can take a screenshot of the view as the headset shows it (libmonado's mnd_root_take_screenshot).
    bool canTakeScreenshot() const;
    /*!
     * The runtime's screenshot of the view (every layer composited, frosted glass, its HDR mapping), written to
     * @p pngPath (its folder must exist), taken on the thread; waits up to @p timeout. False when the runtime cannot
     * (an older one, no frame) or it took too long: then nothing is written there later either.
     */
    bool takeScreenshot(const std::string &pngPath, std::chrono::milliseconds timeout);
    /*!
     * The runtime draws the lens test's cross onto each eye's panel at its lens centre, pixel for pixel as Oculus does
     * (libmonado's mnd_root_set_lens_pattern, API 1.10; CV1 only): true while it can, false once it refused.
     */
    bool canDrawLensPattern() const;
    //! Turns the runtime's lens test cross on or off (done on the thread; logged). Off by itself when Rift Home goes.
    void setLensPattern(bool on);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace RiftHome::Xr
