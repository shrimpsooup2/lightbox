#include "render/gpu_context.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "core/log.hpp"

namespace lb::render {

VulkanError::VulkanError(VkResult result, const char* expression, const char* file, int line)
    : std::runtime_error(std::format("Vulkan call failed with {}: {} ({}:{})", toString(result),
                                     expression, file, line)),
      result_(result) {}

const char* toString(VkResult result) {
    switch (result) {
#define LB_VK_RESULT_CASE(name)                                                                    \
    case name:                                                                                     \
        return #name;
        LB_VK_RESULT_CASE(VK_SUCCESS)
        LB_VK_RESULT_CASE(VK_NOT_READY)
        LB_VK_RESULT_CASE(VK_TIMEOUT)
        LB_VK_RESULT_CASE(VK_INCOMPLETE)
        LB_VK_RESULT_CASE(VK_SUBOPTIMAL_KHR)
        LB_VK_RESULT_CASE(VK_ERROR_OUT_OF_HOST_MEMORY)
        LB_VK_RESULT_CASE(VK_ERROR_OUT_OF_DEVICE_MEMORY)
        LB_VK_RESULT_CASE(VK_ERROR_INITIALIZATION_FAILED)
        LB_VK_RESULT_CASE(VK_ERROR_DEVICE_LOST)
        LB_VK_RESULT_CASE(VK_ERROR_MEMORY_MAP_FAILED)
        LB_VK_RESULT_CASE(VK_ERROR_LAYER_NOT_PRESENT)
        LB_VK_RESULT_CASE(VK_ERROR_EXTENSION_NOT_PRESENT)
        LB_VK_RESULT_CASE(VK_ERROR_FEATURE_NOT_PRESENT)
        LB_VK_RESULT_CASE(VK_ERROR_INCOMPATIBLE_DRIVER)
        LB_VK_RESULT_CASE(VK_ERROR_TOO_MANY_OBJECTS)
        LB_VK_RESULT_CASE(VK_ERROR_FORMAT_NOT_SUPPORTED)
        LB_VK_RESULT_CASE(VK_ERROR_FRAGMENTED_POOL)
        LB_VK_RESULT_CASE(VK_ERROR_OUT_OF_POOL_MEMORY)
        LB_VK_RESULT_CASE(VK_ERROR_SURFACE_LOST_KHR)
        LB_VK_RESULT_CASE(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
        LB_VK_RESULT_CASE(VK_ERROR_OUT_OF_DATE_KHR)
        LB_VK_RESULT_CASE(VK_ERROR_VALIDATION_FAILED_EXT)
#undef LB_VK_RESULT_CASE
    default:
        return "VkResult (unlisted)";
    }
}

namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

// Every window-system surface extension we know about. Enabling all available ones lets the
// windowing layer (Qt) create surfaces on any platform without platform #ifdefs here.
constexpr const char* kSurfaceExtensions[] = {
    "VK_KHR_win32_surface", "VK_KHR_xcb_surface",   "VK_KHR_xlib_surface",
    "VK_KHR_wayland_surface", "VK_EXT_metal_surface",
};

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        log::error("[vulkan] {}", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        log::warn("[vulkan] {}", data->pMessage);
    } else {
        log::debug("[vulkan] {}", data->pMessage);
    }
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT debugMessengerInfo() {
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = debugCallback;
    return info;
}

bool hasExtension(const std::vector<VkExtensionProperties>& available, const char* name) {
    return std::any_of(available.begin(), available.end(), [name](const VkExtensionProperties& e) {
        return std::strcmp(e.extensionName, name) == 0;
    });
}

std::vector<VkExtensionProperties> instanceExtensions() {
    std::uint32_t count = 0;
    LB_VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr));
    std::vector<VkExtensionProperties> result(count);
    LB_VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &count, result.data()));
    return result;
}

std::vector<VkExtensionProperties> deviceExtensions(VkPhysicalDevice device) {
    std::uint32_t count = 0;
    LB_VK_CHECK(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr));
    std::vector<VkExtensionProperties> result(count);
    LB_VK_CHECK(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, result.data()));
    return result;
}

bool hasLayer(const char* name) {
    std::uint32_t count = 0;
    LB_VK_CHECK(vkEnumerateInstanceLayerProperties(&count, nullptr));
    std::vector<VkLayerProperties> layers(count);
    LB_VK_CHECK(vkEnumerateInstanceLayerProperties(&count, layers.data()));
    return std::any_of(layers.begin(), layers.end(), [name](const VkLayerProperties& l) {
        return std::strcmp(l.layerName, name) == 0;
    });
}

std::string formatVersion(std::uint32_t version) {
    return std::format("{}.{}.{}", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
                       VK_API_VERSION_PATCH(version));
}

