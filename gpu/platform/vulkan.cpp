// SPDX-License-Identifier: GPL-2.0-or-later
// The Vulkan instance, window surface and device that the native renderer and the presenter
// share (one graphics queue). The device turns on every core feature the GPU offers except
// robust buffer access (slow, and nothing here reads out of bounds), plus swapchains, push
// descriptors (the post passes) and the memory budget (the performance overlay) when present.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include "platform.h"
#include "sdl_window.h"

namespace Platform {

Vulkan vk;
std::mutex queue_mutex;
Frontend::WindowSDL* window;

namespace {
VkPhysicalDeviceMemoryProperties memory_properties;

bool HasExtension(VkPhysicalDevice gpu, const char* name) {
    u32 count = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> all(count);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, all.data());
    for (const auto& e : all)
        if (!std::strcmp(e.extensionName, name)) return true;
    return false;
}

bool CanPresent(VkPhysicalDevice gpu, u32& family) {
    u32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families.data());
    for (u32 i = 0; i < count; ++i) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, vk.surface, &present);
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
            family = i;
            return true;
        }
    }
    return false;
}
} // namespace

bool CreateVulkan(Frontend::WindowSDL& w) {
    Uint32 extension_count = 0;
    const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&extension_count);
    if (!sdl_extensions) {
        std::printf("Vulkan: no window surface support (%s)\n", SDL_GetError());
        return false;
    }
    std::vector<const char*> extensions(sdl_extensions, sdl_extensions + extension_count);
    std::vector<const char*> layers;
    if (const char* v = std::getenv("VW_VK_VALIDATION"); v && *v == '1') {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    const VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                .pApplicationName = "Unicorn Overlord",
                                .pEngineName = "vwport",
                                .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo instance_info{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                             .pApplicationInfo = &app,
                                             .enabledLayerCount = u32(layers.size()),
                                             .ppEnabledLayerNames = layers.data(),
                                             .enabledExtensionCount = u32(extensions.size()),
                                             .ppEnabledExtensionNames = extensions.data()};
    if (vkCreateInstance(&instance_info, nullptr, &vk.instance) != VK_SUCCESS) {
        std::printf("Vulkan: no instance (is a Vulkan driver installed?)\n");
        return false;
    }
    if (!SDL_Vulkan_CreateSurface(w.GetSDLWindow(), vk.instance, nullptr, &vk.surface)) {
        std::printf("Vulkan: no window surface (%s)\n", SDL_GetError());
        return false;
    }

    // The GPU: VW_GPU_ID (an index), else the first discrete one with Vulkan 1.3 that can show
    // the window (laptops list the integrated GPU first), else the first that can.
    u32 count = 0;
    vkEnumeratePhysicalDevices(vk.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(vk.instance, &count, gpus.data());
    const char* pick = std::getenv("VW_GPU_ID");
    const int wanted = pick && *pick && std::atoi(pick) >= 0 ? std::atoi(pick) : -1;
    int chosen = -1;
    for (int pass = 0; pass < 2 && chosen < 0; ++pass) {
        for (u32 i = 0; i < count && chosen < 0; ++i) {
            if (wanted >= 0 && int(i) != wanted) continue;
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(gpus[i], &p);
            u32 family;
            if (p.apiVersion < VK_API_VERSION_1_3 || !CanPresent(gpus[i], family)) continue;
            if (pass == 0 && wanted < 0 && p.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;
            chosen = int(i);
            vk.family = family;
        }
    }
    if (chosen < 0) {
        std::printf("Vulkan: no GPU with Vulkan 1.3 can show the window%s\n",
                    wanted >= 0 ? " (VW_GPU_ID)" : "");
        return false;
    }
    vk.physical = gpus[chosen];
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(vk.physical, &properties);
    vk.api_version = std::min<u32>(properties.apiVersion, VK_API_VERSION_1_3);
    vk.gpu_name = properties.deviceName;
    vkGetPhysicalDeviceMemoryProperties(vk.physical, &memory_properties);

    // Every feature the GPU has, except robust buffer access.
    VkPhysicalDeviceVulkan13Features f13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12};
    VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11};
    vkGetPhysicalDeviceFeatures2(vk.physical, &features);
    features.features.robustBufferAccess = VK_FALSE;
    f13.robustImageAccess = VK_FALSE;

    std::vector<const char*> device_extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (!HasExtension(vk.physical, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) {
        std::printf("Vulkan: %s has no %s\n", properties.deviceName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
        return false;
    }
    device_extensions.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    vk.memory_budget = HasExtension(vk.physical, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    if (vk.memory_budget) device_extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                             .queueFamilyIndex = vk.family,
                                             .queueCount = 1,
                                             .pQueuePriorities = &priority};
    const VkDeviceCreateInfo device_info{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                         .pNext = &features,
                                         .queueCreateInfoCount = 1,
                                         .pQueueCreateInfos = &queue_info,
                                         .enabledExtensionCount = u32(device_extensions.size()),
                                         .ppEnabledExtensionNames = device_extensions.data()};
    if (const VkResult r = vkCreateDevice(vk.physical, &device_info, nullptr, &vk.device); r != VK_SUCCESS) {
        std::printf("Vulkan: no device on %s (%d)\n", properties.deviceName, int(r));
        return false;
    }
    vkGetDeviceQueue(vk.device, vk.family, 0, &vk.queue);
    std::printf("Vulkan: %s, Vulkan %u.%u.%u\n", properties.deviceName, VK_API_VERSION_MAJOR(properties.apiVersion),
                VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion));
    return true;
}

u32 MemoryType(u32 bits, VkMemoryPropertyFlags flags) {
    for (u32 i = 0; i < memory_properties.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & flags) == flags) return i;
    for (u32 i = 0; i < memory_properties.memoryTypeCount; ++i)
        if (bits & (1u << i)) return i;
    return 0;
}

void MemoryUsage(u64& used, u64& budget) {
    used = budget = 0;
    if (!vk.memory_budget) return;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT b{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 p{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &b};
    vkGetPhysicalDeviceMemoryProperties2(vk.physical, &p);
    for (u32 i = 0; i < p.memoryProperties.memoryHeapCount; ++i) {
        if (!(p.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
        used += b.heapUsage[i];
        budget += b.heapBudget[i];
    }
}

} // namespace Platform
