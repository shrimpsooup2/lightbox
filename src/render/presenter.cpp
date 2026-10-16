#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>

#include "core/log.hpp"
#include "render/frame_data.hpp"
#include "render/renderer_impl.hpp"

namespace lb::render {
namespace {

constexpr std::uint32_t kFramesInFlight = 2;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr std::uint32_t kShadowMapSize = 2048;
constexpr std::size_t kInitialMaterialCapacity = 64;

// Shader-side material. Textures the renderer doesn't know (never uploaded or already released)
// become unused slots, so the shader never reads a stale descriptor.
GpuMaterial toGpuMaterial(const MaterialParams& params, const Renderer::Impl& renderer) {
    const auto slot = [&renderer](const MaterialTexture& map) {
        return renderer.hasTexture(map.texture) ? map.texture.index : kNoTexture;
    };
    const auto channel = [](const MaterialTexture& map) {
        return std::min(map.channel, 3u) | (map.invert ? 4u : 0u);
    };

    GpuMaterial m{};
    m.baseColor = glm::vec4(glm::max(params.baseColor, glm::vec3(0.0f)),
                            std::max(params.normalStrength, 0.0f));
    m.factors = glm::vec4(params.roughness, params.metalness, params.normalFlipY ? -1.0f : 1.0f,
                          params.mapping == TextureMapping::Box ? 1.0f : 0.0f);
    glm::vec2 scale = params.textureScale;
    if (params.mapping == TextureMapping::Box) {
        // Tile size in meters → tiles per meter. Tiny sizes are clamped (the shader divides by the
        // scale to orient normal maps); the sign is kept so negative sizes still mirror.
        for (int axis = 0; axis < 2; ++axis) {
            const float size = std::max(std::abs(scale[axis]), 1e-3f);
            scale[axis] = std::copysign(1.0f / size, scale[axis]);
        }
    }
    m.texTransform = glm::vec4(scale, params.textureOffset);
    const float radians = glm::radians(params.textureRotationDegrees);
    m.texRotation = glm::vec4(std::cos(radians), std::sin(radians), 0.0f, 0.0f);
    m.maps = glm::uvec4(slot(params.baseColorMap), slot(params.roughnessMap),
                        slot(params.metalnessMap), slot(params.normalMap));
    m.maps2 = glm::uvec4(slot(params.occlusionMap), channel(params.roughnessMap),
                         channel(params.metalnessMap), channel(params.occlusionMap));
    return m;
}

// Orthographic sun camera fitted around everything that is drawn.
struct ShadowSetup {
    bool active = false;
    glm::mat4 viewProj{1.0f};
    float texelWorldSize = 0.0f; // meters covered by one shadow map texel
};

ShadowSetup fitSunShadow(const std::vector<GpuMesh>& meshes, std::span<const DrawItem> draws,
                         glm::vec3 sunDirection) {
    ShadowSetup setup;
    const glm::vec3 dir = glm::normalize(sunDirection);
    if (dir.z < 0.0f) {
        return setup; // sun below the horizon casts no light, so no shadows either
    }
    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(std::numeric_limits<float>::lowest());
    bool any = false;
    for (const DrawItem& item : draws) {
        if (!item.mesh.valid() || item.mesh.index >= meshes.size() ||
            meshes[item.mesh.index].indexCount == 0) {
            continue;
        }
        const Bounds& b = meshes[item.mesh.index].bounds;
        for (int corner = 0; corner < 8; ++corner) {
            const glm::vec3 local{(corner & 1) ? b.max.x : b.min.x, (corner & 2) ? b.max.y : b.min.y,
                                  (corner & 4) ? b.max.z : b.min.z};
            const glm::vec3 world = glm::vec3(item.transform * glm::vec4(local, 1.0f));
            lo = glm::min(lo, world);
            hi = glm::max(hi, world);
        }
        any = true;
    }
    if (!any) {
        return setup;
    }
    const glm::vec3 center = (lo + hi) * 0.5f;
    const float radius = std::max(glm::length(hi - lo) * 0.5f, 1.0f);
    const glm::vec3 up = std::abs(dir.z) > 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1);
    const glm::mat4 view = glm::lookAtRH(center + dir * (radius * 2.0f), center, up);
    const glm::mat4 projection =
        glm::orthoRH_ZO(-radius, radius, -radius, radius, radius * 0.5f, radius * 3.5f);
    setup.active = true;
    setup.viewProj = projection * view;
    setup.texelWorldSize = 2.0f * radius / static_cast<float>(kShadowMapSize);
    return setup;
}

struct FrameResources {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;
    Buffer frameData;
    Buffer materials; // GpuMaterial array, grown on demand
};

VkImageMemoryBarrier2 imageBarrier(VkImage image, VkImageAspectFlags aspect,
                                   VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                                   VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, 0, 1, 0, 1};
    return barrier;
}

} // namespace

