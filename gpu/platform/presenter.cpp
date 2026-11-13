// SPDX-License-Identifier: GPL-2.0-or-later
// The presenter: shows the native renderer's display image in the window (flip thread only).
//
// Per frame: Anime4K (when chosen) restores the game's image and may double its size, FSR 1
// upscales it (EASU, then RCAS) when the window is larger, and the present pass scales it into
// a frame image that fits the window at the game's aspect ratio (area-averaged when shrinking,
// sharpened when RCAS is on). The frame is then blitted letterboxed into the swapchain image,
// the overlay drawn over it, and the image presented. Everything runs on the one queue the
// renderer also submits to (queue_mutex), so the passes need no semaphores between them: the
// renderer's copy into the display image was submitted before the flip that shows it.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <unordered_map>
#include <vector>
#include "anime4k_models.h"
#include "platform.h"
#include "sdl_window.h"
#include "shaders/fsr_easu_comp.h"
#include "shaders/fsr_rcas_comp.h"
#include "shaders/fullscreen_vert.h"
#include "shaders/present_frag.h"
#include "vw_graphics.h"
#include "vw_overlay.h"
#include "vw_settings.h"

#define A_CPU
#include "shaders/ffx_a.h"
#include "shaders/ffx_fsr1.h"

namespace Platform {
namespace {

constexpr u32 InFlight = 2;
// The frame image is BGRA whatever the swapchain uses: frame dumps (tools/uo_raw2png.py) read BGRA.
constexpr VkFormat FrameFormat = VK_FORMAT_B8G8R8A8_UNORM;
constexpr VkFormat PassFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkImageSubresourceRange Color{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

void Check(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return;
    if (r == VK_ERROR_DEVICE_LOST) {   // as src/native_vk.c: the message and exit code players' tools know
        std::fprintf(stderr, "GPU crash: the graphics device was lost (%s). Attach this run's log to your bug report.\n", what);
        std::fflush(nullptr);
        std::_Exit(23);
    }
    std::printf("Presenter: %s failed (%d)\n", what, int(r));
    std::fflush(stdout);
    std::abort();
}

PFN_vkCmdPushDescriptorSetKHR PushDescriptorSet;

struct Image {
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
    u32 width{}, height{};
};

Image CreateImage(u32 width, u32 height, VkFormat format, VkImageUsageFlags usage) {
    Image i{.width = width, .height = height};
    const VkImageCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                 .imageType = VK_IMAGE_TYPE_2D,
                                 .format = format,
                                 .extent = {width, height, 1},
                                 .mipLevels = 1,
                                 .arrayLayers = 1,
                                 .samples = VK_SAMPLE_COUNT_1_BIT,
                                 .tiling = VK_IMAGE_TILING_OPTIMAL,
                                 .usage = usage};
    Check(vkCreateImage(vk.device, &info, nullptr, &i.image), "image");
    VkMemoryRequirements need;
    vkGetImageMemoryRequirements(vk.device, i.image, &need);
    const VkMemoryAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                        .allocationSize = need.size,
                                        .memoryTypeIndex = MemoryType(need.memoryTypeBits,
                                                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    Check(vkAllocateMemory(vk.device, &allocate, nullptr, &i.memory), "image memory");
    Check(vkBindImageMemory(vk.device, i.image, i.memory, 0), "bind image memory");
    const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .image = i.image,
                                     .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                     .format = format,
                                     .subresourceRange = Color};
    Check(vkCreateImageView(vk.device, &view, nullptr, &i.view), "image view");
    return i;
}

void DestroyImage(Image& i) {
    if (i.view) vkDestroyImageView(vk.device, i.view, nullptr);
    if (i.image) vkDestroyImage(vk.device, i.image, nullptr);
    if (i.memory) vkFreeMemory(vk.device, i.memory, nullptr);
    i = {};
}

void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
             VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
             VkAccessFlags2 dst_access) {
    const VkImageMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                  .srcStageMask = src_stage,
                                  .srcAccessMask = src_access,
                                  .dstStageMask = dst_stage,
                                  .dstAccessMask = dst_access,
                                  .oldLayout = from,
                                  .newLayout = to,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .image = image,
                                  .subresourceRange = Color};
    const VkDependencyInfo d{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                             .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cmd, &d);
}

void MemoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                   VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    const VkMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                             .srcStageMask = src_stage,
                             .srcAccessMask = src_access,
                             .dstStageMask = dst_stage,
                             .dstAccessMask = dst_access};
    const VkDependencyInfo d{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                             .pMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cmd, &d);
}

VkShaderModule Module(std::span<const uint32_t> code) {
    const VkShaderModuleCreateInfo info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                        .codeSize = code.size_bytes(),
                                        .pCode = code.data()};
    VkShaderModule m;
    Check(vkCreateShaderModule(vk.device, &info, nullptr, &m), "shader module");
    return m;
}

VkPipeline ComputePipeline(std::span<const uint32_t> code, VkPipelineLayout layout) {
    const VkShaderModule m = Module(code);
    const VkComputePipelineCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = m,
                  .pName = "main"},
        .layout = layout};
    VkPipeline p;
    Check(vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &info, nullptr, &p), "compute pipeline");
    vkDestroyShaderModule(vk.device, m, nullptr);
    return p;
}

VkSampler LinearSampler() {
    const VkSamplerCreateInfo info{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                   .magFilter = VK_FILTER_LINEAR,
                                   .minFilter = VK_FILTER_LINEAR,
                                   .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                   .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                   .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                   .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                   .maxAnisotropy = 1.0f,
                                   .minLod = -1000.0f,
                                   .maxLod = 1000.0f};
    VkSampler s;
    Check(vkCreateSampler(vk.device, &info, nullptr, &s), "sampler");
    return s;
}

VkDescriptorImageInfo ImageInfo(VkImageView view, VkImageLayout layout) {
    return {.imageView = view, .imageLayout = layout};
}
VkWriteDescriptorSet Write(u32 binding, VkDescriptorType type, const VkDescriptorImageInfo* info) {
    return {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstBinding = binding,
            .descriptorCount = 1,
            .descriptorType = type,
            .pImageInfo = info};
}

// ---- Anime4K (bloc97, MIT): its restore model, then its x2 upscale when the window is larger ----
class Anime4k {
public:
    void Create() {
        sampler = LinearSampler();
        std::array<VkDescriptorSetLayoutBinding, FirstInput + MaxInputs> bindings{};
        bindings[Output] = {Output, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[Sampler] = {Sampler, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &sampler};
        for (u32 i = 0; i < MaxInputs; ++i)
            bindings[FirstInput + i] = {FirstInput + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                        VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        const VkDescriptorSetLayoutCreateInfo set_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
            .bindingCount = u32(bindings.size()),
            .pBindings = bindings.data()};
        Check(vkCreateDescriptorSetLayout(vk.device, &set_info, nullptr, &set_layout), "anime4k set layout");
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(u32)};
        const VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                     .setLayoutCount = 1,
                                                     .pSetLayouts = &set_layout,
                                                     .pushConstantRangeCount = 1,
                                                     .pPushConstantRanges = &push};
        Check(vkCreatePipelineLayout(vk.device, &layout_info, nullptr, &layout), "anime4k layout");
    }

    /// Runs the preset on input (ShaderReadOnlyOptimal); returns the result in that layout and
    /// sets size to its size, or returns input when off. linear: input samples as linear light.
    VkImageView Render(VkCommandBuffer cmd, VkImageView input, VkExtent2D& size, VkExtent2D output_size,
                       int preset, bool linear) {
        preset = VwGraphics::ParseAnime4k(preset);
        if (preset == VwGraphics::Anime4kOff) return input;
        const bool quality = preset == VwGraphics::Anime4kQuality;
        const bool upscale = VwGraphics::Anime4kUpscales(int(size.width), int(size.height),
                                                         int(output_size.width), int(output_size.height));
        VkImageView view = Run(cmd, quality ? restore_m : restore_s, input, size, linear);
        if (upscale) {
            view = Run(cmd, quality ? upscale_m : upscale_s, view, size, linear);
            size = {size.width * 2, size.height * 2};
        }
        return view;
    }