std::string formatDriverVersion(const VkPhysicalDeviceProperties& props) {
    const std::uint32_t v = props.driverVersion;
    if (props.vendorID == 0x10DE) { // NVIDIA
        return std::format("{}.{}", (v >> 22) & 0x3FF, (v >> 14) & 0xFF);
    }
#ifdef _WIN32
    if (props.vendorID == 0x8086) { // Intel on Windows
        return std::format("{}.{}", v >> 14, v & 0x3FFF);
    }
#endif
    return formatVersion(v);
}

std::uint64_t largestDeviceLocalHeap(VkPhysicalDevice device) {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(device, &memory);
    std::uint64_t largest = 0;
    for (std::uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
        if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            largest = std::max<std::uint64_t>(largest, memory.memoryHeaps[i].size);
        }
    }
    return largest;
}

} // namespace

GpuContext::GpuContext(bool enableValidation) {
    try {
        createInstance(enableValidation);
        pickPhysicalDevice();
        createDevice();
        createAllocator();
    } catch (...) {
        destroy();
        throw;
    }
}

GpuContext::~GpuContext() {
    destroy();
}

void GpuContext::destroy() {
    if (device) {
        vkDeviceWaitIdle(device);
    }
    if (immediateFence_) {
        vkDestroyFence(device, immediateFence_, nullptr);
        immediateFence_ = VK_NULL_HANDLE;
    }
    if (immediatePool_) {
        vkDestroyCommandPool(device, immediatePool_, nullptr);
        immediatePool_ = VK_NULL_HANDLE;
    }
    if (allocator) {
        vmaDestroyAllocator(allocator);
        allocator = nullptr;
    }
    if (device) {
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
    if (debugMessenger_) {
        vkDestroyDebugUtilsMessengerEXT(instance, debugMessenger_, nullptr);
        debugMessenger_ = VK_NULL_HANDLE;
    }
    if (instance) {
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }
}

void GpuContext::createInstance(bool enableValidation) {
    if (volkInitialize() != VK_SUCCESS) {
        throw std::runtime_error(
            "The Vulkan runtime was not found on this computer. Install or update your graphics "
            "driver.");
    }
    const std::uint32_t loaderVersion = volkGetInstanceVersion();
    if (loaderVersion < VK_API_VERSION_1_3) {
        throw std::runtime_error(std::format(
            "LightBox needs Vulkan 1.3, but the installed Vulkan runtime is {}. Update your "
            "graphics driver.",
            formatVersion(loaderVersion)));
    }

    const std::vector<VkExtensionProperties> available = instanceExtensions();
    std::vector<const char*> extensions;
    if (hasExtension(available, VK_KHR_SURFACE_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        for (const char* name : kSurfaceExtensions) {
            if (hasExtension(available, name)) {
                extensions.push_back(name);
            }
        }
    } else {
        log::warn("Vulkan surface extensions unavailable; only headless rendering will work");
    }

    VkInstanceCreateFlags flags = 0;
    if (hasExtension(available, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    std::vector<const char*> layers;
    if (enableValidation) {
        if (hasLayer(kValidationLayer)) {
            layers.push_back(kValidationLayer);
        } else {
            log::warn("Validation requested, but {} is not installed (it ships with the Vulkan SDK)",
                      kValidationLayer);
        }
    }
    const bool useDebugMessenger =
        !layers.empty() && hasExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (useDebugMessenger) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "LightBox";
    app.pEngineName = "LightBox Live";
    app.apiVersion = VK_API_VERSION_1_3;

    const VkDebugUtilsMessengerCreateInfoEXT debugInfo = debugMessengerInfo();
    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pNext = useDebugMessenger ? &debugInfo : nullptr;
    instanceInfo.flags = flags;
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    instanceInfo.ppEnabledLayerNames = layers.data();
    instanceInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    instanceInfo.ppEnabledExtensionNames = extensions.data();
    LB_VK_CHECK(vkCreateInstance(&instanceInfo, nullptr, &instance));
    volkLoadInstance(instance);

    if (useDebugMessenger) {
        LB_VK_CHECK(vkCreateDebugUtilsMessengerEXT(instance, &debugInfo, nullptr, &debugMessenger_));
    }
    log::info("Vulkan instance created (runtime {}, validation {})", formatVersion(loaderVersion),
              layers.empty() ? "off" : "on");
}

void GpuContext::pickPhysicalDevice() {
    std::uint32_t count = 0;
    LB_VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    if (count == 0) {
        throw std::runtime_error(
            "No Vulkan-capable graphics card was found. Install or update your graphics driver.");
    }
    std::vector<VkPhysicalDevice> devices(count);
    LB_VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));

    std::int64_t bestScore = -1;
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(candidate, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) {
            log::info("Skipping GPU '{}': supports Vulkan {} (1.3 required)", props.deviceName,
                      formatVersion(props.apiVersion));
            continue;
        }

        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceVulkan12Features features12{};
        features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        features12.pNext = &features13;
        VkPhysicalDeviceFeatures2 features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &features12;
        vkGetPhysicalDeviceFeatures2(candidate, &features);
        if (!features13.dynamicRendering || !features13.synchronization2 ||
            !features12.bufferDeviceAddress) {
            log::info("Skipping GPU '{}': missing required Vulkan 1.3 features", props.deviceName);
            continue;
        }
        // Bindless material textures (a renderer-wide array of sampled images indexed from the
        // material buffer). All of these are mandatory for Vulkan 1.3 drivers, so this only rejects
        // broken drivers, but the log should say why.
        if (!features12.runtimeDescriptorArray || !features12.descriptorBindingPartiallyBound ||
            !features12.descriptorBindingSampledImageUpdateAfterBind ||
            !features12.descriptorBindingUpdateUnusedWhilePending ||
            !features.features.shaderSampledImageArrayDynamicIndexing) {
            log::info("Skipping GPU '{}': missing descriptor indexing features (bindless textures)",
                      props.deviceName);
            continue;
        }
        VkPhysicalDeviceVulkan12Properties props12{};
        props12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &props12;
        vkGetPhysicalDeviceProperties2(candidate, &props2);
        // Two views per texture (sRGB + linear). The limits count all sets of a pipeline layout,
        // so keep headroom for the sampler, the shadow map and attachments.
        constexpr std::uint32_t kHeadroom = 16;
        const std::uint32_t sampledImageLimit =
            std::min({props12.maxPerStageDescriptorUpdateAfterBindSampledImages,
                      props12.maxDescriptorSetUpdateAfterBindSampledImages,
                      props12.maxPerStageUpdateAfterBindResources});
        const std::uint32_t usable =
            sampledImageLimit > kHeadroom ? (sampledImageLimit - kHeadroom) / 2 : 0u;
        const std::uint32_t textureCapacity = std::min(kMaxBindlessTextures, usable);
        if (textureCapacity < 64) {
            log::info("Skipping GPU '{}': too few bindless texture descriptors ({})",
                      props.deviceName, sampledImageLimit);
            continue;
        }

        const std::vector<VkExtensionProperties> extensions = deviceExtensions(candidate);
        if (!hasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            log::info("Skipping GPU '{}': no swapchain support", props.deviceName);
            continue;
        }

        std::uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
        const auto graphics =
            std::find_if(families.begin(), families.end(), [](const VkQueueFamilyProperties& f) {
                return (f.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            });
        if (graphics == families.end()) {
            continue;
        }

        const std::uint64_t vram = largestDeviceLocalHeap(candidate);
        std::int64_t score = static_cast<std::int64_t>(vram >> 20); // MiB
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            score += 1'000'000;
        } else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            score += 100'000;
        }
        if (score <= bestScore) {
            continue;
        }

        bestScore = score;
        physicalDevice = candidate;
        graphicsQueueFamily = static_cast<std::uint32_t>(std::distance(families.begin(), graphics));
        info = DeviceInfo{};
        info.name = props.deviceName;
        info.vulkanVersion = formatVersion(props.apiVersion);
        info.driverVersion = formatDriverVersion(props);
        info.discreteGpu = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        info.hardwareRayTracing =
            hasExtension(extensions, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
            hasExtension(extensions, VK_KHR_RAY_QUERY_EXTENSION_NAME);
        info.deviceLocalBytes = vram;

        const VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts &
                                          props.limits.framebufferDepthSampleCounts;
        msaaSamples = (counts & VK_SAMPLE_COUNT_4_BIT)   ? VK_SAMPLE_COUNT_4_BIT
                      : (counts & VK_SAMPLE_COUNT_2_BIT) ? VK_SAMPLE_COUNT_2_BIT
                                                         : VK_SAMPLE_COUNT_1_BIT;
        info.msaaSamples = static_cast<std::uint32_t>(msaaSamples);
        bindlessTextureCapacity = textureCapacity;
        samplerAnisotropy = features.features.samplerAnisotropy == VK_TRUE;
        maxSamplerAnisotropy = props.limits.maxSamplerAnisotropy;
        // The path tracer indexes textures per pixel (different materials in one dispatch).
        nonUniformTextureIndexing = features12.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
        info.pathTracing = nonUniformTextureIndexing;
        timestamps =
            props.limits.timestampComputeAndGraphics == VK_TRUE && graphics->timestampValidBits > 0;
        timestampPeriodNs = props.limits.timestampPeriod;
    }

    if (physicalDevice == VK_NULL_HANDLE) {
        throw std::runtime_error(
            "No graphics card supports the Vulkan 1.3 features LightBox needs (dynamic rendering, "
            "synchronization2, buffer device address, descriptor indexing). Update your graphics "
            "driver.");
    }
    log::info("GPU: {} (Vulkan {}, driver {}, {} MiB device-local, {}x MSAA, {} textures, hardware "
              "ray tracing: {})",
              info.name, info.vulkanVersion, info.driverVersion, info.deviceLocalBytes >> 20,
              info.msaaSamples, bindlessTextureCapacity, info.hardwareRayTracing ? "yes" : "no");
}

void GpuContext::createDevice() {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = graphicsQueueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.pNext = &features13;
    features12.bufferDeviceAddress = VK_TRUE;
    features12.runtimeDescriptorArray = VK_TRUE;
    features12.descriptorBindingPartiallyBound = VK_TRUE;
    features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    features12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    features12.shaderSampledImageArrayNonUniformIndexing =
        nonUniformTextureIndexing ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &features12;
    features.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    features.features.samplerAnisotropy = samplerAnisotropy ? VK_TRUE : VK_FALSE;

    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &features;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = extensions;
    LB_VK_CHECK(vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device));
    volkLoadDevice(device);
    vkGetDeviceQueue(device, graphicsQueueFamily, 0, &graphicsQueue);

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = graphicsQueueFamily;
    LB_VK_CHECK(vkCreateCommandPool(device, &poolInfo, nullptr, &immediatePool_));

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    LB_VK_CHECK(vkCreateFence(device, &fenceInfo, nullptr, &immediateFence_));
}

