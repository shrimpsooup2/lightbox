#pragma once

#include <cstdint>
#include <functional>

#include "render/types.hpp"
#include "render/vulkan.hpp"

namespace lb::render {

// Upper bound of simultaneously uploaded material textures (bindless descriptor array size). The
// device limit may lower it (GpuContext::bindlessTextureCapacity).
constexpr std::uint32_t kMaxBindlessTextures = 4096;

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    void* mapped = nullptr; // non-null for host-visible, persistently mapped buffers
    VkDeviceAddress address = 0;
    VkDeviceSize size = 0;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkImageView view = VK_NULL_HANDLE;
};

enum class BufferMemory {
    DeviceLocal, // GPU-only; filled with upload()
    HostWrite,   // persistently mapped, CPU writes sequentially (staging, per-frame data)
    HostRead,    // persistently mapped, CPU reads back GPU results
};

// Instance, physical/logical device, queue and allocator. Internal to lb_render; one per Renderer.
class GpuContext {
public:
    explicit GpuContext(bool enableValidation);
    ~GpuContext();
    GpuContext(const GpuContext&) = delete;
    GpuContext& operator=(const GpuContext&) = delete;

    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, BufferMemory memory);
    void destroyBuffer(Buffer& buffer);
    // Copies `size` bytes into a device-local buffer through a staging buffer (blocking).
    void upload(const Buffer& destination, const void* data, VkDeviceSize size);

    Image createImage2D(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage,
                        VkSampleCountFlagBits samples, VkImageAspectFlags aspect);
    void destroyImage(Image& image);

    // Records and submits a one-off command buffer and waits for it to finish.
    void immediateSubmit(const std::function<void(VkCommandBuffer)>& record);

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    std::uint32_t graphicsQueueFamily = 0;
    VmaAllocator allocator = nullptr;
    VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_1_BIT;
    std::uint32_t bindlessTextureCapacity = 0; // texture slots: min(kMaxBindlessTextures, limits)
    bool samplerAnisotropy = false;            // feature enabled on the device
    float maxSamplerAnisotropy = 1.0f;
    DeviceInfo info;

private:
    void createInstance(bool enableValidation);
    void pickPhysicalDevice();
    void createDevice();
    void createAllocator();

    void destroy();

    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkCommandPool immediatePool_ = VK_NULL_HANDLE;
    VkFence immediateFence_ = VK_NULL_HANDLE;
};

} // namespace lb::render