private:
    static constexpr u32 Output = 0, Sampler = 1, FirstInput = 2, MaxInputs = sizeof(Anime4kShader::inputs);
    struct Model {
        std::span<const Anime4kShader> shaders;
        std::vector<VkPipeline> pipelines;
        std::vector<Image> targets;
        VkExtent2D input_size{};
    };

    VkImageView Run(VkCommandBuffer cmd, Model& model, VkImageView input, VkExtent2D size, bool linear) {
        Prepare(model, size);
        for (const Image& target : model.targets)
            Barrier(cmd, target.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        const u32 linear_io = linear ? 1 : 0;
        for (size_t i = 0; i < model.shaders.size(); ++i) {
            const Anime4kShader& shader = model.shaders[i];
            const u32 width = size.width * shader.scale, height = size.height * shader.scale;
            std::array<VkDescriptorImageInfo, FirstInput + MaxInputs> infos{};
            std::array<VkWriteDescriptorSet, FirstInput + MaxInputs> writes{};
            u32 n = 0;
            infos[n] = ImageInfo(model.targets[i].view, VK_IMAGE_LAYOUT_GENERAL);
            writes[n] = Write(Output, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &infos[n]);
            ++n;
            // The immutable sampler is only used when its binding is pushed (NVIDIA, #56).
            infos[n] = {};
            writes[n] = Write(Sampler, VK_DESCRIPTOR_TYPE_SAMPLER, &infos[n]);
            ++n;
            for (u32 k = 0; k < shader.input_count; ++k) {
                const int from = shader.inputs[k];
                infos[n] = from < 0 ? ImageInfo(input, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                                    : ImageInfo(model.targets[from].view, VK_IMAGE_LAYOUT_GENERAL);
                writes[n] = Write(FirstInput + k, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &infos[n]);
                ++n;
            }
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, model.pipelines[i]);
            PushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, n, writes.data());
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(linear_io), &linear_io);
            vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
            MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        Barrier(cmd, model.targets.back().image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        return model.targets.back().view;
    }

    void Prepare(Model& model, VkExtent2D size) {
        if (model.pipelines.empty())
            for (const Anime4kShader& shader : model.shaders)
                model.pipelines.push_back(ComputePipeline(shader.code, layout));
        if (model.input_size.width == size.width && model.input_size.height == size.height &&
            !model.targets.empty())
            return;
        model.input_size = size;
        if (!model.targets.empty()) {
            // The game's image changed size (rare): earlier frames may still read the old images.
            std::scoped_lock lock{queue_mutex};
            vkQueueWaitIdle(vk.queue);
        }
        for (Image& t : model.targets) DestroyImage(t);
        model.targets.clear();
        for (const Anime4kShader& shader : model.shaders)
            model.targets.push_back(CreateImage(size.width * shader.scale, size.height * shader.scale, PassFormat,
                                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT));
    }

    VkSampler sampler{};
    VkDescriptorSetLayout set_layout{};
    VkPipelineLayout layout{};
    Model restore_s{RESTORE_S}, restore_m{RESTORE_M}, upscale_s{UPSCALE_S}, upscale_m{UPSCALE_M};
};

// ---- FSR 1 (AMD, MIT): EASU upscale, then RCAS sharpening ----
class Fsr {
public:
    struct Constants {
        u32 const0[4], const1[4], const2[4], const3[4], sample[4];
    };

