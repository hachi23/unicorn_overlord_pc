// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>

#include "common/assert.h"
#include "core/emulator_settings.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/host_shaders/smaa_blend_frag.h"
#include "video_core/host_shaders/smaa_edge_frag.h"
#include "video_core/host_shaders/smaa_weight_frag.h"
#include "video_core/renderer_vulkan/host_passes/smaa_pass.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

#include <vk_mem_alloc.h>

#include "../../../../third_party/smaa/AreaTex.h"
#include "../../../../third_party/smaa/SearchTex.h"

namespace Vulkan::HostPasses {

namespace {
constexpr vk::Format EdgeFormat = vk::Format::eR8G8Unorm;
constexpr vk::Format WeightFormat = vk::Format::eR8G8B8A8Unorm;
constexpr vk::Format OutputFormat = vk::Format::eR16G16B16A16Sfloat;
constexpr vk::ImageSubresourceRange Color{
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .levelCount = 1,
    .layerCount = 1,
};
constexpr u32 MaxInputs = 3;

struct Constants {
    float metrics[4]; // SMAA_RT_METRICS
    u32 linear;
};
} // namespace

void SmaaPass::Create(vk::Device device, VmaAllocator allocator) {
    this->device = device;
    this->allocator = allocator;

    const auto sampler_ci = [](vk::Filter filter) {
        return vk::SamplerCreateInfo{
            .magFilter = filter,
            .minFilter = filter,
            .mipmapMode = vk::SamplerMipmapMode::eNearest,
            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
            .addressModeW = vk::SamplerAddressMode::eClampToEdge,
            .maxLod = 0.0f,
        };
    };
    linear_sampler = Check<"create smaa sampler">(
        device.createSamplerUnique(sampler_ci(vk::Filter::eLinear)));
    point_sampler = Check<"create smaa sampler">(
        device.createSamplerUnique(sampler_ci(vk::Filter::eNearest)));

    std::array<vk::DescriptorSetLayoutBinding, MaxInputs> bindings{};
    for (u32 i = 0; i < MaxInputs; ++i) {
        bindings[i] = {
            .binding = i,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        };
    }
    descriptor_set_layout =
        Check<"create smaa descriptor set layout">(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptor,
            .bindingCount = bindings.size(),
            .pBindings = bindings.data(),
        }));
    const vk::PushConstantRange push_constants{
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(Constants),
    };
    pipeline_layout = Check<"create smaa pipeline layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &descriptor_set_layout.get(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    }));

    const auto vs_module = CompileSPV(FS_TRI_VERT, device);
    ASSERT(vs_module);
    const auto make_pipeline = [&](std::span<const u32> code, vk::Format format,
                                   const char* name) {
        const auto fs_module = CompileSPV(code, device);
        ASSERT(fs_module);
        SetObjectName(device, fs_module, "{}", name);
        const std::array stages{
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eVertex, .module = vs_module, .pName = "main"},
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eFragment, .module = fs_module, .pName = "main"},
        };
        const vk::PipelineRenderingCreateInfo rendering_ci{
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &format,
        };
        const vk::PipelineVertexInputStateCreateInfo vertex_input{};
        const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
            .topology = vk::PrimitiveTopology::eTriangleList,
        };
        const vk::PipelineViewportStateCreateInfo viewport{.viewportCount = 1, .scissorCount = 1};
        const vk::PipelineRasterizationStateCreateInfo raster{
            .polygonMode = vk::PolygonMode::eFill,
            .cullMode = vk::CullModeFlagBits::eNone,
            .lineWidth = 1.0f,
        };
        const vk::PipelineMultisampleStateCreateInfo multisample{
            .rasterizationSamples = vk::SampleCountFlagBits::e1,
        };
        const vk::PipelineColorBlendAttachmentState attachment{
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
        };
        const vk::PipelineColorBlendStateCreateInfo blend{
            .attachmentCount = 1,
            .pAttachments = &attachment,
        };
        const std::array dynamic_states{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
        const vk::PipelineDynamicStateCreateInfo dynamic{
            .dynamicStateCount = dynamic_states.size(),
            .pDynamicStates = dynamic_states.data(),
        };
        auto pipeline = Check<"create smaa pipeline">(device.createGraphicsPipelineUnique(
            {}, vk::GraphicsPipelineCreateInfo{
                    .pNext = &rendering_ci,
                    .stageCount = stages.size(),
                    .pStages = stages.data(),
                    .pVertexInputState = &vertex_input,
                    .pInputAssemblyState = &input_assembly,
                    .pViewportState = &viewport,
                    .pRasterizationState = &raster,
                    .pMultisampleState = &multisample,
                    .pColorBlendState = &blend,
                    .pDynamicState = &dynamic,
                    .layout = *pipeline_layout,
                }));
        device.destroyShaderModule(fs_module);
        return pipeline;
    };
    edge_pipeline = make_pipeline(SMAA_EDGE_FRAG, EdgeFormat, "smaa_edge.frag");
    weight_pipeline = make_pipeline(SMAA_WEIGHT_FRAG, WeightFormat, "smaa_weight.frag");
    blend_pipeline = make_pipeline(SMAA_BLEND_FRAG, OutputFormat, "smaa_blend.frag");
    device.destroyShaderModule(vs_module);
}