void GpuContext::createAllocator() {
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    allocatorInfo.physicalDevice = physicalDevice;
    allocatorInfo.device = device;
    allocatorInfo.instance = instance;
    allocatorInfo.pVulkanFunctions = &functions;
    LB_VK_CHECK(vmaCreateAllocator(&allocatorInfo, &allocator));
}

Buffer GpuContext::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, BufferMemory memory) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    switch (memory) {
    case BufferMemory::DeviceLocal:
        bufferInfo.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        break;
    case BufferMemory::HostWrite:
        allocInfo.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    case BufferMemory::HostRead:
        allocInfo.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    }

    Buffer buffer;
    buffer.size = size;
    VmaAllocationInfo result{};
    LB_VK_CHECK(vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &buffer.buffer,
                                &buffer.allocation, &result));
    buffer.mapped = result.pMappedData;

    VkBufferDeviceAddressInfo addressInfo{};
    addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addressInfo.buffer = buffer.buffer;
    buffer.address = vkGetBufferDeviceAddress(device, &addressInfo);
    return buffer;
}

void GpuContext::destroyBuffer(Buffer& buffer) {
    if (buffer.buffer) {
        vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
    }
    buffer = Buffer{};
}

void GpuContext::upload(const Buffer& destination, const void* data, VkDeviceSize size) {
    Buffer staging = createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::HostWrite);
    try {
        std::memcpy(staging.mapped, data, static_cast<std::size_t>(size));
        LB_VK_CHECK(vmaFlushAllocation(allocator, staging.allocation, 0, size));
        immediateSubmit([&](VkCommandBuffer cmd) {
            const VkBufferCopy region{0, 0, size};
            vkCmdCopyBuffer(cmd, staging.buffer, destination.buffer, 1, &region);

            // Make the copy visible to any later use of the buffer by the GPU.
            VkMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dependency);
        });
    } catch (...) {
        destroyBuffer(staging);
        throw;
    }
    destroyBuffer(staging);
}

