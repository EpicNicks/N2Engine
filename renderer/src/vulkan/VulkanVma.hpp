#pragma once

// The only way the renderer includes AMD's VulkanMemoryAllocator (external/VulkanMemoryAllocator/vk_mem_alloc.h, MIT).
// PRIVATE: nothing under renderer/include/ may include this file, so VMA types never reach engine code, and a build
// with N2ENGINE_RENDERER_VULKAN=OFF (which does not compile renderer/src/vulkan/) never sees them.
//
// Function import is STATIC: the renderer links the Vulkan loader (Vulkan::Vulkan), so VMA calls the core entry points
// (up to 1.1, the API version the allocator is created for) directly and needs no vkGetInstanceProcAddr plumbing.
// Every translation unit must see the same configuration macros, so they are set here and nowhere else.

#include <vulkan/vulkan.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0

// VMA's own warnings are silenced locally (and its include directory is a SYSTEM one); no global flag changes
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wparentheses"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif

#include <vk_mem_alloc.h>

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
