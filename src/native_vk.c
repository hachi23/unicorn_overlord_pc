/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The native renderer's Vulkan side (see native_gfx.h): draws a recorded frame with its own
 * device, images and hand-written shaders. It knows nothing of the console's command buffers.
 *
 * For now it draws off screen beside the existing renderer, for comparison:
 *   VW_NATIVE_SHOT_TRIGGER  when this file appears, the frame's display image is written as a
 *                           PNG into VW_NATIVE_DUMP_DIR
 *   VW_NATIVE_SHOT_TARGET   (hex address) write that render target instead */
#define _GNU_SOURCE
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include <pthread.h>
#include <sys/stat.h>
#include "../gpu/vwgpu.h"
#include "native_gfx.h"
#include "native_gnm.h"
#include "runtime.h"

/* A game shader converted for this renderer: <shader dir>/<hash>.spv, written from the player's
 * own executable by the recompiler's native mode (tools/native/aot). Loaded on first use. */
typedef struct { uint32_t hash; int vertex; VkShaderModule module; } NativeProgram;
enum { MAX_PROGRAMS = 4096 };
static NativeProgram programs[MAX_PROGRAMS];
static uint32_t program_count;
static char shader_dir[1024];
/* Bumped when the converter's output changes: old conversions are then redone. */
#define CONVERTER_VERSION 2

/* VW_NATIVE_SHADERS=<dir> uses converted shaders from there as they are; otherwise they live in
 * <user>/native_shaders, converted from the dump's eboot.bin when the stamp there does not
 * match it (first start, another game version or a new converter: about 2 seconds). */
static uint64_t clock_ns(void);
/* 0 when there are no converted shaders to draw with. */
static int shaders_prepare(void) {
    const char *given = getenv("VW_NATIVE_SHADERS");
    if (given) { snprintf(shader_dir, sizeof(shader_dir), "%s", given); return 1; }
    snprintf(shader_dir, sizeof(shader_dir), "%s/native_shaders", runtime_file_user_dir());
    char eboot[1024], stamp_path[1100], stamp[128] = "", want[128];
    snprintf(eboot, sizeof(eboot), "%s/eboot.bin", runtime_file_app_dir());
    struct stat st;
    if (stat(eboot, &st)) { printf("Native: no %s to convert shaders from\n", eboot); return 0; }
    snprintf(want, sizeof(want), "%d %lld %lld", CONVERTER_VERSION, (long long)st.st_size, (long long)st.st_mtime);
    snprintf(stamp_path, sizeof(stamp_path), "%s/stamp", shader_dir);
    FILE *f = fopen(stamp_path, "r");
    if (f) { if (!fgets(stamp, sizeof(stamp), f)) stamp[0] = 0; fclose(f); }
    if (!strcmp(stamp, want)) return 1;
    const uint64_t start = clock_ns();
    const int converted = vwgpu_native_convert(eboot, shader_dir, 1);
    printf("Native: converted %d shaders from %s in %.1f s\n", converted, eboot, (double)(clock_ns() - start) / 1e9);
    if (converted <= 0) return 0;
    if ((f = fopen(stamp_path, "w"))) { fputs(want, f); fclose(f); }
    return 1;
}

/* The features the converted shaders and the bindless tables use; the presenter's device
 * enables each one its GPU has (vk_instance.cpp). 0, naming the first one missing, otherwise. */
static int features_present(VkPhysicalDevice gpu) {
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vkGetPhysicalDeviceFeatures2(gpu, &f);
    VkPhysicalDeviceSubgroupProperties subgroup = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 p = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &subgroup};
    vkGetPhysicalDeviceProperties2(gpu, &p);
    const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    const struct { int present; const char *name; } needed[] = {
        {p.properties.apiVersion >= VK_API_VERSION_1_3, "Vulkan 1.3"},
        {f.features.shaderInt64, "shaderInt64"},
        {f12.bufferDeviceAddress, "bufferDeviceAddress"},
        {f12.runtimeDescriptorArray, "runtimeDescriptorArray"},
        {f12.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound"},
        {f12.descriptorBindingSampledImageUpdateAfterBind, "descriptorBindingSampledImageUpdateAfterBind"},
        {f13.dynamicRendering, "dynamicRendering"},
        {f13.shaderDemoteToHelperInvocation, "shaderDemoteToHelperInvocation"},
        {(subgroup.supportedStages & stages) == stages && (subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT),
         "subgroup ballot in vertex and pixel shaders"},
    };
    for (size_t i = 0; i < sizeof(needed) / sizeof(*needed); ++i)
        if (!needed[i].present) {
            printf("Native: %s has no %s\n", p.properties.deviceName, needed[i].name);
            return 0;
        }
    return 1;
}

#define CHECK(call) do { VkResult check_ = (call); if (check_ != VK_SUCCESS) { \
    fprintf(stderr, "Native: %s failed (%d) at %s:%d\n", #call, (int)check_, __FILE__, __LINE__); abort(); } } while (0)

enum { MAX_TARGETS = 1024, MAX_TEXTURES = 2048, MAX_PIPELINES = 512, MAX_SAMPLERS = 64, MAX_STAGING = 256,
       LOCATIONS = 16, CONSTANT_RANGE = 4096, GAME_SLOTS = 16, TEXTURE_SLOTS = 8, FRAMES = 2,
       MAX_IMAGES = MAX_TARGETS + MAX_TEXTURES + 16 };

/* index: the image's place in the bindless texture array (set 1, binding 0). */
typedef struct { VkImage image; VkDeviceMemory memory; VkImageView view; uint32_t index; } Image;
typedef struct { VkBuffer buffer; VkDeviceMemory memory; uint8_t *mapped; VkDeviceSize size, used; VkDeviceAddress address; } Ring;
/* Each draw's record (Info::NativeLayout in the recompiler): per texture slot the bindless
 * texture index (low 20 bits) and sampler index (high 12 bits); per stage and game constant slot
 * the GPU address of its block (an unbound slot reads the ring's zeroed start); per vertex input
 * its stream's address, stride and format (stream_code). The draw's firstInstance is its
 * record's index; the push constant is the address of the frame's records. One draw is then one
 * vkCmdDrawIndexed. */
enum { RECORD_STREAMS = 16, MAX_RECORDS = 16384 };
typedef struct { uint32_t address_low, address_high, stride, format; } RecordStream;
typedef struct {
    uint32_t slot[TEXTURE_SLOTS];
    VkDeviceAddress block[2][GAME_SLOTS];
    RecordStream stream[RECORD_STREAMS];
} Record;
_Static_assert(sizeof(Record) == 544, "Record must match Info::NativeLayout in the recompiler");
typedef struct { VkDeviceAddress records; } Push;
typedef struct { uint64_t address; uint32_t width, height, swap, tiling; Image image; } Target;
/* source: the descriptor it was made from; span: the bytes of game memory it reads; hash: of
 * that memory when last uploaded (the game reuses memory for new content, see textures_check). */
typedef struct {
    uint64_t address; uint32_t width, height, format, number, tiling; uint8_t swizzle[4]; Image image;
    GnmTexture source; size_t span; uint64_t hash;
} Texture;
typedef struct { uint32_t vs, ps, blend, mask, topology; VkPipeline pipeline; } Pipeline;

static VkInstance instance;
static VkPhysicalDevice physical;
static VkDevice device;
static VkQueue queue;
static int shared_device;   /* the presenter's device (vwgpu_vulkan_handles), queue shared with it */
static VkFence once_fence;
/* Submits to the queue; the presenter submits to it from other threads under the same lock. */
static void queue_submit(const VkSubmitInfo *submit, VkFence fence) {
    if (shared_device) vwgpu_queue_lock();
    const VkResult result = vkQueueSubmit(queue, 1, submit, fence);
    if (shared_device) vwgpu_queue_unlock();
    CHECK(result);
}
static VkCommandPool pool;
static VkPhysicalDeviceMemoryProperties memory_properties;
/* One descriptor set: every texture and sampler (bindless), written once when each is created.
 * Constants are read through GPU addresses into the frame's ring (push constants). */
static VkDescriptorSetLayout bindless_layout;
static VkPipelineLayout pipeline_layout;
/* The driver's compiled pipelines, kept in <shader dir>/pipelines.bin: a shader's first use then
 * stutters only once per machine, not once per start. Saved every few seconds after new ones. */
static VkPipelineCache pipeline_cache;
static int pipeline_cache_dirty;
static void pipeline_cache_open(void) {
    char path[1100];
    snprintf(path, sizeof(path), "%s/pipelines.bin", shader_dir);
    void *data = NULL;
    long bytes = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        bytes = ftell(f);
        fseek(f, 0, SEEK_SET);
        data = bytes > 0 ? malloc((size_t)bytes) : NULL;
        if (!data || fread(data, 1, (size_t)bytes, f) != (size_t)bytes) bytes = 0;
        fclose(f);
    }
    /* Data from another driver or GPU is ignored by the driver (its header says whose it is). */
    const VkPipelineCacheCreateInfo info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .initialDataSize = (size_t)bytes, .pInitialData = bytes ? data : NULL};
    if (vkCreatePipelineCache(device, &info, NULL, &pipeline_cache) != VK_SUCCESS) {
        const VkPipelineCacheCreateInfo empty = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        CHECK(vkCreatePipelineCache(device, &empty, NULL, &pipeline_cache));
    }
    free(data);
}
static void pipeline_cache_save(void) {
    size_t bytes = 0;
    if (vkGetPipelineCacheData(device, pipeline_cache, &bytes, NULL) != VK_SUCCESS || !bytes) return;
    void *data = malloc(bytes);
    if (data && vkGetPipelineCacheData(device, pipeline_cache, &bytes, data) == VK_SUCCESS) {
        char path[1100], temp[1110];
        snprintf(path, sizeof(path), "%s/pipelines.bin", shader_dir);
        snprintf(temp, sizeof(temp), "%s.tmp", path);
        FILE *f = fopen(temp, "wb");
        if (f) {
            const int ok = fwrite(data, 1, bytes, f) == bytes;
            if (fclose(f) == 0 && ok) {
#ifdef _WIN32
                remove(path);   /* rename does not replace an existing file there */
#endif
                rename(temp, path);
            }
        }
    }
    free(data);
    pipeline_cache_dirty = 0;
}
static VkDescriptorPool bindless_pool;
static VkDescriptorSet bindless_set;
static uint32_t image_count;
/* A frame being recorded or in flight: the GPU may still read its rings and staging buffers
 * until its fence signals. */
