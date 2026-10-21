#include "render/renderer.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <stdexcept>
#include <vector>

#include <glm/common.hpp>

#include "core/environment_map.hpp"
#include "render/frame_data.hpp"
#include "render/renderer_impl.hpp"

namespace lb::render {
namespace {

constexpr float kMaxAnisotropy = 8.0f;

VkImageMemoryBarrier2 mipBarrier(VkImage image, std::uint32_t baseMip, std::uint32_t mipCount,
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
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, 0, 1};
    return barrier;
}

void pipelineBarrier(VkCommandBuffer cmd, const VkImageMemoryBarrier2& barrier) {
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

// Records the upload of mip 0 from `staging` and the generation of every smaller level by repeated
// half-size blits. Leaves all levels in SHADER_READ_ONLY_OPTIMAL.
void recordTextureUpload(VkCommandBuffer cmd, VkBuffer staging, VkImage image, std::uint32_t width,
                         std::uint32_t height, std::uint32_t mipLevels, VkFilter blitFilter) {
    pipelineBarrier(cmd, mipBarrier(image, 0, mipLevels, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                    VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    std::int32_t mipWidth = static_cast<std::int32_t>(width);
    std::int32_t mipHeight = static_cast<std::int32_t>(height);
    for (std::uint32_t level = 1; level < mipLevels; ++level) {
        // The previous level becomes the blit source once its own write (copy or blit) is done.
        constexpr VkPipelineStageFlags2 kWriteStages =
            VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT;
        pipelineBarrier(cmd, mipBarrier(image, level - 1, 1, kWriteStages,
                                        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                        VK_ACCESS_2_TRANSFER_READ_BIT,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL));
        const std::int32_t nextWidth = std::max(mipWidth / 2, 1);
        const std::int32_t nextHeight = std::max(mipHeight / 2, 1);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
        blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        blit.dstOffsets[1] = {nextWidth, nextHeight, 1};
        vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, blitFilter);
        pipelineBarrier(cmd, mipBarrier(image, level - 1, 1, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                        VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
        mipWidth = nextWidth;
        mipHeight = nextHeight;
    }
    pipelineBarrier(cmd, mipBarrier(image, mipLevels - 1, 1,
                                    VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
                                    VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
}

VkImageView createTextureView(VkDevice device, VkImage image, VkFormat format,
                              std::uint32_t mipLevels) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    LB_VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &view));
    return view;
}

void destroyTexture(GpuContext& ctx, GpuTexture& texture) {
    if (texture.srgbView) {
        vkDestroyImageView(ctx.device, texture.srgbView, nullptr);
    }
    if (texture.linearView) {
        vkDestroyImageView(ctx.device, texture.linearView, nullptr);
    }
    if (texture.image) {
        vmaDestroyImage(ctx.allocator, texture.image, texture.allocation);
    }
    texture = GpuTexture{};
}

} // namespace

Renderer::Impl::Impl(const RendererOptions& options)
    : context(options.enableValidation),
      pipelines(context.device, context.bindlessTextureCapacity,
                context.nonUniformTextureIndexing, options.pipelineCache) {
    createTextureTable();
}

Renderer::Impl::~Impl() {
    vkDeviceWaitIdle(context.device);
    for (GpuMesh& mesh : meshes) {
        context.destroyBuffer(mesh.vertices);
        context.destroyBuffer(mesh.indices);
    }
    for (GpuTexture& texture : textures) {
        destroyTexture(context, texture);
    }
    destroyTextureTable();
}

void Renderer::Impl::createTextureTable() {
    try {
        // Mip generation blits in the image's creation format; linear filtering there is
        // mandatory in Vulkan for RGBA8, but checking keeps odd drivers working (nearest mips).
        // Color maps are created as sRGB so the blit averages light, not gamma-encoded values
        // (otherwise distant textures turn dark and muddy).
        VkFormatProperties formatProps{};
        vkGetPhysicalDeviceFormatProperties(context.physicalDevice, VK_FORMAT_R8G8B8A8_UNORM,
                                            &formatProps);
        linearBlit = (formatProps.optimalTilingFeatures &
                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
        VkFormatProperties srgbProps{};
        vkGetPhysicalDeviceFormatProperties(context.physicalDevice, VK_FORMAT_R8G8B8A8_SRGB,
                                            &srgbProps);
        srgbBlit = (srgbProps.optimalTilingFeatures &
                    (VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                     VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) ==
                   (VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                    VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT);

        // Trilinear, repeating (tiled materials), anisotropic for surfaces seen at grazing angles
        // (floors, long walls).
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.anisotropyEnable = context.samplerAnisotropy ? VK_TRUE : VK_FALSE;
        samplerInfo.maxAnisotropy =
            context.samplerAnisotropy ? std::min(kMaxAnisotropy, context.maxSamplerAnisotropy)
                                      : 1.0f;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        LB_VK_CHECK(vkCreateSampler(context.device, &samplerInfo, nullptr, &materialSampler));

        const std::uint32_t capacity = context.bindlessTextureCapacity;
        const VkDescriptorPoolSize poolSizes[] = {
            {VK_DESCRIPTOR_TYPE_SAMPLER, 1},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * capacity},
        };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;
        LB_VK_CHECK(vkCreateDescriptorPool(context.device, &poolInfo, nullptr, &texturePool));

        const VkDescriptorSetLayout setLayout = pipelines.textureSetLayout();
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = texturePool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &setLayout;
        LB_VK_CHECK(vkAllocateDescriptorSets(context.device, &allocInfo, &textureSet));

        const VkDescriptorImageInfo samplerDescriptor{materialSampler, VK_NULL_HANDLE,
                                                      VK_IMAGE_LAYOUT_UNDEFINED};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = textureSet;
        write.dstBinding = kSamplerBinding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        write.pImageInfo = &samplerDescriptor;
        vkUpdateDescriptorSets(context.device, 1, &write, 0, nullptr);
    } catch (...) {
        destroyTextureTable();
        throw;
    }
}

void Renderer::Impl::destroyTextureTable() {
    if (texturePool) {
        vkDestroyDescriptorPool(context.device, texturePool, nullptr); // frees textureSet
        texturePool = VK_NULL_HANDLE;
        textureSet = VK_NULL_HANDLE;
    }
    if (materialSampler) {
        vkDestroySampler(context.device, materialSampler, nullptr);
        materialSampler = VK_NULL_HANDLE;
    }
}

Renderer::Renderer(const RendererOptions& options) : impl_(std::make_unique<Impl>(options)) {}

Renderer::~Renderer() = default;

VkInstance Renderer::vkInstance() const {
    return impl_->context.instance;
}

const DeviceInfo& Renderer::deviceInfo() const {
    return impl_->context.info;
}

MeshHandle Renderer::uploadMesh(const MeshData& mesh) {
    if (mesh.positions.empty() || mesh.indices.empty() ||
        mesh.normals.size() != mesh.positions.size()) {
        throw std::invalid_argument("uploadMesh: mesh needs positions, matching normals and indices");
    }
    // Meshes without (or with mismatched) texture coordinates get zeros: UV-mapped textures then
    // show one texel, box mapping works regardless.
    const bool hasUvs = mesh.uvs.size() == mesh.positions.size();
    std::vector<Vertex> vertices(mesh.positions.size());
    Bounds bounds{mesh.positions.front(), mesh.positions.front()};
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        vertices[i] = {mesh.positions[i], mesh.normals[i], hasUvs ? mesh.uvs[i] : glm::vec2(0.0f)};
        bounds.min = glm::min(bounds.min, mesh.positions[i]);
        bounds.max = glm::max(bounds.max, mesh.positions[i]);
    }

    GpuContext& ctx = impl_->context;
    GpuMesh gpu;
    const VkDeviceSize vertexBytes = vertices.size() * sizeof(Vertex);
    const VkDeviceSize indexBytes = mesh.indices.size() * sizeof(std::uint32_t);
    gpu.vertices =
        ctx.createBuffer(vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, BufferMemory::DeviceLocal);
    gpu.indices =
        ctx.createBuffer(indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, BufferMemory::DeviceLocal);
    ctx.upload(gpu.vertices, vertices.data(), vertexBytes);
    ctx.upload(gpu.indices, mesh.indices.data(), indexBytes);
    gpu.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
    gpu.bounds = bounds;

    if (!impl_->freeMeshSlots.empty()) {
        const std::uint32_t slot = impl_->freeMeshSlots.back();
        impl_->freeMeshSlots.pop_back();
        impl_->meshes[slot] = gpu;
        return MeshHandle{slot};
    }
    impl_->meshes.push_back(gpu);
    return MeshHandle{static_cast<std::uint32_t>(impl_->meshes.size() - 1)};
}

void Renderer::releaseMesh(MeshHandle handle) {
    if (!handle.valid() || handle.index >= impl_->meshes.size()) {
        return;
    }
    GpuMesh& mesh = impl_->meshes[handle.index];
    if (mesh.indexCount == 0) {
        return;
    }
    // In-flight frames may still read the buffers.
    LB_VK_CHECK(vkDeviceWaitIdle(impl_->context.device));
    impl_->context.destroyBuffer(mesh.vertices);
    impl_->context.destroyBuffer(mesh.indices);
    mesh = GpuMesh{};
    impl_->freeMeshSlots.push_back(handle.index);
}

namespace {

// A mip-mapped texture from tightly packed pixels, with an sRGB-interpreting and a linear view
// (the same format for float images). The mip chain is filtered in `format`.
GpuTexture createTexture(GpuContext& ctx, const void* pixels, VkDeviceSize bytes,
                         std::uint32_t width, std::uint32_t height, VkFormat format,
                         VkFormat srgbViewFormat, VkFormat linearViewFormat, VkFilter blitFilter) {
    const std::uint32_t mipLevels = static_cast<std::uint32_t>(std::bit_width(std::max(width, height)));
    const bool mutableFormat = srgbViewFormat != linearViewFormat;
    const VkFormat viewFormats[] = {linearViewFormat, srgbViewFormat};
    VkImageFormatListCreateInfo formatList{};
    formatList.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
    formatList.viewFormatCount = 2;
    formatList.pViewFormats = viewFormats;
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.pNext = mutableFormat ? &formatList : nullptr;
    imageInfo.flags = mutableFormat ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;

    GpuTexture texture;
    Buffer staging;
    try {
        LB_VK_CHECK(vmaCreateImage(ctx.allocator, &imageInfo, &allocInfo, &texture.image,
                                   &texture.allocation, nullptr));
        staging =
            ctx.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::HostWrite);
        std::memcpy(staging.mapped, pixels, bytes);
        LB_VK_CHECK(vmaFlushAllocation(ctx.allocator, staging.allocation, 0, bytes));
        ctx.immediateSubmit([&](VkCommandBuffer cmd) {
            recordTextureUpload(cmd, staging.buffer, texture.image, width, height, mipLevels,
                                blitFilter);
        });
        ctx.destroyBuffer(staging);
        texture.srgbView = createTextureView(ctx.device, texture.image, srgbViewFormat, mipLevels);
        texture.linearView =
            createTextureView(ctx.device, texture.image, linearViewFormat, mipLevels);
    } catch (...) {
        ctx.destroyBuffer(staging);
        destroyTexture(ctx, texture);
        throw;
    }
    return texture;
}

} // namespace

void Renderer::Impl::checkTextureCapacity() const {
    if (freeTextureSlots.empty() && textures.size() >= context.bindlessTextureCapacity) {
        throw std::runtime_error(std::format(
            "This model uses more textures than the graphics card can show at once ({}).",
            context.bindlessTextureCapacity));
    }
}

TextureHandle Renderer::Impl::addTexture(const GpuTexture& texture) {
    std::uint32_t slot = 0;
    if (!freeTextureSlots.empty()) {
        slot = freeTextureSlots.back();
        freeTextureSlots.pop_back();
        textures[slot] = texture;
    } else {
        slot = static_cast<std::uint32_t>(textures.size());
        textures.push_back(texture);
    }

    // The slot is not referenced by any submitted frame (new, or released after a device wait), so
    // update-after-bind + update-unused-while-pending allow writing it while frames are in flight.
    const VkDescriptorImageInfo srgbInfo{VK_NULL_HANDLE, texture.srgbView,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo linearInfo{VK_NULL_HANDLE, texture.linearView,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[2]{};
    for (VkWriteDescriptorSet& write : writes) {
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = textureSet;
        write.dstArrayElement = slot;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    }
    writes[0].dstBinding = kSrgbTexturesBinding;
    writes[0].pImageInfo = &srgbInfo;
    writes[1].dstBinding = kLinearTexturesBinding;
    writes[1].pImageInfo = &linearInfo;
    vkUpdateDescriptorSets(context.device, 2, writes, 0, nullptr);
    return TextureHandle{slot};
}

TextureHandle Renderer::uploadTexture(const ImageData& image, bool color) {
    if (image.empty() ||
        image.pixels.size() != std::size_t{image.width} * std::size_t{image.height} * 4) {
        throw std::invalid_argument("uploadTexture: image needs RGBA8 pixels matching its size");
    }
    Impl& r = *impl_;
    r.checkTextureCapacity();
    // One image, two interpretations: sRGB for color, UNORM for data maps. Created in the format of
    // its main use, because the mip chain is filtered in the creation format.
    const bool srgbImage = color && r.srgbBlit;
    const GpuTexture texture = createTexture(
        r.context, image.pixels.data(), image.pixels.size(), image.width, image.height,
        srgbImage ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB,
        VK_FORMAT_R8G8B8A8_UNORM, r.linearBlit ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    return r.addTexture(texture);
}

TextureHandle Renderer::uploadEnvironmentMap(const EnvironmentMap& map) {
    if (map.width == 0 || map.height == 0 ||
        map.pixels.size() != std::size_t{map.width} * std::size_t{map.height} * 4) {
        throw std::invalid_argument("uploadEnvironmentMap: needs RGBA16F pixels matching its size");
    }
    Impl& r = *impl_;
    r.checkTextureCapacity();
    // Linear blits of R16G16B16A16_SFLOAT are mandatory in Vulkan.
    const GpuTexture texture = createTexture(
        r.context, map.pixels.data(), map.pixels.size() * sizeof(std::uint16_t), map.width,
        map.height, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_FILTER_LINEAR);
    return r.addTexture(texture);
}

void Renderer::releaseTexture(TextureHandle handle) {
    if (!impl_->hasTexture(handle)) {
        return;
    }
    // In-flight frames may still sample the image. The stale descriptor stays in the slot; it is
    // never read again because frames only reference live textures (partially bound binding).
    LB_VK_CHECK(vkDeviceWaitIdle(impl_->context.device));
    destroyTexture(impl_->context, impl_->textures[handle.index]);
    impl_->freeTextureSlots.push_back(handle.index);
}

std::unique_ptr<Presenter> Renderer::createPresenter(VkSurfaceKHR surface, std::uint32_t width,
                                                     std::uint32_t height) {
    return makePresenter(*impl_, surface, width, height);
}

std::unique_ptr<Presenter> Renderer::createOffscreenPresenter(std::uint32_t width,
                                                              std::uint32_t height) {
    return makePresenter(*impl_, VK_NULL_HANDLE, width, height);
}

} // namespace lb::render
