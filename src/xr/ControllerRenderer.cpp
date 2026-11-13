// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "xr/ControllerRenderer.hpp"


#include <QLoggingCategory>

#include <cmath>
#include <cstring>

Q_DECLARE_LOGGING_CATEGORY(lcXr)

namespace RiftHome::Xr
{

namespace
{

const uint32_t kVertexShader[] =
#include "controller_vert.spv.h"
    ;
const uint32_t kFragmentShader[] =
#include "controller_frag.spv.h"
    ;

constexpr VkSampleCountFlagBits kSamples = VK_SAMPLE_COUNT_4_BIT;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr float kNear = 0.05f;
constexpr float kFar = 20.0f;

//! Column-major 4x4 (GLSL's mat4).
struct Mat4 {
    float m[16] = {};
    static Mat4 identity()
    {
        Mat4 r;
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }
    Mat4 operator*(const Mat4 &o) const
    {
        Mat4 r;
        for (int c = 0; c < 4; ++c) {
            for (int row = 0; row < 4; ++row) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    sum += m[k * 4 + row] * o.m[c * 4 + k];
                }
                r.m[c * 4 + row] = sum;
            }
        }
        return r;
    }
};

Mat4 fromPose(const Spatial::Pose &pose)
{
    const Spatial::Quat &q = pose.orientation;
    Mat4 r = Mat4::identity();
    r.m[0] = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    r.m[1] = 2.0f * (q.x * q.y + q.z * q.w);
    r.m[2] = 2.0f * (q.x * q.z - q.y * q.w);
    r.m[4] = 2.0f * (q.x * q.y - q.z * q.w);
    r.m[5] = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
    r.m[6] = 2.0f * (q.y * q.z + q.x * q.w);
    r.m[8] = 2.0f * (q.x * q.z + q.y * q.w);
    r.m[9] = 2.0f * (q.y * q.z - q.x * q.w);
    r.m[10] = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    r.m[12] = pose.position.x;
    r.m[13] = pose.position.y;
    r.m[14] = pose.position.z;
    return r;
}

//! OpenXR's field of view as Vulkan's clip space (y down, depth 0..1), as OpenXR's own xr_linear.h does it.
Mat4 projection(const EyeView &view)
{
    const float left = std::tan(view.angleLeft);
    const float right = std::tan(view.angleRight);
    const float up = std::tan(view.angleUp);
    const float down = std::tan(view.angleDown);
    const float width = right - left;
    const float height = down - up;
    Mat4 r;
    r.m[0] = 2.0f / width;
    r.m[8] = (right + left) / width;
    r.m[5] = 2.0f / height;
    r.m[9] = (up + down) / height;
    r.m[10] = -kFar / (kFar - kNear);
    r.m[14] = -(kFar * kNear) / (kFar - kNear);
    r.m[11] = -1.0f;
    return r;
}

struct Push {
    float clip[16];
    float modelView[16];
};

} // namespace

uint32_t ControllerRenderer::memoryType(uint32_t bits, VkMemoryPropertyFlags wanted) const
{
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(m_physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & wanted) == wanted) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool ControllerRenderer::createAttachment(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, Attachment &out)
{
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {m_width, m_height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = kSamples;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_device, &ici, nullptr, &out.image) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(m_device, out.image, &req);
    uint32_t type = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT);
    if (type == UINT32_MAX) {
        type = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (type == UINT32_MAX || vkAllocateMemory(m_device, &mai, nullptr, &out.memory) != VK_SUCCESS) {
        return false;
    }
    vkBindImageMemory(m_device, out.image, out.memory, 0);
    VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivci.image = out.image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = format;
    ivci.subresourceRange = {aspect, 0, 1, 0, 1};
    return vkCreateImageView(m_device, &ivci, nullptr, &out.view) == VK_SUCCESS;
}