    void Create() {
        sampler = LinearSampler();
        const std::array<VkDescriptorSetLayoutBinding, 3> bindings{{
            {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &sampler},
        }};
        const VkDescriptorSetLayoutCreateInfo set_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
            .bindingCount = u32(bindings.size()),
            .pBindings = bindings.data()};
        Check(vkCreateDescriptorSetLayout(vk.device, &set_info, nullptr, &set_layout), "fsr set layout");
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Constants)};
        const VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                     .setLayoutCount = 1,
                                                     .pSetLayouts = &set_layout,
                                                     .pushConstantRangeCount = 1,
                                                     .pPushConstantRanges = &push};
        Check(vkCreatePipelineLayout(vk.device, &layout_info, nullptr, &layout), "fsr layout");
        easu = ComputePipeline(FSR_EASU_COMP, layout);
        rcas = ComputePipeline(FSR_RCAS_COMP, layout);
    }

    /// FSR 1 when the window is larger than the image, RCAS alone at the same size; else input.
    VkImageView Render(VkCommandBuffer cmd, VkImageView input, VkExtent2D input_size, VkExtent2D output_size,
                       bool fsr1, bool use_rcas, float attenuation) {
        if (!VwGraphics::FsrPassRuns(fsr1, use_rcas, int(input_size.width), int(input_size.height),
                                     int(output_size.width), int(output_size.height)))
            return input;
        if (output.width != output_size.width || output.height != output_size.height) {
            if (output.image) {
                std::scoped_lock lock{queue_mutex};
                vkQueueWaitIdle(vk.queue);
            }
            DestroyImage(intermediate);
            DestroyImage(output);
            const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
            intermediate = CreateImage(output_size.width, output_size.height, PassFormat, usage);
            output = CreateImage(output_size.width, output_size.height, PassFormat, usage);
        }
        const u32 groups_x = (output.width + 15) / 16, groups_y = (output.height + 15) / 16;
        Constants c{};
        FsrEasuCon(reinterpret_cast<AU1*>(c.const0), reinterpret_cast<AU1*>(c.const1),
                   reinterpret_cast<AU1*>(c.const2), reinterpret_cast<AU1*>(c.const3), AF1(input_size.width),
                   AF1(input_size.height), AF1(input_size.width), AF1(input_size.height), AF1(output.width),
                   AF1(output.height));
        const Image& easu_target = use_rcas ? intermediate : output;
        Barrier(cmd, easu_target.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        Dispatch(cmd, easu, input, easu_target.view, c, groups_x, groups_y);
        if (use_rcas) {
            Barrier(cmd, intermediate.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
            Barrier(cmd, output.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            c = {};
            FsrRcasCon(reinterpret_cast<AU1*>(c.const0), attenuation);
            Dispatch(cmd, rcas, intermediate.view, output.view, c, groups_x, groups_y);
        }
        Barrier(cmd, output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        return output.view;
    }

private:
    void Dispatch(VkCommandBuffer cmd, VkPipeline pipeline, VkImageView in, VkImageView out, const Constants& c,
                  u32 x, u32 y) {
        const VkDescriptorImageInfo infos[3] = {ImageInfo(in, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
                                                ImageInfo(out, VK_IMAGE_LAYOUT_GENERAL), {}};
        const VkWriteDescriptorSet writes[3] = {Write(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &infos[0]),
                                                Write(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &infos[1]),
                                                Write(2, VK_DESCRIPTOR_TYPE_SAMPLER, &infos[2])};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        PushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 3, writes);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(c), &c);
        vkCmdDispatch(cmd, x, y, 1);
    }

    VkSampler sampler{};
    VkDescriptorSetLayout set_layout{};
    VkPipelineLayout layout{};
    VkPipeline easu{}, rcas{};
    Image intermediate, output;
};

// ---- The present pass (present.frag): the image into the frame, sRGB encoded ----
class PresentPass {
public:
    struct Settings {
        u32 srgb_input;
        float sharpen;
    };

    void Create() {
        sampler = LinearSampler();
        const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        const VkDescriptorSetLayoutCreateInfo set_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
            .bindingCount = 1,
            .pBindings = &binding};
        Check(vkCreateDescriptorSetLayout(vk.device, &set_info, nullptr, &set_layout), "present set layout");
        const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Settings)};
        const VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                     .setLayoutCount = 1,
                                                     .pSetLayouts = &set_layout,
                                                     .pushConstantRangeCount = 1,
                                                     .pPushConstantRanges = &push};
        Check(vkCreatePipelineLayout(vk.device, &layout_info, nullptr, &layout), "present layout");

        const VkShaderModule vs = Module(FULLSCREEN_VERT), fs = Module(PRESENT_FRAG);
        const VkPipelineShaderStageCreateInfo stages[2] = {
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
             .module = vs, .pName = "main"},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
             .module = fs, .pName = "main"}};
        const VkFormat format = FrameFormat;
        const VkPipelineRenderingCreateInfo rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                                      .colorAttachmentCount = 1,
                                                      .pColorAttachmentFormats = &format};
        const VkPipelineVertexInputStateCreateInfo vertex_input{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        const VkPipelineInputAssemblyStateCreateInfo assembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        const VkPipelineViewportStateCreateInfo viewport{.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                         .viewportCount = 1,
                                                         .scissorCount = 1};
        const VkPipelineRasterizationStateCreateInfo raster{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .lineWidth = 1.0f};
        const VkPipelineMultisampleStateCreateInfo multisample{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
        const VkPipelineColorBlendAttachmentState attachment{.colorWriteMask = 0xf};
        const VkPipelineColorBlendStateCreateInfo blend{.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                        .attachmentCount = 1,
                                                        .pAttachments = &attachment};
        const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        const VkPipelineDynamicStateCreateInfo dynamic{.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                       .dynamicStateCount = 2,
                                                       .pDynamicStates = dynamic_states};
        const VkGraphicsPipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                                .pNext = &rendering,
                                                .stageCount = 2,
                                                .pStages = stages,
                                                .pVertexInputState = &vertex_input,
                                                .pInputAssemblyState = &assembly,
                                                .pViewportState = &viewport,
                                                .pRasterizationState = &raster,
                                                .pMultisampleState = &multisample,
                                                .pColorBlendState = &blend,
                                                .pDynamicState = &dynamic,
                                                .layout = layout};
        Check(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline), "present pipeline");
        vkDestroyShaderModule(vk.device, vs, nullptr);
        vkDestroyShaderModule(vk.device, fs, nullptr);
    }

    /// Draws input into frame (left in TRANSFER_SRC_OPTIMAL for the blit).
    void Render(VkCommandBuffer cmd, VkImageView input, const Image& frame, Settings settings) {
        Barrier(cmd, frame.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        const VkRenderingAttachmentInfo attachment{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                                   .imageView = frame.view,
                                                   .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                   .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                                   .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        const VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                        .renderArea = {{0, 0}, {frame.width, frame.height}},
                                        .layerCount = 1,
                                        .colorAttachmentCount = 1,
                                        .pColorAttachments = &attachment};
        const VkDescriptorImageInfo image{.sampler = sampler, .imageView = input,
                                          .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkWriteDescriptorSet write = Write(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &image);
        const VkViewport viewport{0, 0, float(frame.width), float(frame.height), 0, 1};
        const VkRect2D scissor{{0, 0}, {frame.width, frame.height}};
        vkCmdBeginRendering(cmd, &rendering);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        PushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &write);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(settings), &settings);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        Barrier(cmd, frame.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    }

private:
    VkSampler sampler{};
    VkDescriptorSetLayout set_layout{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
};

// ---- The swapchain ----
struct Swapchain {
    VkSwapchainKHR handle{};
    VkSurfaceFormatKHR format{};
    VkPresentModeKHR mode{};
    bool vsync{};
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkSemaphore> rendered; // per image: the present waits for it
    u32 width{}, height{};             // the window size it was made for

    void ChooseFormat() {
        u32 count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical, vk.surface, &count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical, vk.surface, &count, formats.data());
        format = {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        if (count && formats[0].format == VK_FORMAT_UNDEFINED) return;
        for (const auto& f : formats)
            if (f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) {
                format = f;
                return;
            }
    }

    void ChooseMode() {
        u32 count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(vk.physical, vk.surface, &count, nullptr);
        std::vector<VkPresentModeKHR> modes(count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(vk.physical, vk.surface, &count, modes.data());
        const auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
        // VSync on is Fifo (no tearing); off is Mailbox when the driver has it, else Immediate.
        vsync = VwSettings::Get().vsync;
        mode = vsync                                  ? VK_PRESENT_MODE_FIFO_KHR
               : has(VK_PRESENT_MODE_MAILBOX_KHR)     ? VK_PRESENT_MODE_MAILBOX_KHR
               : has(VK_PRESENT_MODE_IMMEDIATE_KHR)   ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                                      : VK_PRESENT_MODE_FIFO_KHR;
        std::printf("Swapchain: VSync %s (%s)\n", vsync ? "on" : "off",
                    mode == VK_PRESENT_MODE_FIFO_KHR      ? "Fifo"
                    : mode == VK_PRESENT_MODE_MAILBOX_KHR ? "Mailbox"
                                                          : "Immediate");
    }

    /// Callers hold no queue work on the old images (the queue is idle).
    void Create(u32 w, u32 h) {
        width = w;
        height = h;
        VkSurfaceCapabilitiesKHR caps;
        Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physical, vk.surface, &caps),
              "surface capabilities (the graphics driver cannot show this window; seen on Windows after "
              "the PC woke from sleep: restart the PC)");
        extent = caps.currentExtent;
        if (extent.width == UINT32_MAX) {
            extent.width = std::clamp(w, caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = std::clamp(h, caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        u32 count = caps.minImageCount + 1;
        if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);
        const VkSwapchainKHR old = handle;
        const VkSwapchainCreateInfoKHR info{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = vk.surface,
            .minImageCount = count,
            .imageFormat = format.format,
            .imageColorSpace = format.colorSpace,
            .imageExtent = extent,
            .imageArrayLayers = 1,
            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                                ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                : caps.currentTransform,
            .compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                                  ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                  : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
            .presentMode = mode,
            .clipped = VK_TRUE,
            .oldSwapchain = old};
        Check(vkCreateSwapchainKHR(vk.device, &info, nullptr, &handle), "swapchain");
        DestroyViews();
        if (old) vkDestroySwapchainKHR(vk.device, old, nullptr);
        vkGetSwapchainImagesKHR(vk.device, handle, &count, nullptr);
        images.resize(count);
        vkGetSwapchainImagesKHR(vk.device, handle, &count, images.data());
        for (VkImage image : images) {
            const VkImageViewCreateInfo view_info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                                  .image = image,
                                                  .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                                  .format = format.format,
                                                  .subresourceRange = Color};
            VkImageView view;
            Check(vkCreateImageView(vk.device, &view_info, nullptr, &view), "swapchain view");
            views.push_back(view);
            const VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkSemaphore semaphore;
            Check(vkCreateSemaphore(vk.device, &semaphore_info, nullptr, &semaphore), "semaphore");
            rendered.push_back(semaphore);
        }
    }

    void DestroyViews() {
        for (VkImageView v : views) vkDestroyImageView(vk.device, v, nullptr);
        for (VkSemaphore s : rendered) vkDestroySemaphore(vk.device, s, nullptr);
        views.clear();
        rendered.clear();
    }
};

// ---- State (flip thread) ----
Swapchain swapchain;
Anime4k anime4k;
Fsr fsr;
PresentPass present_pass;
VkCommandPool pool;
struct Slot {
    VkCommandBuffer cmd;
    VkFence done;
    VkSemaphore acquired;
};
std::array<Slot, InFlight> slots;
u32 slot_index;
Image frame;          // the last game frame at window aspect (TRANSFER_SRC_OPTIMAL once drawn)
bool frame_valid;     // frame holds a game image
float game_ratio = 16.0f / 9.0f;
std::unordered_map<u64, VkImageView> native_views;

void WaitIdle() {
    std::scoped_lock lock{queue_mutex};
    vkQueueWaitIdle(vk.queue);
}

/// The swapchain matches the window and the VSync setting; false while the window has no pixels.
bool UpdateSwapchain(bool force) {
    const u32 w = u32(std::max(window->GetWidth(), 0)), h = u32(std::max(window->GetHeight(), 0));
    if (!w || !h) return false;
    const bool vsync_changed = VwSettings::Get().vsync != swapchain.vsync;
    if (force || vsync_changed || w != swapchain.width || h != swapchain.height) {
        WaitIdle();
        if (vsync_changed) swapchain.ChooseMode();
        swapchain.Create(w, h);
    }
    return true;
}

/// The frame size for the swapchain: the game's aspect ratio fitted into it.
VkExtent2D FrameSize(VkExtent2D target) {
    VkExtent2D size = target;
    if (float(target.width) / float(target.height) > game_ratio)
        size.width = u32(float(target.height) * game_ratio);
    else
        size.height = u32(float(target.width) / game_ratio);
    return {std::max(size.width, 1u), std::max(size.height, 1u)};
}

VkImageView NativeView(VkImage image, bool srgb) {
    const u64 key = reinterpret_cast<u64>(image) ^ u64(srgb);
    VkImageView& view = native_views[key];
    if (!view) {
        const VkImageViewCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
            // Alpha is excluded from the output.
            .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_ONE},
            .subresourceRange = Color};
        Check(vkCreateImageView(vk.device, &info, nullptr, &view), "native view");
    }
    return view;
}