typedef struct {
    VkCommandBuffer cmd;
    VkFence fence;
    Ring constants, vertex, index, records, indirect;
    uint32_t staging_count, record_count;
    struct { VkBuffer buffer; VkDeviceMemory memory; } staging[MAX_STAGING];
} Frame;
static Frame frames[FRAMES];
static Frame *frame;
static VkCommandBuffer cmd, once_cmd;
#define constants_ring (frame->constants)
#define vertex_ring (frame->vertex)
#define index_ring (frame->index)
static Target targets[MAX_TARGETS];
static Texture textures[MAX_TEXTURES];
static Pipeline pipelines[MAX_PIPELINES];
static struct { uint32_t key[2]; VkSampler sampler; } samplers[MAX_SAMPLERS];
static uint32_t target_count, texture_count, pipeline_count, sampler_count;
#define staging_count (frame->staging_count)
#define staging (frame->staging)
static Image dummy_texture;
static VkSampler dummy_sampler;     /* sampler index 0 */
static uint64_t frame_number, skipped_draws, drawn_draws, replay_ns, wait_ns, texture_reloads;
/* Set when this frame logged something new: the frame is then dumped as text (up to 32 times). */
static int report, reports;
static uint64_t clock_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
/* Why draws were skipped, for the statistics line. */
enum { SKIP_NO_TARGET, SKIP_MASK, SKIP_EMPTY, SKIP_SHADER, SKIP_FORMAT, SKIP_PRIM, SKIP_SCISSOR, SKIP_COUNT };
static const char *const skip_names[SKIP_COUNT] = {"no target", "mask 0", "empty", "shader", "input format", "primitive", "scissor"};
static uint64_t skips[SKIP_COUNT];
static uint32_t skipped_prims[8];
/* VW_NATIVE_WATCH=<hex address>: log the first draws into that target and why any was skipped
 * (debugging aid). */
static uint64_t watch_address;
static int watch_logged;
#define WATCHED() (watch_address && state.target && state.target->address == watch_address && watch_logged < 200)
#define SKIP(why) do { ++skipped_draws; ++skips[why]; \
    if (WATCHED()) { ++watch_logged; printf("Native: watch frame %" PRIu64 " vs %08x ps %08x skipped (%s)\n", frame_number, state.vs, state.ps, skip_names[why]); } \
    return; } while (0)

/* ---- Memory, images, buffers ---- */
static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & flags) == flags) return i;
    fprintf(stderr, "Native: no memory type for %x\n", (unsigned)flags);
    abort();
}
static Image image_create(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, const uint8_t *swizzle) {
    Image image;
    const VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = format, .extent = {width, height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
    CHECK(vkCreateImage(device, &info, NULL, &image.image));
    VkMemoryRequirements need;
    vkGetImageMemoryRequirements(device, image.image, &need);
    const VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = need.size,
        .memoryTypeIndex = memory_type(need.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    CHECK(vkAllocateMemory(device, &allocate, NULL, &image.memory));
    CHECK(vkBindImageMemory(device, image.image, image.memory, 0));
    VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (swizzle) {
        /* The game's channel selects: 0 zero, 1 one, 4..7 x, y, z, w. */
        VkComponentSwizzle *out[4] = {&view.components.r, &view.components.g, &view.components.b, &view.components.a};
        for (int i = 0; i < 4; ++i)
            *out[i] = swizzle[i] == 0 ? VK_COMPONENT_SWIZZLE_ZERO : swizzle[i] == 1 ? VK_COMPONENT_SWIZZLE_ONE :
                      swizzle[i] == 4 ? VK_COMPONENT_SWIZZLE_R : swizzle[i] == 5 ? VK_COMPONENT_SWIZZLE_G :
                      swizzle[i] == 6 ? VK_COMPONENT_SWIZZLE_B : VK_COMPONENT_SWIZZLE_A;
    }
    CHECK(vkCreateImageView(device, &view, NULL, &image.view));
    image.index = 0;
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
        if (image_count == MAX_IMAGES) { fprintf(stderr, "Native: more than %d images\n", MAX_IMAGES); abort(); }
        image.index = image_count++;
        const VkDescriptorImageInfo info = {.imageView = image.view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        const VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = bindless_set,
            .dstBinding = 0, .dstArrayElement = image.index, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &info};
        vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    }
    return image;
}
static void sampler_register(VkSampler sampler, uint32_t index) {
    const VkDescriptorImageInfo info = {.sampler = sampler};
    const VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = bindless_set,
        .dstBinding = 1, .dstArrayElement = index, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
        .pImageInfo = &info};
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
}
static void buffer_create(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buffer, VkDeviceMemory *memory, uint8_t **mapped) {
    const VkMemoryAllocateFlagsInfo flags = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                             .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    const VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    CHECK(vkCreateBuffer(device, &info, NULL, buffer));
    VkMemoryRequirements need;
    vkGetBufferMemoryRequirements(device, *buffer, &need);
    const VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) ? &flags : NULL, .allocationSize = need.size,
        .memoryTypeIndex = memory_type(need.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    CHECK(vkAllocateMemory(device, &allocate, NULL, memory));
    CHECK(vkBindBufferMemory(device, *buffer, *memory, 0));
    void *p;
    CHECK(vkMapMemory(device, *memory, 0, size, 0, &p));
    *mapped = p;
}
static void ring_create(Ring *ring, VkDeviceSize size, VkBufferUsageFlags usage) {
    ring->size = size;
    ring->used = 0;
    buffer_create(size, usage, &ring->buffer, &ring->memory, &ring->mapped);
    ring->address = 0;
    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        const VkBufferDeviceAddressInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = ring->buffer};
        ring->address = vkGetBufferDeviceAddress(device, &info);
    }
}
/* Copies bytes into the ring, leaving `slack` readable bytes after them; the offset. */
static VkDeviceSize ring_put(Ring *ring, const void *data, size_t bytes, size_t align, size_t slack) {
    VkDeviceSize at = (ring->used + align - 1) & ~(VkDeviceSize)(align - 1);
    if (at + bytes + slack > ring->size) {
        fprintf(stderr, "Native: a frame needs more than %llu MiB of %s data\n", (unsigned long long)(ring->size >> 20),
                ring == &frame->vertex ? "vertex" : ring == &frame->index ? "index" : "constant");
        abort();
    }
    if (bytes) memcpy(ring->mapped + at, data, bytes);
    ring->used = at + bytes;
    return at;
}
static void to_general(VkImage image, VkImageLayout from) {
    const VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = from,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
        .srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                         VK_ACCESS_TRANSFER_WRITE_BIT,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, from == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
}

/* ---- Address maps: game address -> index + 1 (0: empty), open addressing ---- */
enum { MAP_SIZE = 8192 };
typedef struct { uint64_t key[MAP_SIZE]; uint32_t value[MAP_SIZE]; } Map;
static uint32_t map_slot(const Map *m, uint64_t key) {
    uint32_t h = (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> 51) & (MAP_SIZE - 1);
    while (m->value[h] && m->key[h] != key) h = (h + 1) & (MAP_SIZE - 1);
    return h;
}
static uint32_t map_get(const Map *m, uint64_t key) { return m->value[map_slot(m, key)]; }
static void map_set(Map *m, uint64_t key, uint32_t value) {
    const uint32_t h = map_slot(m, key);
    m->key[h] = key;
    m->value[h] = value;
}
static Map target_map, texture_map, pipeline_map, program_map;