vk::ImageView SmaaPass::Render(vk::CommandBuffer cmdbuf, vk::ImageView input,
                               vk::Extent2D input_size, bool enable, bool linear) {
    if (!enable) {
        return input;
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{.pLabelName = "Host/SMAA"});
    }
    if (!staging) {
        Upload(cmdbuf);
    }
    Prepare(cmdbuf, input_size);

    // The previous frame's reads of the targets are done before they are drawn again.
    std::array<vk::ImageMemoryBarrier2, 3> enter;
    const std::array<const Target*, 3> targets{&edges, &weights, &output};
    for (size_t i = 0; i < targets.size(); ++i) {
        enter[i] = {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .image = targets[i]->image,
            .subresourceRange = Color,
        };
    }
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 3, .pImageMemoryBarriers = enter.data()});

    constexpr auto Read = vk::ImageLayout::eShaderReadOnlyOptimal;
    const vk::DescriptorImageInfo color{*linear_sampler, input, Read};
    // The pixels without an edge are discarded and keep the cleared 0.
    Draw(cmdbuf, *edge_pipeline, edges, std::array{color}, linear);
    Draw(cmdbuf, *weight_pipeline, weights,
         std::array{vk::DescriptorImageInfo{*linear_sampler, *edges.view, Read},
                    vk::DescriptorImageInfo{*linear_sampler, *area.view, Read},
                    vk::DescriptorImageInfo{*point_sampler, *search.view, Read}},
         linear);
    Draw(cmdbuf, *blend_pipeline, output,
         std::array{color, vk::DescriptorImageInfo{*linear_sampler, *weights.view, Read}}, linear);

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }
    return *output.view;
}

void SmaaPass::Draw(vk::CommandBuffer cmdbuf, vk::Pipeline pipeline, const Target& target,
                    std::span<const vk::DescriptorImageInfo> inputs, bool linear) {
    std::array<vk::WriteDescriptorSet, MaxInputs> writes{};
    for (u32 i = 0; i < inputs.size(); ++i) {
        writes[i] = {.dstBinding = i,
                     .descriptorCount = 1,
                     .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                     .pImageInfo = &inputs[i]};
    }
    const Constants constants{
        .metrics = {1.0f / float(size.width), 1.0f / float(size.height), float(size.width),
                    float(size.height)},
        .linear = linear ? 1u : 0u,
    };
    const vk::RenderingAttachmentInfo attachment{
        .imageView = *target.view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
    cmdbuf.setViewport(0, vk::Viewport{.width = float(size.width),
                                       .height = float(size.height),
                                       .maxDepth = 1.0f});
    cmdbuf.setScissor(0, vk::Rect2D{.extent = size});
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, *pipeline_layout, 0,
                                std::span(writes.data(), inputs.size()));
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0,
                         sizeof(constants), &constants);
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea{.extent = size},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    cmdbuf.draw(3, 1, 0, 0);
    cmdbuf.endRendering();

    // Read by the next pass, or by Anime4K, FSR 1 and the post process pass.
    const vk::ImageMemoryBarrier2 done{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = target.image,
        .subresourceRange = Color,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &done});
}

