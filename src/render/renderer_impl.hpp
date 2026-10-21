#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "render/gpu_context.hpp"
#include "render/pipelines.hpp"
#include "render/renderer.hpp"

namespace lb::render {

struct GpuMesh {
    Buffer vertices;
    Buffer indices;
    std::uint32_t indexCount = 0;
    Bounds bounds; // local space, for fitting the sun shadow map
};

// An RGBA8 image viewed both as sRGB (base color) and as linear data (roughness, normals, …).
struct GpuTexture {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkImageView srgbView = VK_NULL_HANDLE;
    VkImageView linearView = VK_NULL_HANDLE;
};

// Shared state behind lb::render::Renderer. Member order matters: the context is destroyed last.
struct Renderer::Impl {
    explicit Impl(const RendererOptions& options);
    ~Impl();
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    // True when the handle names a live texture (released slots are empty).
    bool hasTexture(TextureHandle handle) const {
        return handle.valid() && handle.index < textures.size() &&
               textures[handle.index].image != VK_NULL_HANDLE;
    }
    // Throws when every bindless slot is taken.
    void checkTextureCapacity() const;
    // Puts a created texture into a free slot of the bindless table.
    TextureHandle addTexture(const GpuTexture& texture);

    GpuContext context;
    Pipelines pipelines;
    std::vector<GpuMesh> meshes;            // indexed by MeshHandle; released slots are empty
    std::vector<std::uint32_t> freeMeshSlots;
    std::vector<GpuTexture> textures;       // indexed by TextureHandle = bindless array slot
    std::vector<std::uint32_t> freeTextureSlots;

    // Descriptor set 0 (see pipelines.hpp): the material sampler and every texture's two views.
    VkSampler materialSampler = VK_NULL_HANDLE;
    VkDescriptorPool texturePool = VK_NULL_HANDLE;
    VkDescriptorSet textureSet = VK_NULL_HANDLE;
    bool linearBlit = false; // RGBA8 supports linear-filtered blits (mip generation quality)
    bool srgbBlit = false;   // sRGB RGBA8 can be blitted with linear filtering (color map mips)

private:
    void createTextureTable();
    void destroyTextureTable();
};

std::unique_ptr<Presenter> makePresenter(Renderer::Impl& renderer, VkSurfaceKHR surface,
                                         std::uint32_t width, std::uint32_t height);

} // namespace lb::render