/* ---- Rendering state while a frame is replayed ---- */
static struct {
    Target *target;          /* the bound colour target, NULL when none */
    Target *rendering;       /* the target a render pass is open on */
    Target *display;         /* the last display image bound this frame */
    uint32_t mask, blend, prim, vs, ps;
    int vs_program, ps_program;   /* indices into programs, -1 when there is none */
    int32_t viewport[4], scissor[4];
    uint8_t texture[TEXTURE_SLOTS][32], sampler[TEXTURE_SLOTS][16];
    int texture_present[TEXTURE_SLOTS];
    /* Bindless indices, resolved at the first draw after a bind (dirty bit per slot). */
    uint32_t texture_index[TEXTURE_SLOTS], sampler_index[TEXTURE_SLOTS], dirty;
    uint32_t constants[2][GAME_SLOTS]; /* vertex, pixel stage: offsets into the frame's constant ring; 0 is zeros */
} state;

/* Images stay in GENERAL; one global barrier orders a pass's writes (or a clear's) before any
 * later read or write. */
static void barrier(void) {
    const VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &b, 0, NULL, 0, NULL);
}
/* What was last recorded into the command buffer, to skip repeating it. Reset per frame. */
static struct {
    VkPipeline pipeline;
    VkViewport viewport;
    VkRect2D scissor;
    int valid;
} emitted;

/* Draws are recorded as indirect commands; consecutive ones with the same pipeline, viewport,
 * scissor and target go to the GPU as one vkCmdDrawIndexedIndirect. Anything else recorded into
 * the command buffer flushes the batch first. */
static uint32_t batch_first, batch_count;
static void flush_draws(void) {
    if (!batch_count) return;
    vkCmdDrawIndexedIndirect(cmd, frame->indirect.buffer, (VkDeviceSize)batch_first * sizeof(VkDrawIndexedIndirectCommand),
                             batch_count, sizeof(VkDrawIndexedIndirectCommand));
    batch_count = 0;
}
static void end_rendering(void) {
    flush_draws();
    if (state.rendering) { vkCmdEndRendering(cmd); barrier(); }
    state.rendering = NULL;
}
static void begin_rendering(Target *target) {
    if (state.rendering == target) return;
    end_rendering();
    const VkRenderingAttachmentInfo color = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = target->image.view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    const VkRenderingInfo info = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, {target->width, target->height}}, .layerCount = 1,
        .colorAttachmentCount = 1, .pColorAttachments = &color};
    vkCmdBeginRendering(cmd, &info);
    state.rendering = target;
}

static Target *target_of(uint64_t address) {
    const uint32_t i = map_get(&target_map, address);
    return i ? &targets[i - 1] : NULL;
}
static Target *target_get(const GnmTarget *t) {
    Target *target = target_of(t->address);
    if (target && target->width == t->width && target->height == t->height) return target;
    if (!t->width || !t->height) return NULL;
    end_rendering();
    if (target) {
        /* The game reuses the memory for another size: the old image is dropped with the device. */
        target->address = ~0ull;
    }
    if (target_count == MAX_TARGETS) { fprintf(stderr, "Native: more than %d render targets\n", MAX_TARGETS); abort(); }
    target = &targets[target_count++];
    map_set(&target_map, t->address, target_count);
    target->address = t->address; target->width = t->width; target->height = t->height;
    target->swap = t->swap; target->tiling = t->tiling;
    if (getenv("VW_NATIVE_LOG_TARGETS"))
        printf("Native: target %ux%u tiling %u at %" PRIx64 " (frame %" PRIu64 ")\n", t->width, t->height, t->tiling,
               t->address, frame_number);
    target->image = image_create(t->width, t->height, VK_FORMAT_R8G8B8A8_UNORM,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, NULL);
    to_general(target->image.image, VK_IMAGE_LAYOUT_UNDEFINED);
    const VkClearColorValue zero = {{0, 0, 0, 0}};
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, target->image.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    barrier();
    return target;
}

/* ---- Textures ---- */
/* Bytes of a texture element (a pixel, or a 4x4 block) and the Vulkan format; 0 when unknown. */
static uint32_t texture_format(uint32_t format, uint32_t number, VkFormat *out, int *block) {
    const int srgb = number == GNM_NUM_SRGB;
    *block = 0;
    switch (format) {
    case GNM_FMT_8_8_8_8: *out = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM; return 4;
    case GNM_FMT_8: *out = VK_FORMAT_R8_UNORM; return 1;
    case GNM_FMT_BC1: *block = 1; *out = srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK; return 8;
    case GNM_FMT_BC2: *block = 1; *out = srgb ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK; return 16;
    case GNM_FMT_BC3: *block = 1; *out = srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK; return 16;
    case GNM_FMT_BC4: *block = 1; *out = VK_FORMAT_BC4_UNORM_BLOCK; return 8;
    case GNM_FMT_BC5: *block = 1; *out = VK_FORMAT_BC5_UNORM_BLOCK; return 16;
    case GNM_FMT_BC7: *block = 1; *out = srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK; return 16;
    default: return 0;
    }
}
/* Tile mode 13 (Thin_1dThin): 8x8-element micro tiles in rows of `pitch` elements; inside a
 * tile the elements are in Morton order (x0 y0 x1 y1 x2 y2), whatever the element size. An
 * element is a pixel, or a 4x4 block for BC formats. This lays them out as plain rows. */
static void untile(const uint8_t *src, uint8_t *dst, uint32_t width, uint32_t height, uint32_t pitch, uint32_t bytes) {
    static uint8_t morton[64];
    if (!morton[63])
        for (uint32_t i = 0; i < 64; ++i) {
            const uint32_t x = (i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4), y = ((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4);
            morton[y * 8 + x] = (uint8_t)i;
        }
    const uint32_t tiles_x = (pitch + 7) / 8;
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t *out = dst + (size_t)y * width * bytes;
        for (uint32_t x = 0; x < width; ++x) {
            const size_t tile = (size_t)(y / 8) * tiles_x + x / 8;
            memcpy(out + (size_t)x * bytes, src + (tile * 64 + morton[(y & 7) * 8 + (x & 7)]) * bytes, bytes);
        }
    }
}
/* A fast hash of game memory (8 bytes at a time). */
static uint64_t memory_hash(const uint8_t *p, size_t n) {
    uint64_t h = 0x9e3779b97f4a7c15ull ^ n, w;
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t a, b, c, d;
        memcpy(&a, p + i, 8); memcpy(&b, p + i + 8, 8); memcpy(&c, p + i + 16, 8); memcpy(&d, p + i + 24, 8);
        h = (h ^ (a + (b << 1) + (c << 2) + (d << 3)) ^ (a >> 29)) * 0xbf58476d1ce4e5b9ull;
    }
    for (; i + 8 <= n; i += 8) { memcpy(&w, p + i, 8); h = (h ^ w) * 0xbf58476d1ce4e5b9ull; }
    return h;
}
/* A sample of game memory: 64 bytes of every 16 KiB (and the last 64). New content replaces a
 * whole texture, so the sample changes with it; each sample is a cache and TLB miss, so few. */
static uint64_t memory_sample_hash(const uint8_t *p, size_t n) {
    uint64_t h = 0x9e3779b97f4a7c15ull ^ n;
    for (size_t i = 0; i + 64 <= n; i += 16384) h = memory_hash(p + i, 64) ^ (h * 0xbf58476d1ce4e5b9ull);
    if (n >= 64) h = memory_hash(p + n - 64, 64) ^ (h * 0xbf58476d1ce4e5b9ull);
    return h;
}
/* Copies a texture's game memory into its image (detiled). The caller ends any open pass. */
static void texture_fill(Texture *x) {
    const GnmTexture *t = &x->source;
    VkFormat format;
    int block;
    const uint32_t bytes = texture_format(t->format, t->number, &format, &block);
    const uint32_t width = block ? (t->width + 3) / 4 : t->width, height = block ? (t->height + 3) / 4 : t->height;
    const uint32_t pitch = block ? (t->pitch + 3) / 4 : t->pitch;
    const VkDeviceSize size = (VkDeviceSize)width * height * bytes;
    const uint8_t *source = (const uint8_t *)(uintptr_t)t->address;
    x->hash = memory_sample_hash(source, x->span);
    uint8_t *mapped;
    buffer_create(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging[staging_count].buffer, &staging[staging_count].memory, &mapped);
    if (t->tiling == 13) {
        untile(source, mapped, width, height, pitch, bytes);
    } else {
        for (uint32_t y = 0; y < height; ++y)
            memcpy(mapped + (size_t)y * width * bytes, source + (size_t)y * pitch * bytes, (size_t)width * bytes);
    }
    const VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .imageExtent = {t->width, t->height, 1}};
    vkCmdCopyBufferToImage(cmd, staging[staging_count].buffer, x->image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    barrier();
    ++staging_count;
}
/* The game loads new content into memory a texture was made from (the location emblem goes
 * into a UI atlas after the atlas is first drawn). Each frame, before replay, a few textures
 * are hashed again, round robin, up to a byte budget; a changed one is uploaded again into the
 * same image, so its bindless index stays. */
