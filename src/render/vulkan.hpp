#pragma once

// Single include point for Vulkan inside lb_render. Function pointers come from volk (loaded at
// runtime), memory from VMA. Never include <vulkan/vulkan.h> directly in render code.
#include <volk.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#include <stdexcept>

namespace lb::render {

class VulkanError : public std::runtime_error {
public:
    VulkanError(VkResult result, const char* expression, const char* file, int line);
    VkResult result() const noexcept { return result_; }

private:
    VkResult result_;
};

const char* toString(VkResult result);

} // namespace lb::render

// Throws lb::render::VulkanError for any negative VkResult. Not for per-frame swapchain results that
// need explicit handling (VK_ERROR_OUT_OF_DATE_KHR, VK_SUBOPTIMAL_KHR).
#define LB_VK_CHECK(expr)                                                                          \
    do {                                                                                           \
        const VkResult lbVkResult_ = (expr);                                                       \
        if (lbVkResult_ < 0) {                                                                     \
            throw ::lb::render::VulkanError(lbVkResult_, #expr, __FILE__, __LINE__);               \
        }                                                                                          \
    } while (0)
