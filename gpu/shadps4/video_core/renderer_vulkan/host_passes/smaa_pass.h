// SPDX-License-Identifier: GPL-2.0-or-later
// vw: SMAA 1x anti-aliasing (Jimenez et al., MIT, gpu/third_party/smaa) on the game's image, at
// its own size, before Anime4K and FSR 1: edges, blending weights, neighbourhood blending.
#pragma once

#include <optional>
#include <span>

#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan::HostPasses {

class SmaaPass {
public:
    void Create(vk::Device device, VmaAllocator allocator);

    /// Smooths the edges of input, read as ShaderReadOnlyOptimal; returns the result (RGBA16F,
    /// holding what input samples as) in that layout, or input when off. linear: input samples
    /// as linear light (an sRGB view).
    vk::ImageView Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D size,
                         bool enable, bool linear);

private:
    struct Target {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
    };

    void Prepare(vk::CommandBuffer cmdbuf, vk::Extent2D size);
    void Upload(vk::CommandBuffer cmdbuf);
    Target MakeImage(vk::Extent2D size, vk::Format format, vk::ImageUsageFlags usage,
                     const char* name);
    void Draw(vk::CommandBuffer cmdbuf, vk::Pipeline pipeline, const Target& target,
              std::span<const vk::DescriptorImageInfo> inputs, bool linear);

    vk::Device device{};
    VmaAllocator allocator{};
    vk::UniqueSampler linear_sampler{}, point_sampler{};
    vk::UniqueDescriptorSetLayout descriptor_set_layout{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniquePipeline edge_pipeline{}, weight_pipeline{}, blend_pipeline{};
    Target area, search; // SMAA's lookup textures, uploaded on first use
    std::optional<VideoCore::UniqueBuffer> staging;
    Target edges, weights, output;
    vk::Extent2D size{};
};

} // namespace Vulkan::HostPasses