bool ControllerRenderer::createBuffer(VkBufferUsageFlags usage, const void *data, VkDeviceSize size, VkBuffer &buffer, VkDeviceMemory &memory)
{
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    if (vkCreateBuffer(m_device, &bci, nullptr, &buffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(m_device, buffer, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(m_device, &mai, nullptr, &memory) != VK_SUCCESS) {
        return false;
    }
    vkBindBufferMemory(m_device, buffer, memory, 0);
    void *mapped = nullptr;
    if (vkMapMemory(m_device, memory, 0, size, 0, &mapped) != VK_SUCCESS) {
        return false;
    }
    std::memcpy(mapped, data, size_t(size));
    vkUnmapMemory(m_device, memory);
    return true;
}

bool ControllerRenderer::init(VkPhysicalDevice physical, VkDevice device, VkFormat format, uint32_t width, uint32_t height,
                              const std::array<std::vector<VkImage>, 2> &images, const ControllerMesh &right, const ControllerMesh &left)
{
    m_physical = physical;
    m_device = device;
    m_width = width;
    m_height = height;
    const auto fail = [this](const char *what) {
        qCWarning(lcXr) << "controllers: not drawn (" << what << ")";
        destroy();
        return false;
    };

    // Multisampled colour and depth, resolved into the eye's swapchain image.
    VkAttachmentDescription attachments[3]{};
    attachments[0] = {0, format, kSamples, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                      VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    attachments[1] = {0, kDepthFormat, kSamples, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                      VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    attachments[2] = {0, format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                      VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference colourRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference resolveRef{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colourRef;
    subpass.pResolveAttachments = &resolveRef;
    subpass.pDepthStencilAttachment = &depthRef;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 3;
    rpci.pAttachments = attachments;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dependency;
    if (vkCreateRenderPass(device, &rpci, nullptr, &m_renderPass) != VK_SUCCESS) {
        return fail("render pass");
    }

    for (int eye = 0; eye < 2; ++eye) {
        if (!createAttachment(format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT, m_colour[eye])
            || !createAttachment(kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, m_depth[eye])) {
            return fail("attachments");
        }
        for (VkImage image : images[eye]) {
            VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            ivci.image = image;
            ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            ivci.format = format;
            ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView view = VK_NULL_HANDLE;
            if (vkCreateImageView(device, &ivci, nullptr, &view) != VK_SUCCESS) {
                return fail("swapchain views");
            }
            m_targetViews[eye].push_back(view);
            const VkImageView views[3] = {m_colour[eye].view, m_depth[eye].view, view};
            VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fci.renderPass = m_renderPass;
            fci.attachmentCount = 3;
            fci.pAttachments = views;
            fci.width = width;
            fci.height = height;
            fci.layers = 1;
            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            if (vkCreateFramebuffer(device, &fci, nullptr, &framebuffer) != VK_SUCCESS) {
                return fail("framebuffers");
            }
            m_framebuffers[eye].push_back(framebuffer);
        }
    }

    // The two models in one vertex and one index buffer: right first, then left.
    std::vector<ControllerVertex> vertices;
    std::vector<uint32_t> indices;
    for (int side = 0; side < 2; ++side) {
        const ControllerMesh &mesh = side == 0 ? right : left;
        m_vertexOffset[side] = int32_t(vertices.size());
        m_firstIndex[side] = uint32_t(indices.size());
        m_indexCount[side] = uint32_t(mesh.indices.size());
        vertices.insert(vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
        indices.insert(indices.end(), mesh.indices.begin(), mesh.indices.end());
    }
    if (!createBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices.data(), vertices.size() * sizeof(ControllerVertex), m_vertices, m_vertexMemory)
        || !createBuffer(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices.data(), indices.size() * sizeof(uint32_t), m_indices, m_indexMemory)) {
        return fail("model buffers");
    }

    // The pipeline.
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = sizeof(kVertexShader);
    smci.pCode = kVertexShader;
    vkCreateShaderModule(device, &smci, nullptr, &vertex);
    smci.codeSize = sizeof(kFragmentShader);
    smci.pCode = kFragmentShader;
    vkCreateShaderModule(device, &smci, nullptr, &fragment);
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "main", nullptr};
    const VkVertexInputBindingDescription binding{0, sizeof(ControllerVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[5] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(ControllerVertex, position)},
                                                             {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(ControllerVertex, normal)},
                                                             {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(ControllerVertex, colour)},
                                                             {3, 0, VK_FORMAT_R32_SFLOAT, offsetof(ControllerVertex, roughness)},
                                                             {4, 0, VK_FORMAT_R32_SFLOAT, offsetof(ControllerVertex, metallic)}};
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 5;
    vertexInput.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    const VkViewport viewport{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {width, height}};
    VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE; // the models' materials are double-sided; the depth test keeps the outside
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = kSamples;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    const VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(device, &plci, nullptr, &m_layout) != VK_SUCCESS) {
        vkDestroyShaderModule(device, vertex, nullptr);
        vkDestroyShaderModule(device, fragment, nullptr);
        return fail("pipeline layout");
    }
    VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vertexInput;
    gpci.pInputAssemblyState = &assembly;
    gpci.pViewportState = &viewportState;
    gpci.pRasterizationState = &raster;
    gpci.pMultisampleState = &multisample;
    gpci.pDepthStencilState = &depth;
    gpci.pColorBlendState = &blend;
    gpci.layout = m_layout;
    gpci.renderPass = m_renderPass;
    const VkResult made = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, nullptr, &m_pipeline);
    vkDestroyShaderModule(device, vertex, nullptr);
    vkDestroyShaderModule(device, fragment, nullptr);
    if (made != VK_SUCCESS) {
        m_pipeline = VK_NULL_HANDLE;
        return fail("pipeline");
    }
    qCInfo(lcXr).noquote() << QStringLiteral("controllers: drawn in the room (%1x%2 per eye, 4x multisampled, %3 and %4 triangles)")
                                  .arg(width)
                                  .arg(height)
                                  .arg(m_indexCount[0] / 3)
                                  .arg(m_indexCount[1] / 3);
    return true;
}

void ControllerRenderer::destroy()
{
    if (m_device == VK_NULL_HANDLE) {
        return;
    }
    vkDeviceWaitIdle(m_device);
    for (int eye = 0; eye < 2; ++eye) {
        for (VkFramebuffer f : m_framebuffers[eye]) {
            vkDestroyFramebuffer(m_device, f, nullptr);
        }
        m_framebuffers[eye].clear();
        for (VkImageView v : m_targetViews[eye]) {
            vkDestroyImageView(m_device, v, nullptr);
        }
        m_targetViews[eye].clear();
        for (Attachment *a : {&m_colour[eye], &m_depth[eye]}) {
            if (a->view) {
                vkDestroyImageView(m_device, a->view, nullptr);
            }
            if (a->image) {
                vkDestroyImage(m_device, a->image, nullptr);
            }
            if (a->memory) {
                vkFreeMemory(m_device, a->memory, nullptr);
            }
            *a = Attachment{};
        }
    }
    if (m_pipeline) {
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
    }
    if (m_layout) {
        vkDestroyPipelineLayout(m_device, m_layout, nullptr);
    }
    if (m_renderPass) {
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
    }
    for (auto [buffer, memory] : {std::pair{m_vertices, m_vertexMemory}, std::pair{m_indices, m_indexMemory}}) {
        if (buffer) {
            vkDestroyBuffer(m_device, buffer, nullptr);
        }
        if (memory) {
            vkFreeMemory(m_device, memory, nullptr);
        }
    }
    m_pipeline = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_renderPass = VK_NULL_HANDLE;
    m_vertices = m_indices = VK_NULL_HANDLE;
    m_vertexMemory = m_indexMemory = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
}

void ControllerRenderer::record(VkCommandBuffer cmd, int eye, uint32_t imageIndex, const EyeView &view, const std::vector<DrawnController> &controllers)
{
    if (!ready() || eye < 0 || eye > 1 || imageIndex >= m_framebuffers[eye].size()) {
        return;
    }
    VkClearValue clears[3]{};
    clears[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}}; // see-through where no controller is
    clears[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpbi.renderPass = m_renderPass;
    rpbi.framebuffer = m_framebuffers[eye][imageIndex];
    rpbi.renderArea = {{0, 0}, {m_width, m_height}};
    rpbi.clearValueCount = 3;
    rpbi.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertices, &offset);
    vkCmdBindIndexBuffer(cmd, m_indices, 0, VK_INDEX_TYPE_UINT32);
    const Mat4 viewMatrix = fromPose(view.pose.inverse());
    const Mat4 proj = projection(view);
    for (const DrawnController &c : controllers) {
        const int side = c.left ? 1 : 0;
        const Mat4 modelView = viewMatrix * fromPose(c.grip);
        const Mat4 clip = proj * modelView;
        Push push{};
        std::memcpy(push.clip, clip.m, sizeof(push.clip));
        std::memcpy(push.modelView, modelView.m, sizeof(push.modelView));
        vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push), &push);
        vkCmdDrawIndexed(cmd, m_indexCount[side], 1, m_firstIndex[side], m_vertexOffset[side], 0);
    }
    vkCmdEndRenderPass(cmd);
}

} // namespace RiftHome::Xr
