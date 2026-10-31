// SPDX-License-Identifier: GPL-2.0-or-later

#include "vw_graphics.h"
#include "common/assert.h"
#include "core/emulator_settings.h"
#include "video_core/renderer_vulkan/host_passes/anime4k_pass.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan::HostPasses {

namespace {
constexpr u32 OutputBinding = 0, SamplerBinding = 1, FirstInputBinding = 2;
constexpr u32 MaxInputs = sizeof(Anime4kShader::inputs);
constexpr vk::Format Format = vk::Format::eR16G16B16A16Sfloat;
constexpr vk::ImageSubresourceRange Color{
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .levelCount = 1,
    .layerCount = 1,
};
} // namespace

void Anime4kPass::Create(vk::Device device, VmaAllocator allocator) {
    this->device = device;
    this->allocator = allocator;

    // mpv samples its textures bilinearly: the convolutions read texel centres, the depth to
    // space pass blends the model's input up to twice its size.
    sampler = Check<"create anime4k sampler">(device.createSamplerUnique(vk::SamplerCreateInfo{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxAnisotropy = 1.0f,
        .minLod = -1000.0f,
        .maxLod = 1000.0f,
    }));

    std::array<vk::DescriptorSetLayoutBinding, FirstInputBinding + MaxInputs> bindings{};
    bindings[OutputBinding] = {
        .binding = OutputBinding,
        .descriptorType = vk::DescriptorType::eStorageImage,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
    };
    bindings[SamplerBinding] = {
        .binding = SamplerBinding,
        .descriptorType = vk::DescriptorType::eSampler,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .pImmutableSamplers = &sampler.get(),
    };
    for (u32 i = 0; i < MaxInputs; ++i) {
        bindings[FirstInputBinding + i] = {
            .binding = FirstInputBinding + i,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        };
    }
    descriptor_set_layout =
        Check<"create anime4k descriptor set layout">(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptor,
            .bindingCount = bindings.size(),
            .pBindings = bindings.data(),
        }));

    const vk::PushConstantRange push_constants{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(u32),
    };
    pipeline_layout = Check<"anime4k pipeline layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &descriptor_set_layout.get(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    }));
    SetObjectName(device, pipeline_layout.get(), "anime4k pipeline layout");
}

vk::ImageView Anime4kPass::Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D& size,
                                  vk::Extent2D output_size, int preset, bool linear) {
    preset = VwGraphics::ParseAnime4k(preset);
    if (preset == VwGraphics::Anime4kOff) {
        return input;
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{.pLabelName = "Host/Anime4K"});
    }
    const bool quality = preset == VwGraphics::Anime4kQuality;
    const bool upscale = VwGraphics::Anime4kUpscales(int(size.width), int(size.height),
                                                     int(output_size.width),
                                                     int(output_size.height));
    vk::ImageView view = Run(cmdbuf, quality ? restore_m : restore_s, input, size, linear);
    if (upscale) {
        view = Run(cmdbuf, quality ? upscale_m : upscale_s, view, size, linear);
        size = vk::Extent2D{size.width * 2, size.height * 2};
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }
    return view;
}

