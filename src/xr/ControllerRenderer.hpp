// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "spatial/Math.hpp"
#include "xr/ControllerModel.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <vector>

namespace RiftHome::Xr
{

//! A controller to draw: which hand's model, where it is (OpenXR's grip pose).
struct DrawnController {
    bool left = false;
    Spatial::Pose grip;
};

//! One eye's view: its pose in the frame's space and its field of view (radians, OpenXR's angles).
struct EyeView {
    Spatial::Pose pose;
    float angleLeft = 0.0f;
    float angleRight = 0.0f;
    float angleUp = 0.0f;
    float angleDown = 0.0f;
};

/*!
 * The controllers in the room (Rift Home's own models, ControllerModel): drawn into the two eyes' images of a
 * projection layer of Rift Home's own, opaque and lit (a key light from above in front, a soft fill, the room's light, a
 * cool rim so dark controllers stand out from dark rooms), in linear light with highlights past white (an HDR layer), a
 * see-through background (premultiplied alpha), 4x multisampled, depth-tested. XR thread; the images are the layer's
 * swapchain images (COLOR_ATTACHMENT_OPTIMAL after).
 */
class ControllerRenderer
{
public:
    //! Pipeline, meshes and framebuffers for the eyes' swapchain @p images (@p format, @p width x @p height); false (logged): none.
    bool init(VkPhysicalDevice physical, VkDevice device, VkFormat format, uint32_t width, uint32_t height,
              const std::array<std::vector<VkImage>, 2> &images, const ControllerMesh &right, const ControllerMesh &left);
    void destroy();
    bool ready() const
    {
        return m_pipeline != VK_NULL_HANDLE;
    }
    //! Clears eye @p eye's image @p imageIndex and draws @p controllers seen from @p view.
    void record(VkCommandBuffer cmd, int eye, uint32_t imageIndex, const EyeView &view, const std::vector<DrawnController> &controllers);

private:
    struct Attachment {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    bool createAttachment(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, Attachment &out);
    bool createBuffer(VkBufferUsageFlags usage, const void *data, VkDeviceSize size, VkBuffer &buffer, VkDeviceMemory &memory);
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags wanted) const;

    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    std::array<Attachment, 2> m_colour; //!< per eye, multisampled
    std::array<Attachment, 2> m_depth; //!< per eye, multisampled
    std::array<std::vector<VkImageView>, 2> m_targetViews; //!< the swapchain images
    std::array<std::vector<VkFramebuffer>, 2> m_framebuffers;
    VkBuffer m_vertices = VK_NULL_HANDLE;
    VkDeviceMemory m_vertexMemory = VK_NULL_HANDLE;
    VkBuffer m_indices = VK_NULL_HANDLE;
    VkDeviceMemory m_indexMemory = VK_NULL_HANDLE;
    std::array<uint32_t, 2> m_firstIndex{}; //!< right, left
    std::array<uint32_t, 2> m_indexCount{};
    std::array<int32_t, 2> m_vertexOffset{};
};

} // namespace RiftHome::Xr