static void textures_check(void) {
    enum { BUDGET = 64 << 20 };   /* bytes of texture covered per frame (4096 samples) */
    static uint32_t cursor;
    size_t hashed = 0;
    int barrier_done = 0;
    for (uint32_t n = 0; n < texture_count && hashed < BUDGET && staging_count < MAX_STAGING; ++n) {
        if (cursor >= texture_count) cursor = 0;
        Texture *x = &textures[cursor++];
        hashed += x->span;
        if (memory_sample_hash((const uint8_t *)(uintptr_t)x->address, x->span) == x->hash) continue;
        if (!barrier_done) {
            /* Earlier frames' reads of the image come before the copy. */
            const VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
                                       .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0, NULL, 0, NULL);
            barrier_done = 1;
        }
        texture_fill(x);
        ++texture_reloads;
    }
}
static uint32_t texture_index(const uint8_t raw[32]) {
    GnmTexture t;
    gnm_texture(raw, &t);
    Target *target = target_of(t.address);
    if (target) return target->image.index;
    const uint32_t found = map_get(&texture_map, t.address);
    if (found) {
        const Texture *x = &textures[found - 1];
        if (x->width == t.width && x->height == t.height && x->format == t.format && x->number == t.number &&
            x->tiling == t.tiling && !memcmp(x->swizzle, t.swizzle, 4))
            return x->image.index;
    }
    VkFormat format;
    int block;
    const uint32_t bytes = texture_format(t.format, t.number, &format, &block);
    static int warned;
    if (!bytes || (t.tiling != 13 && t.tiling != 8) || texture_count == MAX_TEXTURES || staging_count == MAX_STAGING) {
        if (warned++ < 8)
            printf("Native: texture %" PRIx64 " %ux%u format %u/%u tiling %u not handled yet\n", t.address, t.width,
                   t.height, t.format, t.number, t.tiling);
        return dummy_texture.index;
    }
    Texture *x = &textures[texture_count++];
    map_set(&texture_map, t.address, texture_count);
    x->address = t.address; x->width = t.width; x->height = t.height; x->format = t.format; x->number = t.number;
    x->tiling = t.tiling;
    memcpy(x->swizzle, t.swizzle, 4);
    x->source = t;
    const uint32_t height = block ? (t.height + 3) / 4 : t.height, pitch = block ? (t.pitch + 3) / 4 : t.pitch;
    x->span = (size_t)pitch * (t.tiling == 13 ? (height + 7) & ~7u : height) * bytes;
    x->image = image_create(t.width, t.height, format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, t.swizzle);
    end_rendering();
    to_general(x->image.image, VK_IMAGE_LAYOUT_UNDEFINED);
    texture_fill(x);
    return x->image.index;
}
static uint32_t sampler_index(const uint8_t raw[16]) {
    uint32_t w[4];
    memcpy(w, raw, 16);
    const uint32_t key[2] = {w[0] & 0x3f, w[2] & 0x0ff00000};
    for (uint32_t i = 0; i < sampler_count; ++i)
        if (samplers[i].key[0] == key[0] && samplers[i].key[1] == key[1]) return i;
    if (sampler_count == MAX_SAMPLERS) return 0;
    static const VkSamplerAddressMode modes[8] = {VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER};
    const VkSamplerCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = ((w[2] >> 20) & 3) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = ((w[2] >> 22) & 3) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = modes[w[0] & 7], .addressModeV = modes[(w[0] >> 3) & 7],
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.0f};
    CHECK(vkCreateSampler(device, &info, NULL, &samplers[sampler_count].sampler));
    samplers[sampler_count].key[0] = key[0];
    samplers[sampler_count].key[1] = key[1];
    sampler_register(samplers[sampler_count].sampler, sampler_count);
    return sampler_count++;
}

/* ---- Pipelines ---- */
static int program_of(uint32_t hash, int vertex) {
    const uint64_t key = (uint64_t)hash << 1 | (uint64_t)vertex;
    uint32_t found = map_get(&program_map, key);
    if (!found) {
        if (program_count == MAX_PROGRAMS) return -1;
        NativeProgram *p = &programs[program_count];
        p->hash = hash, p->vertex = vertex, p->module = VK_NULL_HANDLE;
        char path[1100];
        snprintf(path, sizeof(path), "%s/%08x.spv", shader_dir, hash);
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            const long bytes = ftell(f);
            fseek(f, 0, SEEK_SET);
            uint32_t *code = malloc((size_t)bytes);
            if (code && fread(code, 1, (size_t)bytes, f) == (size_t)bytes) {
                const VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                    .codeSize = (size_t)bytes, .pCode = code};
                CHECK(vkCreateShaderModule(device, &info, NULL, &p->module));
            }
            free(code);
            fclose(f);
        }
        if (!p->module)
            report = 1, printf("Native: no converted %s shader %08x (%s)\n", vertex ? "vertex" : "pixel", hash, path);
        map_set(&program_map, key, found = ++program_count);
    }
    return programs[found - 1].module ? (int)found - 1 : -1;
}
static VkBlendFactor blend_factor(uint32_t f) {
    static const VkBlendFactor table[21] = {VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_DST_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_SRC1_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR, VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,
        VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA};
    return f < 21 ? table[f] : VK_BLEND_FACTOR_ONE;
}
static VkBlendOp blend_op(uint32_t f) {
    static const VkBlendOp table[5] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX,
                                       VK_BLEND_OP_REVERSE_SUBTRACT};
    return f < 5 ? table[f] : VK_BLEND_OP_ADD;
}
static VkPipeline pipeline_create(int vs, int ps, VkPrimitiveTopology topology);
/* The pipeline for the draw's shaders and state; the last one is kept, as runs of draws share it. */
static VkPipeline pipeline_get(int vs, int ps, VkPrimitiveTopology topology) {
    static uint32_t last[5];
    static VkPipeline last_pipeline;
    const uint32_t key[5] = {(uint32_t)vs, (uint32_t)ps, state.blend, state.mask, (uint32_t)topology};
    if (last_pipeline && !memcmp(key, last, sizeof(key))) return last_pipeline;
    memcpy(last, key, sizeof(key));
    return last_pipeline = pipeline_create(vs, ps, topology);
}
static VkPipeline pipeline_create(int vs, int ps, VkPrimitiveTopology topology) {
    /* FNV-1a over the key; collisions are resolved by comparing the stored key. */
    uint64_t key = 0xcbf29ce484222325ull;
    const uint32_t words[5] = {(uint32_t)vs, (uint32_t)ps, state.blend, state.mask, (uint32_t)topology};
    for (int i = 0; i < 5; ++i) key = (key ^ words[i]) * 0x100000001b3ull;
    for (uint32_t probe = 0;; ++probe) {
        const uint32_t i = map_get(&pipeline_map, key + probe);
        if (!i) { key += probe; break; }
        const Pipeline *p = &pipelines[i - 1];
        if (p->vs == (uint32_t)vs && p->ps == (uint32_t)ps && p->blend == state.blend && p->mask == state.mask &&
            p->topology == (uint32_t)topology)
            return p->pipeline;
    }
    if (pipeline_count == MAX_PIPELINES) { fprintf(stderr, "Native: more than %d pipelines\n", MAX_PIPELINES); abort(); }
    Pipeline *p = &pipelines[pipeline_count++];
    memset(p, 0, sizeof(*p));
    p->vs = (uint32_t)vs; p->ps = (uint32_t)ps; p->blend = state.blend; p->mask = state.mask;
    p->topology = (uint32_t)topology;
    map_set(&pipeline_map, key, pipeline_count);
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = programs[vs].module, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = programs[ps].module, .pName = "main"}};
    /* The shaders fetch their own vertices from the record's streams. */
    const VkPipelineVertexInputStateCreateInfo input = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = topology};
    const VkPipelineViewportStateCreateInfo viewport = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1};
    const VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
    const VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    const uint32_t b = state.blend;
    const int separate = (b >> 29) & 1;
    const VkPipelineColorBlendAttachmentState attachment = {.blendEnable = (b >> 30) & 1,
        .srcColorBlendFactor = blend_factor(b & 0x1f), .colorBlendOp = blend_op((b >> 5) & 7),
        .dstColorBlendFactor = blend_factor((b >> 8) & 0x1f),
        .srcAlphaBlendFactor = blend_factor(separate ? (b >> 16) & 0x1f : b & 0x1f),
        .alphaBlendOp = blend_op(separate ? (b >> 21) & 7 : (b >> 5) & 7),
        .dstAlphaBlendFactor = blend_factor(separate ? (b >> 24) & 0x1f : (b >> 8) & 0x1f),
        .colorWriteMask = state.mask & 0xf};
    const VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment};
    const VkDynamicState dynamics[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dynamics};
    const VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
    const VkPipelineRenderingCreateInfo rendering = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1, .pColorAttachmentFormats = &color_format};
    const VkPipelineDepthStencilStateCreateInfo depth = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    const VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rendering,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &input, .pInputAssemblyState = &assembly,
        .pViewportState = &viewport, .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pDepthStencilState = &depth, .pColorBlendState = &blend, .pDynamicState = &dynamic, .layout = pipeline_layout};
    CHECK(vkCreateGraphicsPipelines(device, pipeline_cache, 1, &info, NULL, &p->pipeline));
    pipeline_cache_dirty = 1;
    return p->pipeline;
}
/* A vertex stream's format as native_input decodes it: 1..4 that many 32-bit floats, 5 four
 * 8-bit unorm; 0 when not handled. */
