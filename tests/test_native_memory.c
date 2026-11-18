/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the renderer's resource lifetime code with a deterministic Vulkan memory budget. */
#include <assert.h>
#define vkAllocateMemory test_allocate_memory
#define vkGetBufferMemoryRequirements test_buffer_requirements
#define vkMapMemory test_map_memory
#define vkCreateBuffer test_create_buffer
#define vkBindBufferMemory test_bind_buffer
#define vkGetBufferDeviceAddress test_buffer_address
#define vkDestroyBuffer test_destroy_buffer
#define vkDestroyImageView test_destroy_image_view
#define vkDestroyImage test_destroy_image
#define vkFreeMemory test_free_memory
#define vkWaitForFences test_wait_for_fences
#include "../src/native_vk.c"

static VkDeviceSize available, freed;
static uint32_t waits, destroyed_images, buffers_created;
static uint8_t *mapped_buffer;
static const VkDeviceAddress chunk_address = 0x100000000ull;

VKAPI_ATTR VkResult VKAPI_CALL test_allocate_memory(VkDevice dev, const VkMemoryAllocateInfo *info,
        const VkAllocationCallbacks *callbacks, VkDeviceMemory *memory) {
    (void)dev; (void)callbacks;
    if (info->allocationSize > available) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    available -= info->allocationSize;
    *memory = (VkDeviceMemory)(uintptr_t)info->allocationSize;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL test_free_memory(VkDevice dev, VkDeviceMemory memory,
        const VkAllocationCallbacks *callbacks) {
    (void)dev; (void)callbacks;
    const VkDeviceSize size = (uintptr_t)memory;
    available += size;
    freed += size;
}

VKAPI_ATTR void VKAPI_CALL test_destroy_image(VkDevice dev, VkImage image, const VkAllocationCallbacks *callbacks) {
    (void)dev; (void)image; (void)callbacks;
    ++destroyed_images;
}

VKAPI_ATTR void VKAPI_CALL test_destroy_image_view(VkDevice dev, VkImageView view, const VkAllocationCallbacks *callbacks) {
    (void)dev; (void)view; (void)callbacks;
}

VKAPI_ATTR VkResult VKAPI_CALL test_wait_for_fences(VkDevice dev, uint32_t count, const VkFence *fences,
        VkBool32 all, uint64_t timeout) {
    (void)dev; (void)all; (void)timeout;
    assert(count == 1 && fences == &frames[(frame_number - 1) % FRAMES].fence);
    ++waits;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL test_create_buffer(VkDevice dev, const VkBufferCreateInfo *info,
        const VkAllocationCallbacks *callbacks, VkBuffer *buffer) {
    (void)dev; (void)callbacks;
    *buffer = (VkBuffer)(uintptr_t)info->size;
    ++buffers_created;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL test_buffer_requirements(VkDevice dev, VkBuffer buffer, VkMemoryRequirements *need) {
    (void)dev;
    *need = (VkMemoryRequirements){.size = (uintptr_t)buffer, .memoryTypeBits = 1};
}

VKAPI_ATTR VkResult VKAPI_CALL test_bind_buffer(VkDevice dev, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    (void)dev; (void)buffer; (void)memory; (void)offset;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL test_map_memory(VkDevice dev, VkDeviceMemory memory, VkDeviceSize offset,
        VkDeviceSize size, VkMemoryMapFlags flags, void **mapped) {
    (void)dev; (void)memory; (void)offset; (void)flags;
    mapped_buffer = malloc(size);
    assert(mapped_buffer);
    *mapped = mapped_buffer;
    return VK_SUCCESS;
}

VKAPI_ATTR VkDeviceAddress VKAPI_CALL test_buffer_address(VkDevice dev, const VkBufferDeviceAddressInfo *info) {
    (void)dev; (void)info;
    return chunk_address;
}

VKAPI_ATTR void VKAPI_CALL test_destroy_buffer(VkDevice dev, VkBuffer buffer, const VkAllocationCallbacks *callbacks) {
    (void)dev; (void)buffer; (void)callbacks;
}

/* These entry points belong to replay or initialization, outside these tests. */
void vwgpu_queue_lock(void) { abort(); }
void vwgpu_queue_unlock(void) { abort(); }
void native_dump_frame(NativeList *lists, const char *path) { (void)lists; (void)path; abort(); }
void native_list_free(NativeList *list) { (void)list; abort(); }
int vwgpu_vulkan_handles(void **instance, void **physical, void **device, void **queue, uint32_t *family) {
    (void)instance; (void)physical; (void)device; (void)queue; (void)family;
    abort();
}
const char *runtime_file_user_dir(void) { abort(); }
const char *runtime_file_app_dir(void) { abort(); }
int ns_convert_all(const char *eboot, const char *out_dir) { (void)eboot; (void)out_dir; abort(); }

static Image test_image(uint32_t index, VkDeviceSize bytes) {
    return (Image){.image = (VkImage)(uintptr_t)1, .memory = (VkDeviceMemory)(uintptr_t)bytes,
                   .index = index, .bytes = bytes};
}

static void test_pressure(void) {
    frame_number = 10;
    texture_slots = texture_count = 3;
    texture_bytes = 30;
    for (uint32_t i = 0; i < texture_slots; ++i) {
        textures[i] = (Texture){.address = 0x1000 + i * 0x100, .used = 8 + i, .image = test_image(i, 10)};
        map_set(&texture_map, textures[i].address, i + 1);
    }
    Target *old = &targets[0];
    *old = (Target){.image = test_image(3, 20)};
    frames[9 % FRAMES].retired_targets = old;
    target_count = 1;
    target_bytes = 20;

    VkMemoryAllocateInfo info = {.allocationSize = 10};
    VkDeviceMemory memory;
    allocate_image_memory(&info, &memory);
    assert(freed == 10 && destroyed_images == 1 && waits == 0);
    assert(texture_count == 2 && texture_slot() == &textures[0]);
    assert(map_get(&texture_map, textures[0].address) == 0);
    assert(map_get(&texture_map, textures[2].address) == 3);

    info.allocationSize = 25;
    allocate_image_memory(&info, &memory);
    assert(freed == 40 && destroyed_images == 3 && waits == 1);
    assert(target_count == 0 && !target_bytes && free_targets == old);
    assert(texture_count == 1 && texture_bytes == 10 && textures[2].image.image);
    assert(map_get(&texture_map, textures[2].address) == 3);
}

static void test_vertex_overflow(void) {
    enum { BASE_SIZE = 64 << 20 };
    static uint8_t base[4];
    frame = &frames[0];
    frame->vertex = (Ring){.mapped = base, .size = BASE_SIZE, .used = BASE_SIZE - 64, .address = 0x200000000ull};
    frame->vertex_chunk = &frame->vertex;
    available = 64 << 20;
    memory_properties.memoryTypeCount = 1;
    memory_properties.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const uint8_t bytes[] = {1, 2, 3, 4};
    assert(vertex_put(bytes, sizeof(bytes)) == chunk_address);
    assert(!memcmp(mapped_buffer, bytes, sizeof(bytes)) && buffers_created == 1);
    assert(vertex_put(bytes, sizeof(bytes)) == chunk_address + 4);
    assert(frame->vertex.used == BASE_SIZE - 64);
    frame->vertex_chunk = &frame->vertex;
    assert(vertex_put(bytes, sizeof(bytes)) == chunk_address);
    assert(buffers_created == 1);
    free(mapped_buffer);
    free(frame->vertex.next);
}

int main(void) {
    test_pressure();
    test_vertex_overflow();
    puts("Native renderer memory tests passed");
    return 0;
}