vk::ImageView Anime4kPass::Run(vk::CommandBuffer cmdbuf, Model& model, vk::ImageView input,
                               vk::Extent2D size, bool linear) {
    Prepare(model, size);

    // The previous frame's reads of these images are done before they are written again.
    std::vector<vk::ImageMemoryBarrier2> enter;
    for (const Target& target : model.targets) {
        enter.push_back({
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = target.image,
            .subresourceRange = Color,
        });
    }
    cmdbuf.pipelineBarrier2({
        .imageMemoryBarrierCount = u32(enter.size()),
        .pImageMemoryBarriers = enter.data(),
    });

    const u32 linear_io = linear ? 1 : 0;
    for (size_t i = 0; i < model.shaders.size(); ++i) {
        const Anime4kShader& shader = model.shaders[i];
        const u32 width = size.width * shader.scale, height = size.height * shader.scale;
        std::array<vk::DescriptorImageInfo, FirstInputBinding + MaxInputs> infos{};
        std::array<vk::WriteDescriptorSet, FirstInputBinding + MaxInputs> writes{};
        u32 count = 0;
        infos[count] = {.imageView = model.targets[i].view.get(),
                        .imageLayout = vk::ImageLayout::eGeneral};
        writes[count] = {.dstBinding = OutputBinding,
                         .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eStorageImage,
                         .pImageInfo = &infos[count]};
        ++count;
        // A push descriptor set holds its immutable sampler only when the binding is pushed
        // (Vulkan spec, Push Descriptor Updates); the image info is ignored. RADV bakes the
        // sampler into the shader either way, NVIDIA sampled garbage without it (#56).
        infos[count] = vk::DescriptorImageInfo{};
        writes[count] = {.dstBinding = SamplerBinding,
                         .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eSampler,
                         .pImageInfo = &infos[count]};
        ++count;
        for (u32 k = 0; k < shader.input_count; ++k) {
            const int from = shader.inputs[k];
            infos[count] = from < 0 ? vk::DescriptorImageInfo{
                                          .imageView = input,
                                          .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal}
                                    : vk::DescriptorImageInfo{
                                          .imageView = model.targets[from].view.get(),
                                          .imageLayout = vk::ImageLayout::eGeneral};
            writes[count] = {.dstBinding = FirstInputBinding + k,
                             .descriptorCount = 1,
                             .descriptorType = vk::DescriptorType::eSampledImage,
                             .pImageInfo = &infos[count]};
            ++count;
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, model.pipelines[i].get());
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, pipeline_layout.get(), 0,
                                    std::span(writes.data(), count));
        cmdbuf.pushConstants(pipeline_layout.get(), vk::ShaderStageFlagBits::eCompute, 0,
                             sizeof(linear_io), &linear_io);
        cmdbuf.dispatch((width + 7) / 8, (height + 7) / 8, 1);

        const vk::MemoryBarrier2 written{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &written});
    }

    // The result as FSR 1 and the post process pass read their input.
    const vk::ImageMemoryBarrier2 done{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = model.targets.back().image,
        .subresourceRange = Color,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &done});
    return model.targets.back().view.get();
}

void Anime4kPass::Prepare(Model& model, vk::Extent2D size) {
    if (model.pipelines.empty()) {
        for (size_t i = 0; i < model.shaders.size(); ++i) {
            const auto module = CompileSPV(model.shaders[i].code, device);
            ASSERT(module);
            model.pipelines.push_back(Check<"anime4k compute pipeline">(
                device.createComputePipelineUnique({}, vk::ComputePipelineCreateInfo{
                                                           .stage{
                                                               .stage = vk::ShaderStageFlagBits::eCompute,
                                                               .module = module,
                                                               .pName = "main",
                                                           },
                                                           .layout = pipeline_layout.get(),
                                                       })));
            device.destroyShaderModule(module);
        }
    }
    if (model.input_size == size && !model.targets.empty()) {
        return;
    }
    model.input_size = size;
    if (!model.targets.empty()) {
        // The game's image changed size (rare): earlier frames may still read the old images.
        Check<"anime4k resize wait">(device.waitIdle());
    }
    model.targets.clear();
    model.targets.resize(model.shaders.size());
    for (size_t i = 0; i < model.shaders.size(); ++i) {
        Target& target = model.targets[i];
        const u32 scale = model.shaders[i].scale;
        target.image = VideoCore::UniqueImage(device, allocator);
        target.image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = Format,
            .extent{.width = size.width * scale, .height = size.height * scale, .depth = 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage |
                     vk::ImageUsageFlagBits::eTransferSrc,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        SetObjectName(device, static_cast<vk::Image>(target.image), "Anime4K Image #{}", i);
        target.view = Check<"create anime4k image view">(device.createImageViewUnique({
            .image = target.image,
            .viewType = vk::ImageViewType::e2D,
            .format = Format,
            .subresourceRange = Color,
        }));
    }
}

} // namespace Vulkan::HostPasses