struct Presenter::Impl {
    Impl(Renderer::Impl& renderer, VkSurfaceKHR surface, std::uint32_t width, std::uint32_t height);
    ~Impl();

    bool render(const FrameInput& input, CapturedImage* capture);
    void recreateSwapchain();
    void destroySwapchainResources();
    void writeFrameData(FrameResources& frame, const FrameInput& input,
                        const ShadowSetup& shadow) const;
    // Fills the frame's material buffer; returns the number of materials written (at least 1).
    std::uint32_t writeMaterials(FrameResources& frame, const FrameInput& input);
    void record(VkCommandBuffer cmd, std::uint32_t imageIndex, const FrameInput& input,
                const FrameResources& frame, std::uint32_t materialCount, const ShadowSetup& shadow,
                const Buffer* readback);
    void createShadowResources();

    Renderer::Impl& renderer;
    GpuContext& ctx;
    VkSurfaceKHR surface;
    VkSurfaceFormatKHR surfaceFormat{};
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkExtent2D extent{0, 0};
    VkExtent2D requested{0, 0};
    bool dirty = true;
    bool canCapture = false; // swapchain images support TRANSFER_SRC (readback)

    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkSemaphore> renderFinished; // one per swapchain image
    Image depth;
    Image msaaColor;

    std::array<FrameResources, kFramesInFlight> frames{};
    std::uint32_t frameIndex = 0;

    // Sun shadow map, sampled by the mesh shader through descriptor set 1 (kShadowSet).
    Image shadowMap;
    VkSampler shadowSampler = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet shadowSet = VK_NULL_HANDLE;
};