/// Records the passes from the game's image into `frame`.
void DrawFrame(VkCommandBuffer cmd, VkImage image, u32 width, u32 height, DisplayFormat format) {
    const bool srgb = format == DisplayFormat::Srgb;
    VkImageView view = NativeView(image, srgb);
    VkExtent2D size{width, height};
    game_ratio = float(width) / float(height);
    const VkExtent2D frame_size = FrameSize(swapchain.extent);
    if (frame.width != frame_size.width || frame.height != frame_size.height) {
        if (frame.image) WaitIdle();
        DestroyImage(frame);
        frame = CreateImage(frame_size.width, frame_size.height, FrameFormat,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    }
    auto& s = VwSettings::Get();
    const VkExtent2D game_size = size;
    view = anime4k.Render(cmd, view, size, frame_size, s.anime4k, srgb);
    const bool fsr1 = s.fsr1, rcas = s.rcas;
    s.image_width = int(game_size.width);
    s.image_height = int(game_size.height);
    s.anime4k_upscaled = size.width != game_size.width || size.height != game_size.height;
    s.window_width = int(frame.width);
    s.window_height = int(frame.height);
    s.rcas_applied = rcas && VwGraphics::FsrPassRuns(fsr1, true, int(size.width), int(size.height),
                                                     int(frame.width), int(frame.height));
    // A larger image is averaged down in the present pass, which also sharpens it (CAS) when
    // sharpening is on: RCAS only runs at the same size or above.
    const bool downscaling = size.width > frame.width || size.height > frame.height;
    PresentPass::Settings settings{
        .srgb_input = format == DisplayFormat::Srgb10 ? 1u : 0u,
        .sharpen = downscaling && rcas ? float(VwGraphics::ParseRcasStrength(s.rcas_strength)) / 100.0f : 0.0f};
    if (settings.sharpen > 0.0f) s.rcas_applied = true;
    view = fsr.Render(cmd, view, size, {frame.width, frame.height}, fsr1, rcas,
                      VwGraphics::RcasAttenuation(s.rcas_strength));
    present_pass.Render(cmd, view, frame, settings);
    frame_valid = true;
}

/// VW_FRAME_DUMP_TRIGGER=<file>: when the file exists it is removed and the presented frame is
/// written to VW_DUMP_DIR (default out/dump) as raw BGRA (output_<n>_<w>x<h>.raw).
struct Dump {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* mapped{};
    u32 width{}, height{};
};

Dump RecordDump(VkCommandBuffer cmd) {
    Dump d{.width = frame.width, .height = frame.height};
    const VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  .size = VkDeviceSize(d.width) * d.height * 4,
                                  .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    Check(vkCreateBuffer(vk.device, &info, nullptr, &d.buffer), "dump buffer");
    VkMemoryRequirements need;
    vkGetBufferMemoryRequirements(vk.device, d.buffer, &need);
    const VkMemoryAllocateInfo allocate{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = need.size,
        .memoryTypeIndex = MemoryType(need.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                               VK_MEMORY_PROPERTY_HOST_CACHED_BIT)};
    Check(vkAllocateMemory(vk.device, &allocate, nullptr, &d.memory), "dump memory");
    Check(vkBindBufferMemory(vk.device, d.buffer, d.memory, 0), "bind dump memory");
    Check(vkMapMemory(vk.device, d.memory, 0, VK_WHOLE_SIZE, 0, &d.mapped), "map dump");
    const VkBufferImageCopy copy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                 .imageExtent = {d.width, d.height, 1}};
    vkCmdCopyImageToBuffer(cmd, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d.buffer, 1, &copy);
    return d;
}

