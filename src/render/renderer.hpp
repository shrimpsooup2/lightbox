#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

// Vulkan handle types only, for interop with the windowing layer (VK_NO_PROTOTYPES is defined).
#include <vulkan/vulkan_core.h>

#include "core/image.hpp"
#include "core/mesh.hpp"
#include "render/types.hpp"

namespace lb::render {

class Presenter;

struct RendererOptions {
    bool enableValidation = false; // needs VK_LAYER_KHRONOS_validation (Vulkan SDK)
    std::filesystem::path pipelineCache; // compiled shaders kept between runs (empty: none)
};

// Owns the Vulkan instance, device and GPU resources shared by all viewports. Window-system agnostic:
// the caller creates a VkSurfaceKHR for vkInstance() and hands it to createPresenter().
// Throws std::runtime_error with a user-presentable message if no suitable GPU is available.
class Renderer {
public:
    explicit Renderer(const RendererOptions& options = {});
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    VkInstance vkInstance() const;
    const DeviceInfo& deviceInfo() const;

    MeshHandle uploadMesh(const MeshData& mesh);
    // Frees the mesh's GPU memory; the handle becomes invalid. Waits for the GPU to go idle, so it is
    // meant for occasional edits (deleting a resource), not per-frame use. Unknown handles are ignored.
    void releaseMesh(MeshHandle handle);

    // Uploads an RGBA8 image with a full mip chain, usable both as sRGB (base color) and linear (data
    // maps). `color`: mainly used as a color map, so the mips are averaged in linear light (data
    // maps such as normals must be averaged as stored). Blocking. Throws std::invalid_argument for
    // an empty or inconsistent image and std::runtime_error when the GPU texture limit (bindless
    // capacity, ≤ 4096) is reached.
    TextureHandle uploadTexture(const ImageData& image, bool color);
    // Uploads a sky image (RGBA16F with mips) into the same bindless table; release it with
    // releaseTexture. Blocking. Same errors as uploadTexture.
    TextureHandle uploadEnvironmentMap(const EnvironmentMap& map);
    // Same rules as releaseMesh (waits for the GPU; unknown handles are ignored).
    void releaseTexture(TextureHandle handle);

    // The presenter must be destroyed before the surface and before this renderer.
    std::unique_ptr<Presenter> createPresenter(VkSurfaceKHR surface, std::uint32_t width,
                                               std::uint32_t height);
    // A presenter without a window: renders into an image of the given size that every render()
    // can read back (the render screen's final images, at any resolution). No MSAA (the path
    // tracer antialiases); resize() changes the output size.
    std::unique_ptr<Presenter> createOffscreenPresenter(std::uint32_t width, std::uint32_t height);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// Renders frames into one window surface (swapchain + per-frame resources).
class Presenter {
public:
    struct Impl;
    explicit Presenter(std::unique_ptr<Impl> impl);
    ~Presenter();
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    void resize(std::uint32_t width, std::uint32_t height);
    // Returns false when no image was presented (minimized, or swapchain being recreated).
    // With `capture`, the presented image is also read back (blocking until the GPU finishes);
    // capture->pixels stays empty if the surface does not support readback.
    bool render(const FrameInput& input, CapturedImage* capture = nullptr);

    std::uint32_t width() const;
    std::uint32_t height() const;
    // Progress of the ray-traced view as of the last rendered frame.
    RayTracingStatus rayTracingStatus() const;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace lb::render