static uint32_t stream_code(uint32_t format, uint32_t number) {
    if (number == GNM_NUM_FLOAT) {
        if (format == GNM_FMT_32) return 1;
        if (format == GNM_FMT_32_32) return 2;
        if (format == GNM_FMT_32_32_32) return 3;
        if (format == GNM_FMT_32_32_32_32) return 4;
    }
    if (format == GNM_FMT_8_8_8_8 && number == GNM_NUM_UNORM) return 5;
    return 0;
}

/* ---- Replaying a frame ---- */
static void draw(const uint8_t *payload) {
    const NativeDraw *d = (const NativeDraw *)payload;
    const NativeStream *streams = (const NativeStream *)(d + 1);
    const NativeRange *ranges = (const NativeRange *)(streams + d->stream_count);
    if (!state.target) SKIP(SKIP_NO_TARGET);
    if (!(state.mask & 0xf)) SKIP(SKIP_MASK);
    if (!d->index_count) SKIP(SKIP_EMPTY);
    const int vs = state.vs_program, ps = state.ps_program;
    if (vs < 0 || ps < 0) SKIP(SKIP_SHADER);
    if (frame->record_count == MAX_RECORDS) SKIP(SKIP_FORMAT);
    /* Inputs: each stream feeds the vertex shader input the recorder found for it in the fetch
     * shader; inputs without one read (0, 0, 0, 1). */
    RecordStream inputs[RECORD_STREAMS];
    memset(inputs, 0, sizeof(inputs));
    VkDeviceSize range_at[32];
    for (uint32_t r = 0; r < d->range_count; ++r)
        range_at[r] = ring_put(&vertex_ring, payload + ranges[r].data, ranges[r].bytes, 4, 64);
    for (uint32_t k = 0; k < d->stream_count; ++k) {
        const NativeStream *s = &streams[k];
        const uint32_t location = s->location == NATIVE_NO_LOCATION ? s->slot : s->location;
        if (location >= RECORD_STREAMS) continue;
        const uint32_t code = stream_code(s->format, s->number);
        if (!code) {
            static uint32_t seen[32][3], seen_count;
            const uint32_t key[3] = {state.vs, location, s->format << 8 | s->number};
            uint32_t q = 0;
            while (q < seen_count && memcmp(seen[q], key, sizeof(key))) ++q;
            if (q == seen_count && seen_count < 32) {
                memcpy(seen[seen_count++], key, sizeof(key));
                report = 1, printf("Native: vs %08x input %u has format %u/%u, not handled\n", state.vs, location, s->format, s->number);
            }
            SKIP(SKIP_FORMAT);
        }
        const VkDeviceAddress at = vertex_ring.address + range_at[s->range] + s->offset;
        inputs[location] = (RecordStream){(uint32_t)at, (uint32_t)(at >> 32), s->stride, code};
    }
    /* Indices, always 32-bit in the frame's index buffer (bound once): fans and quads become
     * triangle lists (not every driver draws fans; none draws quads). */
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    const uint8_t *index_data = payload + sizeof(*d) + d->stream_count * sizeof(*streams) + d->range_count * sizeof(*ranges);
    const uint32_t ib = d->index_bytes, n = d->index_count;
    const int fan = state.prim == GNM_PRIM_FAN, quads = state.prim == GNM_PRIM_QUADS;
    if (state.prim == GNM_PRIM_STRIP) topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    if (!fan && !quads && state.prim != GNM_PRIM_LIST && state.prim != GNM_PRIM_STRIP) {
        for (int i = 0; i < 8; ++i)
            if (skipped_prims[i] == state.prim) break;
            else if (!skipped_prims[i]) { skipped_prims[i] = state.prim; report = 1, printf("Native: primitive type %#x not drawn yet\n", state.prim); break; }
        SKIP(SKIP_PRIM);
    }
    const uint32_t index_count = fan ? (n >= 3 ? (n - 2) * 3 : 0) : quads ? n / 4 * 6 : n;
    if (!index_count) SKIP(SKIP_EMPTY);
    const VkDeviceSize index_at = ring_put(&index_ring, NULL, 0, 4, (size_t)index_count * 4);
    uint32_t *out = (uint32_t *)(index_ring.mapped + index_at);
    index_ring.used = index_at + (size_t)index_count * 4;
    if (ib == 4 && !fan && !quads) {
        memcpy(out, index_data, (size_t)n * 4);
    } else {
        uint32_t k = 0;
#define INDEX(i) (ib == 2 ? ((const uint16_t *)index_data)[i] : ((const uint32_t *)index_data)[i])
        if (fan)
            for (uint32_t i = 0; i + 2 < n; ++i) { out[k++] = INDEX(0); out[k++] = INDEX(i + 1); out[k++] = INDEX(i + 2); }
        else if (quads)
            for (uint32_t q = 0; q + 3 < n; q += 4) {
                out[k++] = INDEX(q); out[k++] = INDEX(q + 1); out[k++] = INDEX(q + 2);
                out[k++] = INDEX(q); out[k++] = INDEX(q + 2); out[k++] = INDEX(q + 3);
            }
        else
            for (uint32_t i = 0; i < n; ++i) out[k++] = INDEX(i);
#undef INDEX
    }
    /* Resources. Texture uploads end the open pass, so they come before it begins. A target
     * never samples itself (the game unbinds it; a stale binding reads the dummy instead). */
    for (uint32_t dirty = state.dirty; dirty; dirty &= dirty - 1) {
        const int i = __builtin_ctz(dirty);
        state.texture_index[i] = state.texture_present[i] ? texture_index(state.texture[i]) : dummy_texture.index;
        /* VW_NATIVE_WHITE=<hex address>: that texture reads as white (debugging aid). */
        static uint64_t white = ~0ull;
        if (white == ~0ull) { const char *e = getenv("VW_NATIVE_WHITE"); white = e ? strtoull(e, NULL, 16) : 0; }
        if (white && state.texture_present[i]) {
            GnmTexture t;
            gnm_texture(state.texture[i], &t);
            if (t.address == white) state.texture_index[i] = dummy_texture.index;
        }
        state.sampler_index[i] = state.texture_present[i] ? sampler_index(state.sampler[i]) : 0;
    }
    state.dirty = 0;
    const uint32_t record_index = frame->record_count++;
    Record *record = (Record *)frame->records.mapped + record_index;
    for (int i = 0; i < TEXTURE_SLOTS; ++i)
        record->slot[i] = (state.texture_index[i] == state.target->image.index ? dummy_texture.index : state.texture_index[i]) |
                          state.sampler_index[i] << 20;
    /* Every game constant slot of both stages: the shaders read theirs by slot. */
    for (int stage = 0; stage < 2; ++stage)
        for (int i = 0; i < GAME_SLOTS; ++i)
            record->block[stage][i] = constants_ring.address + state.constants[stage][i];
    memcpy(record->stream, inputs, sizeof(inputs));
    const VkPipeline pipeline = pipeline_get(vs, ps, topology);
    begin_rendering(state.target);
    if (!emitted.valid || emitted.pipeline != pipeline) {
        flush_draws();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        emitted.pipeline = pipeline;
    }

    /* The game's viewport has y up; a negative height keeps its clip space. */
    const float x = (float)state.viewport[0], y = (float)state.viewport[1];
    const float w = (float)(state.viewport[2] - state.viewport[0]), h = (float)(state.viewport[3] - state.viewport[1]);
    const VkViewport viewport = {x, y + h, w, -h, 0.0f, 1.0f};
    if (!emitted.valid || memcmp(&emitted.viewport, &viewport, sizeof(viewport))) {
        flush_draws();
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        emitted.viewport = viewport;
    }
    int32_t sl = state.scissor[0], st = state.scissor[1], sr = state.scissor[2], sb = state.scissor[3];
    if (sl < 0) sl = 0;
    if (st < 0) st = 0;
    if (sr > (int32_t)state.target->width) sr = (int32_t)state.target->width;
    if (sb > (int32_t)state.target->height) sb = (int32_t)state.target->height;
    if (sr <= sl || sb <= st) SKIP(SKIP_SCISSOR);
    const VkRect2D scissor = {{sl, st}, {(uint32_t)(sr - sl), (uint32_t)(sb - st)}};
    if (!emitted.valid || memcmp(&emitted.scissor, &scissor, sizeof(scissor))) {
        flush_draws();
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        emitted.scissor = scissor;
    }
    emitted.valid = 1;
    VkDrawIndexedIndirectCommand *command = (VkDrawIndexedIndirectCommand *)frame->indirect.mapped + record_index;
    *command = (VkDrawIndexedIndirectCommand){index_count, 1, (uint32_t)(index_at / 4), 0, record_index};
    if (!batch_count) batch_first = record_index;
    else if (batch_first + batch_count != record_index) { flush_draws(); batch_first = record_index; }
    ++batch_count;
    ++drawn_draws;
    if (WATCHED()) { ++watch_logged; printf("Native: watch frame %" PRIu64 " vs %08x ps %08x drawn, %u indices\n", frame_number, state.vs, state.ps, index_count); }
}