void WriteDump(Dump& d, VkFence done) {
    static int index;
    vkWaitForFences(vk.device, 1, &done, VK_TRUE, UINT64_MAX);
    const char* dir = std::getenv("VW_DUMP_DIR");
    char path[512];
    std::snprintf(path, sizeof(path), "%s/output_%03d_%ux%u.raw", dir && *dir ? dir : "out/dump", index++, d.width,
                  d.height);
    if (FILE* f = std::fopen(path, "wb")) {
        std::fwrite(d.mapped, 1, size_t(d.width) * d.height * 4, f);
        std::fclose(f);
    }
    std::printf("Frame dump: %s\n", path);
    vkDestroyBuffer(vk.device, d.buffer, nullptr);
    vkFreeMemory(vk.device, d.memory, nullptr);
}

/// One presentation: the game's image when given (else the last frame, or black when draw is
/// false), the overlay over it.
void Show(VkImage image, u32 width, u32 height, DisplayFormat format, bool draw) {
    if (!UpdateSwapchain(false)) return;
    Slot& slot = slots[slot_index];
    slot_index = (slot_index + 1) % InFlight;
    vkWaitForFences(vk.device, 1, &slot.done, VK_TRUE, UINT64_MAX);

    u32 index = 0;
    VkResult acquired = vkAcquireNextImageKHR(vk.device, swapchain.handle, UINT64_MAX, slot.acquired,
                                              VK_NULL_HANDLE, &index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        if (!UpdateSwapchain(true)) return;
        acquired = vkAcquireNextImageKHR(vk.device, swapchain.handle, UINT64_MAX, slot.acquired, VK_NULL_HANDLE,
                                         &index);
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        // The window changes faster than the swapchain keeps up: skip this frame.
        return;
    }
    vkResetFences(vk.device, 1, &slot.done);

    const VkCommandBuffer cmd = slot.cmd;
    vkResetCommandBuffer(cmd, 0);
    const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cmd, &begin);

    static const char* dump_trigger = std::getenv("VW_FRAME_DUMP_TRIGGER");
    if (image) DrawFrame(cmd, image, width, height, format);
    else if (draw) frame_valid = false;
    const bool dump = image && dump_trigger && std::remove(dump_trigger) == 0;
    Dump dumped;
    if (dump) dumped = RecordDump(cmd);

    const VkImage target = swapchain.images[index];
    Barrier(cmd, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
    vkCmdClearColorImage(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &Color);
    if (frame_valid) {
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        // Letterboxed at the frame's own aspect ratio.
        const VkExtent2D e = swapchain.extent;
        const float frame_aspect = float(frame.width) / float(frame.height);
        u32 w = e.width, h = e.height;
        if (frame_aspect > float(e.width) / float(e.height)) h = u32(float(e.width) / frame_aspect);
        else w = u32(float(e.height) * frame_aspect);
        const s32 x = s32(e.width - w) / 2, y = s32(e.height - h) / 2;
        const VkImageBlit blit{.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               .srcOffsets = {{0, 0, 0}, {s32(frame.width), s32(frame.height), 1}},
                               .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               .dstOffsets = {{x, y, 0}, {x + s32(w), y + s32(h), 1}}};
        vkCmdBlitImage(cmd, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    VwOverlay::NotePresent();
    if (VwOverlay::Visible()) {
        Barrier(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VwOverlay::Render(cmd, swapchain.views[index], swapchain.extent);
        Barrier(cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
    } else {
        Barrier(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
    }
    vkEndCommandBuffer(cmd);

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                              .waitSemaphoreCount = 1,
                              .pWaitSemaphores = &slot.acquired,
                              .pWaitDstStageMask = &wait_stage,
                              .commandBufferCount = 1,
                              .pCommandBuffers = &cmd,
                              .signalSemaphoreCount = 1,
                              .pSignalSemaphores = &swapchain.rendered[index]};
    const VkPresentInfoKHR present{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                   .waitSemaphoreCount = 1,
                                   .pWaitSemaphores = &swapchain.rendered[index],
                                   .swapchainCount = 1,
                                   .pSwapchains = &swapchain.handle,
                                   .pImageIndices = &index};
    VkResult presented;
    {
        std::scoped_lock lock{queue_mutex};
        Check(vkQueueSubmit(vk.queue, 1, &submit, slot.done), "present submit");
        presented = vkQueuePresentKHR(vk.queue, &present);
    }
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) UpdateSwapchain(true);
    if (dump) WriteDump(dumped, slot.done);
}

} // namespace

void PresenterInit() {
    PushDescriptorSet = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
        vkGetDeviceProcAddr(vk.device, "vkCmdPushDescriptorSetKHR"));
    swapchain.ChooseFormat();
    swapchain.ChooseMode();
    swapchain.Create(u32(std::max(window->GetWidth(), 1)), u32(std::max(window->GetHeight(), 1)));
    const VkCommandPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                            .queueFamilyIndex = vk.family};
    Check(vkCreateCommandPool(vk.device, &pool_info, nullptr, &pool), "command pool");
    for (Slot& slot : slots) {
        const VkCommandBufferAllocateInfo cmd_info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                   .commandPool = pool,
                                                   .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                   .commandBufferCount = 1};
        Check(vkAllocateCommandBuffers(vk.device, &cmd_info, &slot.cmd), "command buffer");
        const VkFenceCreateInfo fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                           .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        Check(vkCreateFence(vk.device, &fence_info, nullptr, &slot.done), "fence");
        const VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        Check(vkCreateSemaphore(vk.device, &semaphore_info, nullptr, &slot.acquired), "semaphore");
    }
    anime4k.Create();
    fsr.Create();
    present_pass.Create();
    VwOverlay::Init(swapchain.format.format, u32(swapchain.images.size()));
}

void Present(VkImage image, u32 width, u32 height, DisplayFormat format) {
    Show(image, width, height, format, true);
}

void PresentAgain() {
    Show(VK_NULL_HANDLE, 0, 0, DisplayFormat::Srgb, false);
}

} // namespace Platform