Image GpuContext::createImage2D(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage,
                                VkSampleCountFlagBits samples, VkImageAspectFlags aspect) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = samples;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usage;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    Image image;
    LB_VK_CHECK(
        vmaCreateImage(allocator, &imageInfo, &allocInfo, &image.image, &image.allocation, nullptr));

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {aspect, 0, 1, 0, 1};
    LB_VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &image.view));
    return image;
}

void GpuContext::destroyImage(Image& image) {
    if (image.view) {
        vkDestroyImageView(device, image.view, nullptr);
    }
    if (image.image) {
        vmaDestroyImage(allocator, image.image, image.allocation);
    }
    image = Image{};
}

void GpuContext::immediateSubmit(const std::function<void(VkCommandBuffer)>& record) {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = immediatePool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    LB_VK_CHECK(vkAllocateCommandBuffers(device, &allocInfo, &cmd));

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    LB_VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
    record(cmd);
    LB_VK_CHECK(vkEndCommandBuffer(cmd));

    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = cmd;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    LB_VK_CHECK(vkResetFences(device, 1, &immediateFence_));
    LB_VK_CHECK(vkQueueSubmit2(graphicsQueue, 1, &submit, immediateFence_));
    LB_VK_CHECK(vkWaitForFences(device, 1, &immediateFence_, VK_TRUE, UINT64_MAX));
    vkFreeCommandBuffers(device, immediatePool_, 1, &cmd);
}

} // namespace lb::render