static void replay(const NativeList *list) {
    /* Every list starts from the library's default state. */
    Target *const display = state.display;
    memset(&state, 0, sizeof(state));
    state.display = display;
    state.mask = 0xf;
    state.prim = 4;
    state.vs_program = state.ps_program = -1;
    state.dirty = (1u << TEXTURE_SLOTS) - 1;
    for (size_t at = 0; at < list->used;) {
        const NativeCommand *command = (const NativeCommand *)(list->data + at);
        const uint32_t *w = (const uint32_t *)(command + 1);
        const uint8_t *payload = (const uint8_t *)w;
        switch (command->op) {
        case NC_TARGET:
            if (w[0] != 0) break;
            if (w[1]) {
                GnmTarget t;
                gnm_target(w + 2, &t);
                state.target = target_get(&t);
                if (state.target && t.tiling == 10) state.display = state.target;
            } else {
                state.target = NULL;
            }
            break;
        case NC_TARGET_MASK: state.mask = w[0]; break;
        case NC_VIEWPORT:
            memcpy(state.viewport, w, 16);
            memcpy(state.scissor, w, 16);
            break;
        case NC_SCISSOR: memcpy(state.scissor, w, 16); break;
        case NC_BLEND: if (w[0] == 0) state.blend = w[1]; break;
        case NC_PRIM_TYPE: state.prim = w[0]; break;
        case NC_VS: state.vs = w[5]; state.vs_program = program_of(w[5], 1); break;
        case NC_PS: state.ps = w[5]; state.ps_program = program_of(w[5], 0); break;
        case NC_TEXTURE:
            if (w[0] == GNM_STAGE_PS && w[1] < TEXTURE_SLOTS) {
                state.texture_present[w[1]] = (int)w[2];
                memcpy(state.texture[w[1]], w + 4, 32);
                state.dirty |= 1u << w[1];
            }
            break;
        case NC_SAMPLER:
            if (w[0] == GNM_STAGE_PS && w[1] < TEXTURE_SLOTS) { memcpy(state.sampler[w[1]], w + 4, 16); state.dirty |= 1u << w[1]; }
            break;
        case NC_CONSTANTS:
            if (w[1] < GAME_SLOTS && w[2] && (w[0] == GNM_STAGE_VS || w[0] == GNM_STAGE_PS))
                state.constants[w[0] == GNM_STAGE_PS][w[1]] =
                    (uint32_t)ring_put(&constants_ring, payload + 24, w[2], 256, CONSTANT_RANGE);
            break;
        case NC_FILL: {
            uint64_t address;
            memcpy(&address, w, 8);
            Target *target = target_of(address);
            if (target) {
                end_rendering();
                const uint32_t v = w[4];
                const VkClearColorValue color = {{(v & 0xff) / 255.0f, ((v >> 8) & 0xff) / 255.0f,
                                                  ((v >> 16) & 0xff) / 255.0f, (v >> 24) / 255.0f}};
                const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdClearColorImage(cmd, target->image.image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
                barrier();
            }
            break;
        }
        case NC_DRAW: draw(payload); break;
        default: break;
        }
        at += sizeof(*command) + command->bytes;
    }
    end_rendering();
}

/* ---- PNG (stored, no compression) ---- */
static uint32_t crc_table[256];
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t bytes) {
    if (!crc_table[1])
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            crc_table[n] = c;
        }
    for (size_t i = 0; i < bytes; ++i) crc = crc_table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    return crc;
}
static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t bytes) {
    const uint8_t length[4] = {(uint8_t)(bytes >> 24), (uint8_t)(bytes >> 16), (uint8_t)(bytes >> 8), (uint8_t)bytes};
    uint32_t crc = crc32_update(0xffffffffu, (const uint8_t *)type, 4);
    crc = ~crc32_update(crc, data, bytes);
    const uint8_t tail[4] = {(uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc};
    fwrite(length, 1, 4, f); fwrite(type, 1, 4, f); fwrite(data, 1, bytes, f); fwrite(tail, 1, 4, f);
}
static void png_write(const char *path, const uint8_t *rgba, uint32_t width, uint32_t height, int swap_red_blue) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const size_t row = (size_t)width * 3 + 1, raw_bytes = row * height;
    uint8_t *raw = malloc(raw_bytes), *z = malloc(raw_bytes + raw_bytes / 65535 * 5 + 16);
    if (!raw || !z) abort();
    for (uint32_t y = 0; y < height; ++y) {
        raw[y * row] = 0;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t *p = rgba + ((size_t)y * width + x) * 4;
            uint8_t *o = raw + y * row + 1 + (size_t)x * 3;
            o[0] = p[swap_red_blue ? 2 : 0]; o[1] = p[1]; o[2] = p[swap_red_blue ? 0 : 2];
        }
    }
    size_t n = 0;
    uint32_t a = 1, b = 0;
    z[n++] = 0x78; z[n++] = 0x01;
    for (size_t at = 0; at < raw_bytes;) {
        const size_t chunk = raw_bytes - at > 65535 ? 65535 : raw_bytes - at;
        z[n++] = at + chunk == raw_bytes;
        z[n++] = (uint8_t)chunk; z[n++] = (uint8_t)(chunk >> 8); z[n++] = (uint8_t)~chunk; z[n++] = (uint8_t)(~chunk >> 8);
        memcpy(z + n, raw + at, chunk);
        for (size_t i = 0; i < chunk; ++i) { a = (a + raw[at + i]) % 65521; b = (b + a) % 65521; }
        n += chunk; at += chunk;
    }
    z[n++] = (uint8_t)(b >> 8); z[n++] = (uint8_t)b; z[n++] = (uint8_t)(a >> 8); z[n++] = (uint8_t)a;
    const uint8_t header[13] = {(uint8_t)(width >> 24), (uint8_t)(width >> 16), (uint8_t)(width >> 8), (uint8_t)width,
                                (uint8_t)(height >> 24), (uint8_t)(height >> 16), (uint8_t)(height >> 8), (uint8_t)height,
                                8, 2, 0, 0, 0};
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    png_chunk(f, "IHDR", header, 13);
    png_chunk(f, "IDAT", z, (uint32_t)n);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
}

/* ---- Handing finished frames to the presenter ---- */
/* With the presenter's device shared, each frame's display image is copied into one of a few
 * images the presenter samples (vk_presenter.cpp PrepareFrame, through native_present_image),
 * found by the display buffer's address. The copy leaves it in SHADER_READ_ONLY_OPTIMAL; the
 * presenter's later submission on the same queue is ordered after it by the barrier here.
 * Images are never destroyed (a new size makes new ones): the presenter caches views by image. */
enum { PRESENTS = 4 };
static struct { VkImage image; VkDeviceMemory memory; uint32_t width, height; int used; } presents[PRESENTS];
static struct { uint64_t address; uint32_t index; } shown[8];
static uint32_t present_next, shown_count;
static int present_pending = -1;
static uint64_t present_pending_address;
static pthread_mutex_t present_mutex = PTHREAD_MUTEX_INITIALIZER;

