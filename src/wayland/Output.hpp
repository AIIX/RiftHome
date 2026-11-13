// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "wayland/Listener.hpp"
#include "wayland/StatusPage.hpp"

#include <optional>

struct wlr_output;
struct wlr_scene_output;
struct wlr_scene_buffer;

namespace RiftHome::Wayland
{

class Server;

/*!
 * A wlroots output: the headless one (frame clock at the headset's 90 Hz) or the nested window on the desktop
 * (flat view). Deletes itself when the output is destroyed.
 */
class Output
{
public:
    Output(Server &server, wlr_output *output);
    ~Output();
    Output(const Output &) = delete;
    Output &operator=(const Output &) = delete;

    //! Its own session: the monitor's page, drawn again when what it says changes.
    void showStatus(const StatusState &state);
    enum class PageButton {
        None,
        Leave,
        Terminal,
    };
    //! The page's button at layout coordinates.
    PageButton pageButtonAt(double lx, double ly) const;
    wlr_output *output() const
    {
        return m_output;
    }
    //! The apps' scale (Server::Options::appScale) on Rift Home's own output: not on a monitor of its own session.
    void setScale(double scale);

private:
    Server &m_server;
    wlr_output *m_output;
    wlr_scene_output *m_sceneOutput = nullptr;
    wlr_scene_buffer *m_page = nullptr;
    Listener m_pageDestroy; //!< the scene may go first at shutdown
    std::optional<StatusState> m_shown;
    int m_pageWidth = 0;
    int m_pageHeight = 0;
    Listener m_frame;
    Listener m_requestState;
    Listener m_destroy;
};

} // namespace RiftHome::Wayland