Presenter::Impl::Impl(Renderer::Impl& r, VkSurfaceKHR s, std::uint32_t width, std::uint32_t height)
    : renderer(r), ctx(r.context), surface(s), requested{width, height} {
    VkBool32 presentSupported = VK_FALSE;
    LB_VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(ctx.physicalDevice, ctx.graphicsQueueFamily,
                                                     surface, &presentSupported));
    if (!presentSupported) {
        throw std::runtime_error("The selected GPU cannot present to this window.");
    }

    std::uint32_t formatCount = 0;
    LB_VK_CHECK(
        vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physicalDevice, surface, &formatCount, nullptr));
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    LB_VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physicalDevice, surface, &formatCount,
                                                     formats.data()));
    if (formats.empty()) {
        throw std::runtime_error("The window surface reports no pixel formats.");
    }
    // Shaders output linear color; an sRGB swapchain applies the display encoding.
    surfaceFormat = formats.front();
    for (const VkSurfaceFormatKHR& f : formats) {
        if ((f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surfaceFormat = f;
            break;
        }
    }

    for (FrameResources& frame : frames) {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = ctx.graphicsQueueFamily;
        LB_VK_CHECK(vkCreateCommandPool(ctx.device, &poolInfo, nullptr, &frame.pool));

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = frame.pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        LB_VK_CHECK(vkAllocateCommandBuffers(ctx.device, &allocInfo, &frame.cmd));

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        LB_VK_CHECK(vkCreateSemaphore(ctx.device, &semaphoreInfo, nullptr, &frame.imageAvailable));

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        LB_VK_CHECK(vkCreateFence(ctx.device, &fenceInfo, nullptr, &frame.inFlight));

        frame.frameData = ctx.createBuffer(sizeof(FrameData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                           BufferMemory::HostWrite);
        frame.materials =
            ctx.createBuffer(kInitialMaterialCapacity * sizeof(GpuMaterial),
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::HostWrite);
    }
    createShadowResources();
}

Presenter::Impl::~Impl() {
    vkDeviceWaitIdle(ctx.device);
    destroySwapchainResources();
    if (swapchain) {
        vkDestroySwapchainKHR(ctx.device, swapchain, nullptr);
    }
    for (FrameResources& frame : frames) {
        ctx.destroyBuffer(frame.frameData);
        ctx.destroyBuffer(frame.materials);
        vkDestroyFence(ctx.device, frame.inFlight, nullptr);
        vkDestroySemaphore(ctx.device, frame.imageAvailable, nullptr);
        vkDestroyCommandPool(ctx.device, frame.pool, nullptr);
    }
    ctx.destroyImage(shadowMap);
    vkDestroySampler(ctx.device, shadowSampler, nullptr);
    vkDestroyDescriptorPool(ctx.device, descriptorPool, nullptr);
}

void Presenter::Impl::destroySwapchainResources() {
    ctx.destroyImage(depth);
    ctx.destroyImage(msaaColor);
    for (VkImageView view : views) {
        vkDestroyImageView(ctx.device, view, nullptr);
    }
    views.clear();
    for (VkSemaphore semaphore : renderFinished) {
        vkDestroySemaphore(ctx.device, semaphore, nullptr);
    }
    renderFinished.clear();
    images.clear();
}

void Presenter::Impl::createShadowResources() {
    shadowMap = ctx.createImage2D(kShadowMapFormat, {kShadowMapSize, kShadowMapSize},
                                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                  VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);

    // Hardware depth comparison with bilinear filtering; outside the map counts as lit.
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    samplerInfo.compareEnable = VK_TRUE;
    samplerInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    samplerInfo.maxLod = 0.0f;
    LB_VK_CHECK(vkCreateSampler(ctx.device, &samplerInfo, nullptr, &shadowSampler));

    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    LB_VK_CHECK(vkCreateDescriptorPool(ctx.device, &poolInfo, nullptr, &descriptorPool));

    const VkDescriptorSetLayout setLayout = renderer.pipelines.shadowSetLayout();
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &setLayout;
    LB_VK_CHECK(vkAllocateDescriptorSets(ctx.device, &allocInfo, &shadowSet));

    const VkDescriptorImageInfo imageInfo{shadowSampler, shadowMap.view,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = shadowSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);

    // Start cleared to "far" (everything lit) and in the sampled layout, so frames that skip the
    // shadow pass (shadows off, empty scene) still read a valid image.
    ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 toTransfer = imageBarrier(
            shadowMap.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkDependencyInfo first{};
        first.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        first.imageMemoryBarrierCount = 1;
        first.pImageMemoryBarriers = &toTransfer;
        vkCmdPipelineBarrier2(cmd, &first);
        const VkClearDepthStencilValue clear{1.0f, 0};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(cmd, shadowMap.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    &clear, 1, &range);
        VkImageMemoryBarrier2 toSampled = imageBarrier(
            shadowMap.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_PIPELINE_STAGE_2_CLEAR_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkDependencyInfo second{};
        second.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        second.imageMemoryBarrierCount = 1;
        second.pImageMemoryBarriers = &toSampled;
        vkCmdPipelineBarrier2(cmd, &second);
    });
}

void Presenter::Impl::recreateSwapchain() {
    LB_VK_CHECK(vkDeviceWaitIdle(ctx.device));

    VkSurfaceCapabilitiesKHR caps{};
    LB_VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.physicalDevice, surface, &caps));
    VkExtent2D newExtent = caps.currentExtent;
    if (newExtent.width == UINT32_MAX) {
        newExtent.width =
            std::clamp(requested.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        newExtent.height =
            std::clamp(requested.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (newExtent.width == 0 || newExtent.height == 0) {
        extent = {0, 0}; // minimized: keep `dirty` so we retry on the next frame
        return;
    }

    std::uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) {
        imageCount = std::min(imageCount, caps.maxImageCount);
    }
    VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & compositeAlpha)) {
        for (VkCompositeAlphaFlagBitsKHR candidate :
             {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
            if (caps.supportedCompositeAlpha & candidate) {
                compositeAlpha = candidate;
                break;
            }
        }
    }

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface;
    info.minImageCount = imageCount;
    info.imageFormat = surfaceFormat.format;
    info.imageColorSpace = surfaceFormat.colorSpace;
    info.imageExtent = newExtent;
    info.imageArrayLayers = 1;
    canCapture = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (canCapture) {
        info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = compositeAlpha;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR; // vsync; always available, battery friendly
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain;

    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    LB_VK_CHECK(vkCreateSwapchainKHR(ctx.device, &info, nullptr, &newSwapchain));
    destroySwapchainResources();
    if (swapchain) {
        vkDestroySwapchainKHR(ctx.device, swapchain, nullptr);
    }
    swapchain = newSwapchain;
    extent = newExtent;

    std::uint32_t count = 0;
    LB_VK_CHECK(vkGetSwapchainImagesKHR(ctx.device, swapchain, &count, nullptr));
    images.resize(count);
    LB_VK_CHECK(vkGetSwapchainImagesKHR(ctx.device, swapchain, &count, images.data()));
    for (VkImage image : images) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = surfaceFormat.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        LB_VK_CHECK(vkCreateImageView(ctx.device, &viewInfo, nullptr, &view));
        views.push_back(view);

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkSemaphore semaphore = VK_NULL_HANDLE;
        LB_VK_CHECK(vkCreateSemaphore(ctx.device, &semaphoreInfo, nullptr, &semaphore));
        renderFinished.push_back(semaphore);
    }

    depth = ctx.createImage2D(kDepthFormat, extent, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                              ctx.msaaSamples, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (ctx.msaaSamples != VK_SAMPLE_COUNT_1_BIT) {
        msaaColor = ctx.createImage2D(
            surfaceFormat.format, extent,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
            ctx.msaaSamples, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    dirty = false;
    log::debug("Swapchain {}x{}, {} images, format {}", extent.width, extent.height, count,
               static_cast<int>(surfaceFormat.format));
}

void Presenter::Impl::writeFrameData(FrameResources& frame, const FrameInput& input,
                                     const ShadowSetup& shadow) const {
    const glm::mat4 viewProj = input.projection * input.view;
    const Lighting& light = input.lighting;
    const glm::vec3 sunDir = glm::normalize(light.sunDirection);

    FrameData data{};
    data.viewProj = viewProj;
    data.invViewProj = glm::inverse(viewProj);
    data.shadowViewProj = shadow.viewProj;
    data.cameraPosition = glm::vec4(input.cameraPosition, 1.0f);
    data.sunDirection = glm::vec4(sunDir, 0.0f);

    // Procedural daylight driven by the sun height (Stage 3.4 replaces it with a physical sky):
    // blue sky by day, warm horizon at low sun, dark blue after sunset. The sun fades out as it sets.
    const float day = glm::smoothstep(-0.12f, 0.2f, sunDir.z);
    const float lowSun = 1.0f - glm::smoothstep(0.02f, 0.35f, sunDir.z);
    const glm::vec3 zenith = glm::mix(glm::vec3(0.004f, 0.007f, 0.018f),
                                      glm::vec3(0.09f, 0.24f, 0.62f), day);
    const glm::vec3 horizonDay = glm::mix(glm::vec3(0.52f, 0.66f, 0.84f),
                                          glm::vec3(0.95f, 0.62f, 0.40f), lowSun * day);
    const glm::vec3 horizon = horizonDay * glm::mix(0.03f, 1.0f, day);
    const glm::vec3 ground = glm::vec3(0.23f, 0.235f, 0.245f) * glm::mix(0.03f, 1.0f, day);
    const float sky = std::max(light.skyIntensity, 0.0f);
    data.skyZenith = glm::vec4(zenith * sky, 1.0f);
    data.skyHorizon = glm::vec4(horizon * sky, 1.0f);
    data.groundColor = glm::vec4(ground * sky, 1.0f);
    const float sunVisible = glm::smoothstep(-0.02f, 0.04f, sunDir.z);
    data.sunColor = glm::vec4(light.sunColor * sunVisible, 1.0f);

    const float fadeDistance = std::max(80.0f, std::abs(input.cameraPosition.z) * 40.0f);
    data.gridParams = glm::vec4(1.0f, 10.0f, fadeDistance, input.showGrid ? 1.0f : 0.0f);
    data.shadowParams = glm::vec4(shadow.active ? 1.0f : 0.0f,
                                  std::clamp(light.shadowSoftness, 0.0f, 4.0f) * 3.0f,
                                  1.0f / static_cast<float>(kShadowMapSize),
                                  shadow.texelWorldSize * 1.5f);

    const LookSettings& look = input.look;
    data.look = glm::vec4(std::exp2(look.exposureEv), std::clamp(look.contrast, 0.2f, 3.0f),
                          std::max(look.saturation, 0.0f), std::clamp(look.vignette, 0.0f, 1.0f));
    data.look2 = glm::vec4(static_cast<float>(look.toneMapping), look.clay ? 1.0f : 0.0f,
                           std::clamp(look.warmth, -1.0f, 1.0f), 0.0f);
    data.viewport = glm::vec4(static_cast<float>(extent.width), static_cast<float>(extent.height),
                              1.0f / static_cast<float>(extent.width),
                              1.0f / static_cast<float>(extent.height));

    std::memcpy(frame.frameData.mapped, &data, sizeof(data));
    LB_VK_CHECK(vmaFlushAllocation(ctx.allocator, frame.frameData.allocation, 0, sizeof(data)));
}

std::uint32_t Presenter::Impl::writeMaterials(FrameResources& frame, const FrameInput& input) {
    // Without materials every draw uses the built-in clay material (MaterialParams defaults).
    static const MaterialParams kClay{};
    const std::span<const MaterialParams> materials =
        input.materials.empty() ? std::span<const MaterialParams>(&kClay, 1) : input.materials;

    // The frame's fence has been waited on, so its previous buffer is no longer read by the GPU.
    const VkDeviceSize bytes = materials.size() * sizeof(GpuMaterial);
    if (bytes > frame.materials.size) {
        const VkDeviceSize capacity =
            std::max<VkDeviceSize>(bytes, frame.materials.size * 2) / sizeof(GpuMaterial) *
            sizeof(GpuMaterial);
        ctx.destroyBuffer(frame.materials);
        frame.materials = ctx.createBuffer(capacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                           BufferMemory::HostWrite);
    }
    auto* out = static_cast<GpuMaterial*>(frame.materials.mapped);
    for (std::size_t i = 0; i < materials.size(); ++i) {
        out[i] = toGpuMaterial(materials[i], renderer);
    }
    LB_VK_CHECK(vmaFlushAllocation(ctx.allocator, frame.materials.allocation, 0, bytes));
    return static_cast<std::uint32_t>(materials.size());
}

void Presenter::Impl::record(VkCommandBuffer cmd, std::uint32_t imageIndex,
                             const FrameInput& input, const FrameResources& frame,
                             std::uint32_t materialCount, const ShadowSetup& shadow,
                             const Buffer* readback) {
    const bool msaa = ctx.msaaSamples != VK_SAMPLE_COUNT_1_BIT;
    const PipelineSet pipes =
        renderer.pipelines.get({surfaceFormat.format, kDepthFormat, ctx.msaaSamples});

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    LB_VK_CHECK(vkBeginCommandBuffer(cmd, &begin));

    const VkPipelineLayout layout = renderer.pipelines.layout();
    constexpr VkShaderStageFlags kStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    PushConstants push{};
    push.model = glm::mat4(1.0f);
    push.frame = frame.frameData.address;
    push.materials = frame.materials.address;
    push.material = 0;

    // Sun shadow pass (depth only). Skipped when off: the map then keeps its previous contents,
    // which the shader ignores because shadowParams.x is 0.
    if (shadow.active && input.lighting.shadows) {
        const VkImageMemoryBarrier2 toDepth = imageBarrier(
            shadowMap.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
        VkDependencyInfo toDepthInfo{};
        toDepthInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        toDepthInfo.imageMemoryBarrierCount = 1;
        toDepthInfo.pImageMemoryBarriers = &toDepth;
        vkCmdPipelineBarrier2(cmd, &toDepthInfo);

        VkRenderingAttachmentInfo shadowDepth{};
        shadowDepth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        shadowDepth.imageView = shadowMap.view;
        shadowDepth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        shadowDepth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        shadowDepth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        shadowDepth.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo shadowRendering{};
        shadowRendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        shadowRendering.renderArea = {{0, 0}, {kShadowMapSize, kShadowMapSize}};
        shadowRendering.layerCount = 1;
        shadowRendering.pDepthAttachment = &shadowDepth;
        vkCmdBeginRendering(cmd, &shadowRendering);
        const VkViewport shadowViewport{0.0f, 0.0f, static_cast<float>(kShadowMapSize),
                                        static_cast<float>(kShadowMapSize), 0.0f, 1.0f};
        const VkRect2D shadowScissor{{0, 0}, {kShadowMapSize, kShadowMapSize}};
        vkCmdSetViewport(cmd, 0, 1, &shadowViewport);
        vkCmdSetScissor(cmd, 0, 1, &shadowScissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.pipelines.shadow());
        for (const DrawItem& item : input.draws) {
            if (!item.mesh.valid() || item.mesh.index >= renderer.meshes.size() ||
                renderer.meshes[item.mesh.index].indexCount == 0) {
                continue;
            }
            const GpuMesh& mesh = renderer.meshes[item.mesh.index];
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertices.buffer, &offset);
            vkCmdBindIndexBuffer(cmd, mesh.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
            push.model = item.transform;
            vkCmdPushConstants(cmd, layout, kStages, 0, sizeof(push), &push);
            vkCmdDrawIndexed(cmd, mesh.indexCount, 1, 0, 0, 0);
        }
        vkCmdEndRendering(cmd);

        const VkImageMemoryBarrier2 toSampled = imageBarrier(
            shadowMap.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkDependencyInfo toSampledInfo{};
        toSampledInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        toSampledInfo.imageMemoryBarrierCount = 1;
        toSampledInfo.pImageMemoryBarriers = &toSampled;
        vkCmdPipelineBarrier2(cmd, &toSampledInfo);
    }

    // Attachments are fully overwritten every frame, so they start from UNDEFINED. The source
    // stages/accesses order this frame's writes after the previous frame's use of shared images.
    constexpr VkPipelineStageFlags2 kDepthStages =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    std::array<VkImageMemoryBarrier2, 3> barriers{};
    std::uint32_t barrierCount = 0;
    barriers[barrierCount++] = imageBarrier(
        images[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    barriers[barrierCount++] = imageBarrier(
        depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, kDepthStages,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, kDepthStages,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    if (msaa) {
        barriers[barrierCount++] = imageBarrier(
            msaaColor.image, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = barrierCount;
    dependency.pImageMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(cmd, &dependency);

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = msaa ? msaaColor.view : views[imageIndex];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    if (msaa) {
        color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color.resolveImageView = views[imageIndex];
        color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkRenderingAttachmentInfo depthAttachment{};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttachment.imageView = depth.view;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.clearValue.depthStencil = {0.0f, 0}; // reverse-Z: 0 is infinitely far

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    rendering.pDepthAttachment = &depthAttachment;
    vkCmdBeginRendering(cmd, &rendering);

    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width),
                              static_cast<float>(extent.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    push.model = glm::mat4(1.0f);
    push.material = 0;

    // One layout for every scene pipeline: bind the renderer-wide textures and this viewport's
    // shadow map once.
    const VkDescriptorSet sets[] = {renderer.textureSet, shadowSet};
    static_assert(kTextureSet == 0 && kShadowSet == 1, "sets order");
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, kTextureSet, 2, sets, 0,
                            nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes.sky);
    vkCmdPushConstants(cmd, layout, kStages, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes.mesh);
    for (const DrawItem& item : input.draws) {
        if (!item.mesh.valid() || item.mesh.index >= renderer.meshes.size()) {
            continue;
        }
        const GpuMesh& mesh = renderer.meshes[item.mesh.index];
        if (mesh.indexCount == 0) {
            continue; // released
        }
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertices.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, mesh.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
        push.model = item.transform;
        // Out-of-range indices would read past the material buffer; fall back to the first one.
        push.material = item.material < materialCount ? item.material : 0;
        vkCmdPushConstants(cmd, layout, kStages, 0, sizeof(push), &push);
        vkCmdDrawIndexed(cmd, mesh.indexCount, 1, 0, 0, 0);
    }

    if (input.showGrid) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes.grid);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    vkCmdEndRendering(cmd);

    VkImageLayout imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkPipelineStageFlags2 lastStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkAccessFlags2 lastAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    if (readback) {
        const VkImageMemoryBarrier2 toTransfer = imageBarrier(
            images[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, lastStage, lastAccess,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, imageLayout,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkDependencyInfo transferDependency{};
        transferDependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        transferDependency.imageMemoryBarrierCount = 1;
        transferDependency.pImageMemoryBarriers = &toTransfer;
        vkCmdPipelineBarrier2(cmd, &transferDependency);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(cmd, images[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback->buffer, 1, &region);

        VkBufferMemoryBarrier2 toHost{};
        toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        toHost.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.buffer = readback->buffer;
        toHost.size = VK_WHOLE_SIZE;
        VkDependencyInfo hostDependency{};
        hostDependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        hostDependency.bufferMemoryBarrierCount = 1;
        hostDependency.pBufferMemoryBarriers = &toHost;
        vkCmdPipelineBarrier2(cmd, &hostDependency);

        imageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        lastStage = VK_PIPELINE_STAGE_2_COPY_BIT;
        lastAccess = VK_ACCESS_2_NONE;
    }

    const VkImageMemoryBarrier2 toPresent =
        imageBarrier(images[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, lastStage, lastAccess,
                     VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, imageLayout,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    VkDependencyInfo presentDependency{};
    presentDependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    presentDependency.imageMemoryBarrierCount = 1;
    presentDependency.pImageMemoryBarriers = &toPresent;
    vkCmdPipelineBarrier2(cmd, &presentDependency);

    LB_VK_CHECK(vkEndCommandBuffer(cmd));
}

bool Presenter::Impl::render(const FrameInput& input, CapturedImage* capture) {
    if (dirty) {
        recreateSwapchain();
    }
    if (extent.width == 0 || extent.height == 0) {
        return false;
    }

    FrameResources& frame = frames[frameIndex];
    LB_VK_CHECK(vkWaitForFences(ctx.device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));

    std::uint32_t imageIndex = 0;
    const VkResult acquired = vkAcquireNextImageKHR(ctx.device, swapchain, UINT64_MAX,
                                                    frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        dirty = true;
        return false;
    }
    if (acquired != VK_SUBOPTIMAL_KHR) {
        LB_VK_CHECK(acquired);
    }

    // May (re)allocate the material buffer, so it runs before the fence is reset: a failure then
    // leaves the frame waitable.
    const std::uint32_t materialCount = writeMaterials(frame, input);
    LB_VK_CHECK(vkResetFences(ctx.device, 1, &frame.inFlight));
    LB_VK_CHECK(vkResetCommandPool(ctx.device, frame.pool, 0));
    const ShadowSetup shadow =
        input.lighting.shadows ? fitSunShadow(renderer.meshes, input.draws, input.lighting.sunDirection)
                               : ShadowSetup{};
    writeFrameData(frame, input, shadow);

    // Readback is rare (screenshots, thumbnails), so its buffer is created on demand.
    Buffer readback;
    const bool capturing = capture != nullptr && canCapture;
    if (capture) {
        *capture = CapturedImage{};
    }
    if (capturing) {
        readback = ctx.createBuffer(VkDeviceSize{extent.width} * extent.height * 4,
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::HostRead);
    }
    record(frame.cmd, imageIndex, input, frame, materialCount, shadow,
           capturing ? &readback : nullptr);

    VkSemaphoreSubmitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitInfo.semaphore = frame.imageAvailable;
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signalInfo{};
    signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfo.semaphore = renderFinished[imageIndex];
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = frame.cmd;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &waitInfo;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signalInfo;
    LB_VK_CHECK(vkQueueSubmit2(ctx.graphicsQueue, 1, &submit, frame.inFlight));

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished[imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &imageIndex;
    const VkResult presented = vkQueuePresentKHR(ctx.graphicsQueue, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR ||
        acquired == VK_SUBOPTIMAL_KHR) {
        dirty = true;
    } else {
        LB_VK_CHECK(presented);
    }

    if (capturing) {
        LB_VK_CHECK(vkWaitForFences(ctx.device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));
        LB_VK_CHECK(vmaInvalidateAllocation(ctx.allocator, readback.allocation, 0, VK_WHOLE_SIZE));
        capture->width = extent.width;
        capture->height = extent.height;
        capture->bgra = surfaceFormat.format == VK_FORMAT_B8G8R8A8_SRGB ||
                        surfaceFormat.format == VK_FORMAT_B8G8R8A8_UNORM;
        const auto* bytes = static_cast<const std::uint8_t*>(readback.mapped);
        capture->pixels.assign(bytes, bytes + readback.size);
        ctx.destroyBuffer(readback);
    }

    frameIndex = (frameIndex + 1) % kFramesInFlight;
    return true;
}

Presenter::Presenter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Presenter::~Presenter() = default;

void Presenter::resize(std::uint32_t width, std::uint32_t height) {
    if (width != impl_->requested.width || height != impl_->requested.height) {
        impl_->requested = {width, height};
        impl_->dirty = true;
    }
}

bool Presenter::render(const FrameInput& input, CapturedImage* capture) {
    return impl_->render(input, capture);
}

std::uint32_t Presenter::width() const {
    return impl_->extent.width;
}

std::uint32_t Presenter::height() const {
    return impl_->extent.height;
}

std::unique_ptr<Presenter> makePresenter(Renderer::Impl& renderer, VkSurfaceKHR surface,
                                         std::uint32_t width, std::uint32_t height) {
    return std::make_unique<Presenter>(
        std::make_unique<Presenter::Impl>(renderer, surface, width, height));
}

} // namespace lb::render