static void present_copy(const Target *display) {
    if (!shared_device || !display) return;
    const uint32_t k = present_next++ % PRESENTS;
    if (!presents[k].image || presents[k].width != display->width || presents[k].height != display->height) {
        const VkFormat formats[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
        const VkImageFormatListCreateInfo list = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
                                                  .viewFormatCount = 2, .pViewFormats = formats};
        const VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &list,
            .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = {display->width, display->height, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        CHECK(vkCreateImage(device, &info, NULL, &presents[k].image));
        VkMemoryRequirements need;
        vkGetImageMemoryRequirements(device, presents[k].image, &need);
        const VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = need.size,
            .memoryTypeIndex = memory_type(need.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        CHECK(vkAllocateMemory(device, &allocate, NULL, &presents[k].memory));
        CHECK(vkBindImageMemory(device, presents[k].image, presents[k].memory, 0));
        presents[k].width = display->width;
        presents[k].height = display->height;
        presents[k].used = 0;
    }
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkImageMemoryBarrier to_copy = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = presents[k].used ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = presents[k].image, .subresourceRange = range};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_copy);
    const VkImageCopy copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .extent = {display->width, display->height, 1}};
    vkCmdCopyImage(cmd, display->image.image, VK_IMAGE_LAYOUT_GENERAL, presents[k].image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    const VkImageMemoryBarrier to_read = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = presents[k].image, .subresourceRange = range};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &to_read);
    presents[k].used = 1;
    present_pending = (int)k;
    present_pending_address = display->address;
}
/* After the frame's submit: the presenter may use the image from now on. */
static void present_publish(void) {
    if (present_pending < 0) return;
    pthread_mutex_lock(&present_mutex);
    uint32_t i = 0;
    while (i < shown_count && shown[i].address != present_pending_address) ++i;
    if (i == shown_count && shown_count < 8) ++shown_count;
    if (i < 8) { shown[i].address = present_pending_address; shown[i].index = (uint32_t)present_pending; }
    pthread_mutex_unlock(&present_mutex);
    present_pending = -1;
}
int native_present_image(uint64_t address, void **image, uint32_t *width, uint32_t *height) {
    int found = 0;
    pthread_mutex_lock(&present_mutex);
    for (uint32_t i = 0; i < shown_count; ++i)
        if (shown[i].address == address) {
            const uint32_t k = shown[i].index;
            *image = (void *)presents[k].image;
            *width = presents[k].width;
            *height = presents[k].height;
            found = 1;
        }
    pthread_mutex_unlock(&present_mutex);
    return found;
}

/* ---- Frames ---- */
static void begin_commands(VkCommandBuffer c) {
    CHECK(vkResetCommandBuffer(c, 0));
    const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(c, &begin));
}
/* Work outside the frames (start-up, image dumps): submitted and waited for. */
static void once_begin(void) { begin_commands(once_cmd); cmd = once_cmd; }
static void once_end(void) {
    CHECK(vkEndCommandBuffer(once_cmd));
    const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &once_cmd};
    queue_submit(&submit, once_fence);
    CHECK(vkWaitForFences(device, 1, &once_fence, VK_TRUE, UINT64_MAX));
    CHECK(vkResetFences(device, 1, &once_fence));
}
static void shot(const Target *target) {
    const char *dir = getenv("VW_NATIVE_DUMP_DIR");
    char path[4096];
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t *mapped;
    const VkDeviceSize size = (VkDeviceSize)target->width * target->height * 4;
    buffer_create(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &buffer, &memory, &mapped);
    once_begin();
    const VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .imageExtent = {target->width, target->height, 1}};
    vkCmdCopyImageToBuffer(cmd, target->image.image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &copy);
    once_end();
    snprintf(path, sizeof(path), "%s/native_shot_%06" PRIu64 "_%" PRIx64 ".png", dir && *dir ? dir : ".", frame_number,
             target->address);
    if (getenv("VW_NATIVE_SHOT_ALPHA"))   /* debugging aid: the image's alpha as grey */
        for (size_t i = 0; i < (size_t)target->width * target->height; ++i)
            mapped[i * 4] = mapped[i * 4 + 1] = mapped[i * 4 + 2] = mapped[i * 4 + 3];
    png_write(path, mapped, target->width, target->height, 0);
    printf("Native: %ux%u image written to %s\n", target->width, target->height, path);
    fflush(stdout);
    vkDestroyBuffer(device, buffer, NULL);
    vkFreeMemory(device, memory, NULL);
}

/* Replay and target reads both use the device state (cmd, once_cmd): one at a time. */
static pthread_mutex_t render_mutex = PTHREAD_MUTEX_INITIALIZER;
static void render_frame(NativeList *lists);
void native_render_frame(NativeList *lists) {
    pthread_mutex_lock(&render_mutex);
    render_frame(lists);
    pthread_mutex_unlock(&render_mutex);
}

int native_target_read(uint64_t address, void *out, uint32_t left, uint32_t top, uint32_t right,
                       uint32_t bottom, uint32_t pitch) {
    /* The game reads in bands: one copy of the image per target and frame serves them all. */
    static VkBuffer buffer;
    static VkDeviceMemory memory;
    static uint8_t *mapped;
    static VkDeviceSize capacity;
    static uint64_t read_address = ~0ull, read_frame = ~0ull;
    pthread_mutex_lock(&render_mutex);
    const Target *t = target_of(address);
    if (!t || t->address != address || left >= right || top >= bottom || right > t->width || bottom > t->height ||
        pitch < right - left) {
        pthread_mutex_unlock(&render_mutex);
        return 0;
    }
    if (read_address != address || read_frame != frame_number) {
        const VkDeviceSize size = (VkDeviceSize)t->width * t->height * 4;
        if (size > capacity) {
            if (buffer) { vkDestroyBuffer(device, buffer, NULL); vkFreeMemory(device, memory, NULL); }
            buffer_create(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &buffer, &memory, &mapped);
            capacity = size;
        }
        const VkCommandBuffer saved = cmd;
        once_begin();
        barrier();   /* after the frames already submitted on this queue */
        const VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                        .imageExtent = {t->width, t->height, 1}};
        vkCmdCopyImageToBuffer(cmd, t->image.image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &copy);
        once_end();
        cmd = saved;
        read_address = address, read_frame = frame_number;
        static int told;
        if (!told++) printf("Native: the game reads render target %" PRIx64 " (%ux%u) back\n", address, t->width, t->height);
    }
    /* comp_swap: 0 RGBA, 1 BGRA, 2 ABGR, 3 ARGB (memory byte order of the shader's r, g, b, a). */
    static const uint8_t order[4][4] = {{0, 1, 2, 3}, {2, 1, 0, 3}, {3, 2, 1, 0}, {3, 0, 1, 2}};
    const uint8_t *o = order[t->swap & 3];
    for (uint32_t y = top; y < bottom; ++y) {
        const uint8_t *src = mapped + ((size_t)y * t->width + left) * 4;
        uint8_t *dst = (uint8_t *)out + (size_t)(y - top) * pitch * 4;
        for (uint32_t x = 0; x < right - left; ++x, src += 4, dst += 4) {
            dst[0] = src[o[0]]; dst[1] = src[o[1]]; dst[2] = src[o[2]]; dst[3] = src[o[3]];
        }
    }
    pthread_mutex_unlock(&render_mutex);
    return 1;
}

