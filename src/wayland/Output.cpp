// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "wayland/Output.hpp"

#include "wayland/Logging.hpp"
#include "wayland/Server.hpp"
#include "wayland/wlr.hpp"

#include <QPointF>

#include <cmath>

namespace RiftHome::Wayland
{

namespace
{

//! The output's first state: on, its mode, and (headless, PQ preferred) an HDR description if wlroots accepts it.
//! @p scale: a headless output's (the apps' scale, or 1 where they are told surface by surface).
void buildState(wlr_output_state *state, wlr_output *output, const Server::Options &options, double scale, bool hdr)
{
    wlr_output_state_init(state);
    wlr_output_state_set_enabled(state, true);
    if (wlr_output_is_headless(output)) {
        wlr_output_state_set_custom_mode(state, options.width, options.height, options.refreshMilliHz);
        wlr_output_state_set_scale(state, float(scale)); // apps draw at it (HiDPI): their windows' pixels
    } else if (wlr_output_is_drm(output)) {
        if (wlr_output_mode *mode = wlr_output_preferred_mode(output)) {
            wlr_output_state_set_mode(state, mode); // a real monitor: its own best mode
        }
    } else {
        wlr_output_state_set_custom_mode(state, 1600, 900, 0); // the nested window
    }
    if (hdr) {
        wlr_output_image_description description{};
        description.primaries = WLR_COLOR_NAMED_PRIMARIES_BT2020;
        description.transfer_function = WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ;
        wlr_output_state_set_image_description(state, &description);
    }
}

} // namespace

Output::Output(Server &server, wlr_output *output)
    : m_server(server)
    , m_output(output)
{
    wlr_output_init_render(output, server.allocator, server.renderer);

    const bool headless = wlr_output_is_headless(output);
    bool hdr = headless && server.options().preferred == Server::Preferred::Pq;
    wlr_output_state state;
    const double scale = server.scalesPerSurface() ? 1.0 : server.options().appScale;
    buildState(&state, output, server.options(), scale, hdr);
    if (hdr && !wlr_output_test_state(output, &state)) {
        wlr_output_state_finish(&state);
        hdr = false;
        buildState(&state, output, server.options(), scale, false);
    }
    const bool committed = wlr_output_commit_state(output, &state);
    wlr_output_state_finish(&state);

    m_frame.connect(&output->events.frame, [this](void *) {
        wlr_scene_output_commit(m_sceneOutput, nullptr);
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        wlr_scene_output_send_frame_done(m_sceneOutput, &now);
        if (!m_server.isDrm()) { // in its own session the server keeps the pace (a monitor's page is still)
            m_server.sendFrameDone(now);
            m_server.publishScene();
        }
    });
    m_requestState.connect(&output->events.request_state, [this](void *data) {
        // The nested window was resized or closed by the desktop.
        wlr_output_commit_state(m_output, static_cast<wlr_output_event_request_state *>(data)->state);
    });
    m_destroy.connect(&output->events.destroy, [this](void *) {
        m_server.outputDestroyed(this);
        delete this;
    });

    // Headless and nested: at the origin, where the windows' flat view is, also with the X11 screen placed there (an
    // output placed by hand pushes automatic ones to its right, and this one, which paces the server's frames, then saw
    // no window and stopped asking for frames, so new windows never reached the headset). Monitors (its own
    // session, paced by the server's clock) side by side.
    wlr_output_layout_output *layoutOutput = server.isDrm() ? wlr_output_layout_add_auto(server.outputLayout, output)
                                                            : wlr_output_layout_add(server.outputLayout, output, 0, 0);
    m_sceneOutput = wlr_scene_output_create(server.scene, output);
    wlr_scene_output_layout_add_output(server.sceneLayout, layoutOutput, m_sceneOutput);

    qCInfo(lcWayland).noquote() << "output" << output->name << QStringLiteral("%1x%2 @ %3 Hz").arg(output->width).arg(output->height).arg(output->refresh / 1000.0)
                                << QStringLiteral("| apps at %1 %").arg(qRound(output->scale * 100.0f)) << "| image description" << (hdr ? "PQ BT.2020" : "default (sRGB)")
                                << (committed ? "" : "| COMMIT FAILED");
}

Output::~Output()
{
    if (m_page) {
        m_pageDestroy.disconnect();
        wlr_scene_node_destroy(&m_page->node);
    }
}

void Output::showStatus(const StatusState &state)
{
    if (!m_server.isDrm() || !m_sceneOutput) {
        return;
    }
    int width = 0;
    int height = 0;
    wlr_output_effective_resolution(m_output, &width, &height);
    if (width <= 0 || height <= 0 || (m_shown == state && width == m_pageWidth && height == m_pageHeight)) {
        return;
    }
    if (!m_page) {
        m_page = wlr_scene_buffer_create(m_server.monitorTree(), nullptr);
        wlr_scene_node_lower_to_bottom(&m_page->node); // the monitor's windows (its terminal) above it
        m_pageDestroy.connect(&m_page->node.events.destroy, [this](void *) {
            m_pageDestroy.disconnect();
            m_page = nullptr;
        });
    }
    wlr_box box{};
    wlr_output_layout_get_box(m_server.outputLayout, m_output, &box);
    wlr_scene_node_set_position(&m_page->node, box.x, box.y);
    wlr_buffer *buffer = bufferFromImage(drawStatusPage(state, width, height));
    wlr_scene_buffer_set_buffer(m_page, buffer);
    wlr_buffer_drop(buffer);
    m_shown = state;
    m_pageWidth = width;
    m_pageHeight = height;
}

Output::PageButton Output::pageButtonAt(double lx, double ly) const
{
    if (!m_page) {
        return PageButton::None;
    }
    wlr_box box{};
    wlr_output_layout_get_box(m_server.outputLayout, m_output, &box);
    const QPointF p(lx - box.x, ly - box.y);
    if (leaveButton(m_pageWidth, m_pageHeight).contains(p)) {
        return PageButton::Leave;
    }
    if (terminalButton(m_pageWidth, m_pageHeight).contains(p)) {
        return PageButton::Terminal;
    }
    return PageButton::None;
}

void Output::setScale(double scale)
{
    if (!wlr_output_is_headless(m_output) || std::abs(m_output->scale - float(scale)) < 0.001f) {
        return;
    }
    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_scale(&state, float(scale));
    if (!wlr_output_commit_state(m_output, &state)) {
        qCWarning(lcWayland).noquote() << "output" << m_output->name << "did not take the apps' scale" << scale;
    }
    wlr_output_state_finish(&state);
}

} // namespace RiftHome::Wayland