SmaaPass::Target SmaaPass::MakeImage(vk::Extent2D extent, vk::Format format,
                                     vk::ImageUsageFlags usage, const char* name) {
    Target target;
    target.image = VideoCore::UniqueImage(device, allocator);
    target.image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent{.width = extent.width, .height = extent.height, .depth = 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eSampled | usage,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    SetObjectName(device, static_cast<vk::Image>(target.image), "{}", name);
    target.view = Check<"create smaa image view">(device.createImageViewUnique({
        .image = target.image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = Color,
    }));
    return target;
}

void SmaaPass::Prepare(vk::CommandBuffer cmdbuf, vk::Extent2D new_size) {
    if (size == new_size && output.image) {
        return;
    }
    if (output.image) {
        // The game's image changed size (rare): earlier frames may still read the old images.
        Check<"smaa resize wait">(device.waitIdle());
    }
    size = new_size;
    constexpr auto Attachment =
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc;
    edges = MakeImage(size, EdgeFormat, Attachment, "SMAA edges");
    weights = MakeImage(size, WeightFormat, Attachment, "SMAA weights");
    output = MakeImage(size, OutputFormat, Attachment, "SMAA output");
}

// AreaTex (RG8) and SearchTex (R8) as SMAA ships them, copied once through a staging buffer
// that is kept: the copy may still be running when Render returns.
void SmaaPass::Upload(vk::CommandBuffer cmdbuf) {
    constexpr vk::DeviceSize SearchOffset = AREATEX_SIZE;
    vk::BufferCreateInfo buffer_ci{
        .size = AREATEX_SIZE + SEARCHTEX_SIZE,
        .usage = vk::BufferUsageFlagBits::eTransferSrc,
    };
    VmaAllocationInfo info{};
    staging.emplace(device, allocator);
    staging->Create(buffer_ci, VideoCore::MemoryType::HostUncached, &info);
    ASSERT(info.pMappedData);
    std::memcpy(info.pMappedData, areaTexBytes, AREATEX_SIZE);
    std::memcpy(static_cast<u8*>(info.pMappedData) + SearchOffset, searchTexBytes,
                SEARCHTEX_SIZE);
    vmaFlushAllocation(allocator, staging->allocation, 0, VK_WHOLE_SIZE);

    constexpr auto Dst = vk::ImageUsageFlagBits::eTransferDst;
    area = MakeImage({AREATEX_WIDTH, AREATEX_HEIGHT}, vk::Format::eR8G8Unorm, Dst, "SMAA area");
    search = MakeImage({SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT}, vk::Format::eR8Unorm, Dst,
                       "SMAA search");
    const auto transition = [&](vk::ImageLayout from, vk::ImageLayout to) {
        std::array<vk::ImageMemoryBarrier2, 2> barriers;
        const std::array<vk::Image, 2> images{area.image, search.image};
        for (size_t i = 0; i < 2; ++i) {
            barriers[i] = {
                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask = from == vk::ImageLayout::eUndefined
                                     ? vk::AccessFlagBits2::eNone
                                     : vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = from == vk::ImageLayout::eUndefined
                                    ? vk::PipelineStageFlagBits2::eTransfer
                                    : vk::PipelineStageFlagBits2::eFragmentShader,
                .dstAccessMask = from == vk::ImageLayout::eUndefined
                                     ? vk::AccessFlagBits2::eTransferWrite
                                     : vk::AccessFlagBits2::eShaderSampledRead,
                .oldLayout = from,
                .newLayout = to,
                .image = images[i],
                .subresourceRange = Color,
            };
        }
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = barriers.data()});
    };
    transition(vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);
    const auto copy = [&](vk::Image image, vk::DeviceSize offset, u32 width, u32 height) {
        cmdbuf.copyBufferToImage(
            staging->buffer, image, vk::ImageLayout::eTransferDstOptimal,
            vk::BufferImageCopy{
                .bufferOffset = offset,
                .imageSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
                .imageExtent{.width = width, .height = height, .depth = 1},
            });
    };
    copy(area.image, 0, AREATEX_WIDTH, AREATEX_HEIGHT);
    copy(search.image, SearchOffset, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT);
    transition(vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
}

} // namespace Vulkan::HostPasses