static void render_frame(NativeList *lists) {
    ++frame_number;
    if (pipeline_cache_dirty && frame_number % 300 == 0) pipeline_cache_save();
    /* Wait until the GPU is done with this frame slot's previous use, then reuse it. */
    frame = &frames[frame_number % FRAMES];
    const uint64_t wait_start = clock_ns();
    CHECK(vkWaitForFences(device, 1, &frame->fence, VK_TRUE, UINT64_MAX));
    const uint64_t replay_start = clock_ns();
    wait_ns += replay_start - wait_start;
    CHECK(vkResetFences(device, 1, &frame->fence));
    for (uint32_t i = 0; i < staging_count; ++i) {
        vkDestroyBuffer(device, staging[i].buffer, NULL);
        vkFreeMemory(device, staging[i].memory, NULL);
    }
    staging_count = 0;
    /* Offset 0 of the constants (zeros) is what a shader reads when the game bound nothing. */
    vertex_ring.used = index_ring.used = 0;
    constants_ring.used = CONSTANT_RANGE;
    cmd = frame->cmd;
    begin_commands(cmd);
    /* Earlier frames' writes come before this frame's reads and writes. */
    barrier();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &bindless_set, 0, NULL);
    const Push push = {frame->records.address};
    vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdBindIndexBuffer(cmd, index_ring.buffer, 0, VK_INDEX_TYPE_UINT32);
    frame->record_count = 0;
    if (frame_number == 1) { const char *e = getenv("VW_NATIVE_WATCH"); watch_address = e ? strtoull(e, NULL, 16) : 0; }
    textures_check();
    memset(&state, 0, sizeof(state));
    emitted.valid = 0;
    batch_count = 0;
    for (NativeList *list = lists; list; list = list->next) replay(list);
    end_rendering();
    present_copy(state.display);
    CHECK(vkEndCommandBuffer(cmd));
    const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
    queue_submit(&submit, frame->fence);
    present_publish();
    replay_ns += clock_ns() - replay_start;
    const char *dir = getenv("VW_NATIVE_DUMP_DIR");
    if (report && dir && *dir && reports < 32) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/report_%06" PRIu64 ".txt", dir, frame_number);
        native_dump_frame(lists, path);
        printf("Native: the frame behind the lines above is %s\n", path);
        ++reports;
    }
    report = 0;
    const char *trigger = getenv("VW_NATIVE_SHOT_TRIGGER");
    if (trigger && *trigger) {
        FILE *f = fopen(trigger, "r");
        if (f) {
            fclose(f);
            remove(trigger);
            const char *which = getenv("VW_NATIVE_SHOT_TARGET");
            const Target *target = which && *which ? target_of(strtoull(which, NULL, 16)) : state.display;
            if (target) { CHECK(vkWaitForFences(device, 1, &frame->fence, VK_TRUE, UINT64_MAX)); shot(target); }
            else printf("Native: no image to write (frame %" PRIu64 ")\n", frame_number);
            fflush(stdout);
        }
    }
    if (frame_number % 600 == 0) {
        printf("Native: renderer drew %.0f and skipped %.0f draws per frame (", drawn_draws / 600.0, skipped_draws / 600.0);
        for (int i = 0; i < SKIP_COUNT; ++i)
            if (skips[i]) printf("%s %.1f, ", skip_names[i], skips[i] / 600.0);
        printf("); %u targets, %u textures (%" PRIu64 " reloaded), %u pipelines; replay %.3f ms, GPU wait %.3f ms per frame\n",
               target_count, texture_count, texture_reloads, pipeline_count, replay_ns / 600e6, wait_ns / 600e6);
        replay_ns = wait_ns = texture_reloads = 0;
        /* VW_NATIVE_PEEK=<hex address>: how much of the 256 KiB there is non-zero (debugging aid). */
        const char *peek = getenv("VW_NATIVE_PEEK");
        if (peek) {
            const uint8_t *m = (const uint8_t *)(uintptr_t)strtoull(peek, NULL, 16);
            size_t nonzero = 0;
            for (size_t i = 0; i < 256 * 1024; ++i) nonzero += m[i] != 0;
            printf("Native: memory at %s: %zu of 262144 bytes non-zero, first words %08x %08x %08x %08x\n", peek, nonzero,
                   ((const uint32_t *)m)[0], ((const uint32_t *)m)[1], ((const uint32_t *)m)[4096], ((const uint32_t *)m)[30000]);
        }
        memset(skips, 0, sizeof(skips));
        fflush(stdout);
        drawn_draws = skipped_draws = 0;
    }
    while (lists) { NativeList *next = lists->next; native_list_free(lists); lists = next; }
}

static int device_create(uint32_t *family_out);
static int render_init(uint32_t family);
int native_render_init(void) {
    if (!shaders_prepare()) return 0;
    uint32_t family = 16;
    void *handles[4];
    if (!getenv("VW_NATIVE_OWN_DEVICE") && vwgpu_vulkan_handles(&handles[0], &handles[1], &handles[2], &handles[3], &family)) {
        instance = (VkInstance)handles[0]; physical = (VkPhysicalDevice)handles[1];
        device = (VkDevice)handles[2]; queue = (VkQueue)handles[3];
        if (!features_present(physical)) return 0;
        shared_device = 1;
        vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    } else if (!device_create(&family)) {
        return 0;
    }
    return render_init(family);
}
/* A device of our own (VW_NATIVE_OWN_DEVICE, or no presenter). */
static int device_create(uint32_t *family_out) {
    const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vwport native",
                                   .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    if (vkCreateInstance(&instance_info, NULL, &instance) != VK_SUCCESS) return 0;
    VkPhysicalDevice found[16];
    uint32_t count = 16;
    if (vkEnumeratePhysicalDevices(instance, &count, found) < 0 || !count) return 0;
    const char *pick = getenv("VW_NATIVE_GPU");
    uint32_t wanted = pick ? (uint32_t)atoi(pick) : 0;
    /* By default the first discrete GPU (laptops list the integrated one first). */
    for (uint32_t i = 0; !pick && i < count; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(found[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && p.apiVersion >= VK_API_VERSION_1_3) { wanted = i; break; }
    }
    physical = found[wanted < count ? wanted : 0];
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_3) {
        printf("Native: %s has no Vulkan 1.3\n", properties.deviceName);
        return 0;
    }
    if (!features_present(physical)) return 0;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    VkQueueFamilyProperties families[16];
    uint32_t family_count = 16, family = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families);
    for (uint32_t i = 0; i < family_count && family == 16; ++i)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) family = i;
    if (family == 16) return 0;
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    VkPhysicalDeviceVulkan13Features features13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                                  .dynamicRendering = VK_TRUE};
    VkPhysicalDeviceVulkan12Features features12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &features13, .runtimeDescriptorArray = VK_TRUE, .descriptorBindingPartiallyBound = VK_TRUE,
        .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE, .descriptorIndexing = VK_TRUE,
        .bufferDeviceAddress = VK_TRUE};
    const VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &features12,
                                                .features = {.textureCompressionBC = VK_TRUE, .multiDrawIndirect = VK_TRUE,
                                                             .drawIndirectFirstInstance = VK_TRUE}};
    const VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &features,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info};
    if (vkCreateDevice(physical, &device_info, NULL, &device) != VK_SUCCESS) return 0;
    vkGetDeviceQueue(device, family, 0, &queue);
    *family_out = family;
    return 1;
}
static int render_init(uint32_t family) {
    const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(device, &fence_info, NULL, &once_fence));
    const VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
    CHECK(vkCreateCommandPool(device, &pool_info, NULL, &pool));
    VkCommandBuffer buffers[FRAMES + 1];
    const VkCommandBufferAllocateInfo cmd_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = FRAMES + 1};
    CHECK(vkAllocateCommandBuffers(device, &cmd_info, buffers));
    once_cmd = buffers[FRAMES];
    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDescriptorSetLayoutBinding bindless[2] = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, MAX_IMAGES, both, NULL},
                                                      {1, VK_DESCRIPTOR_TYPE_SAMPLER, MAX_SAMPLERS, both, NULL}};
    const VkDescriptorBindingFlags bindless_flags[2] = {
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT};
    const VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, .bindingCount = 2,
        .pBindingFlags = bindless_flags};
    const VkDescriptorSetLayoutCreateInfo bindless_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &flags_info, .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
        .bindingCount = 2, .pBindings = bindless};
    CHECK(vkCreateDescriptorSetLayout(device, &bindless_info, NULL, &bindless_layout));
    const VkPushConstantRange push_range = {both, 0, sizeof(Push)};
    const VkPipelineLayoutCreateInfo pipeline_layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &bindless_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push_range};
    CHECK(vkCreatePipelineLayout(device, &pipeline_layout_info, NULL, &pipeline_layout));
    const VkDescriptorPoolSize bindless_sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, MAX_IMAGES},
                                                    {VK_DESCRIPTOR_TYPE_SAMPLER, MAX_SAMPLERS}};
    const VkDescriptorPoolCreateInfo bindless_pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT, .maxSets = 1, .poolSizeCount = 2,
        .pPoolSizes = bindless_sizes};
    CHECK(vkCreateDescriptorPool(device, &bindless_pool_info, NULL, &bindless_pool));
    const VkDescriptorSetAllocateInfo bindless_allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = bindless_pool, .descriptorSetCount = 1, .pSetLayouts = &bindless_layout};
    CHECK(vkAllocateDescriptorSets(device, &bindless_allocate, &bindless_set));
    for (int i = 0; i < FRAMES; ++i) {
        Frame *f = &frames[i];
        f->cmd = buffers[i];
        const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        CHECK(vkCreateFence(device, &fence_info, NULL, &f->fence));
        ring_create(&f->constants, 32u << 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        ring_create(&f->vertex, 64u << 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        ring_create(&f->records, MAX_RECORDS * sizeof(Record), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        ring_create(&f->indirect, MAX_RECORDS * sizeof(VkDrawIndexedIndirectCommand), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
        ring_create(&f->index, 16u << 20, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        memset(f->constants.mapped, 0, CONSTANT_RANGE);
    }
    frame = &frames[0];
    const VkSamplerCreateInfo sampler_info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    CHECK(vkCreateSampler(device, &sampler_info, NULL, &dummy_sampler));
    samplers[0].sampler = dummy_sampler;
    samplers[0].key[0] = samplers[0].key[1] = ~0u;
    sampler_register(dummy_sampler, 0);
    sampler_count = 1;
    dummy_texture = image_create(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, NULL);
    once_begin();
    to_general(dummy_texture.image, VK_IMAGE_LAYOUT_UNDEFINED);
    const VkClearColorValue white = {{1, 1, 1, 1}};
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, dummy_texture.image, VK_IMAGE_LAYOUT_GENERAL, &white, 1, &range);
    once_end();
    pipeline_cache_open();
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    printf("Native: renderer on %s (%s)\n", properties.deviceName,
           shared_device ? "the presenter's device" : "its own device");
    return 1;
}
