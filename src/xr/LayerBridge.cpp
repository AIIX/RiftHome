// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "xr/LayerBridge.hpp"

#include <unistd.h>
#include <utility>

namespace RiftHome::Xr
{

BufferHandoff::~BufferHandoff()
{
    for (const DmabufPlane &plane : planes) {
        if (plane.fd >= 0) {
            ::close(plane.fd);
        }
    }
    if (acquireFence >= 0) {
        ::close(acquireFence);
    }
}

LayerBridge::LayerBridge(std::function<void(uint64_t token)> release)
    : m_release(std::move(release))
{
}

LayerBridge::~LayerBridge()
{
    for (auto &[id, buffer] : m_pending) {
        if (buffer->releaseToken) {
            m_release(buffer->releaseToken);
        }
    }
}

void LayerBridge::publishBuffer(uint64_t surfaceId, std::unique_ptr<BufferHandoff> buffer)
{
    std::unique_ptr<BufferHandoff> replaced;
    {
        std::lock_guard lock(m_mutex);
        if (buffer->newBuffer && m_importRequest) {
            m_importRequest(*buffer);
        }
        std::unique_ptr<BufferHandoff> &slot = m_pending[surfaceId];
        replaced = std::move(slot);
        slot = std::move(buffer);
    }
    if (replaced) {
        release(std::move(replaced));
    }
}

void LayerBridge::publishScene(std::vector<WindowPlacement> windows)
{
    std::lock_guard lock(m_mutex);
    m_windows = std::move(windows);
}

void LayerBridge::surfaceDestroyed(uint64_t surfaceId)
{
    std::unique_ptr<BufferHandoff> dropped;
    {
        std::lock_guard lock(m_mutex);
        if (auto it = m_pending.find(surfaceId); it != m_pending.end()) {
            dropped = std::move(it->second);
            m_pending.erase(it);
        }
        m_destroyed.push_back(surfaceId);
        m_dead.insert(surfaceId);
    }
    if (dropped) {
        release(std::move(dropped));
    }
}

void LayerBridge::bufferDestroyed(uint64_t bufferId)
{
    std::lock_guard lock(m_mutex);
    if (m_dropAllImports) {
        return;
    }
    if (m_buffersGone.size() >= kMaxBuffersGone) {
        // Frames not taken for a long time (the session stopped): the XR side drops every import instead (bounded).
        m_buffersGone.clear();
        m_dropAllImports = true;
        return;
    }
    m_buffersGone.push_back(bufferId);
}

LayerBridge::Frame LayerBridge::take()
{
    std::lock_guard lock(m_mutex);
    Frame frame;
    frame.windows = m_windows;
    frame.buffers = std::move(m_pending);
    m_pending.clear();
    frame.destroyed = std::move(m_destroyed);
    m_destroyed.clear();
    frame.buffersGone = std::move(m_buffersGone);
    m_buffersGone.clear();
    frame.dropAllImports = std::exchange(m_dropAllImports, false);
    // The XR side puts back only buffers it took before this, so once told, a destroyed surface needs no guard.
    for (uint64_t id : frame.destroyed) {
        m_dead.erase(id);
    }
    return frame;
}

void LayerBridge::putBack(uint64_t surfaceId, std::unique_ptr<BufferHandoff> buffer)
{
    {
        std::lock_guard lock(m_mutex);
        if (!m_dead.contains(surfaceId)) {
            std::unique_ptr<BufferHandoff> &slot = m_pending[surfaceId];
            if (!slot) {
                slot = std::move(buffer);
                return;
            }
        }
    }
    release(std::move(buffer)); // a newer buffer came meanwhile, or the surface is gone
}

void LayerBridge::release(std::unique_ptr<BufferHandoff> buffer)
{
    if (buffer && buffer->releaseToken) {
        m_release(buffer->releaseToken);
    }
}

} // namespace RiftHome::Xr
