#pragma once

#include <cstdint>
#include <filesystem>
#include <future>
#include <utility>
#include <vector>

#include "render/vulkan.hpp"

namespace lb::render {

struct PipelineKey {
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;

    bool operator==(const PipelineKey&) const = default;
};

struct PipelineSet {
    VkPipeline sky = VK_NULL_HANDLE;  // fullscreen sky gradient, no depth
    VkPipeline mesh = VK_NULL_HANDLE; // opaque lit meshes, depth write
    VkPipeline grid = VK_NULL_HANDLE; // infinite ground grid, depth test, alpha blended
    VkPipeline glass = VK_NULL_HANDLE; // see-through meshes: depth test, no depth write,
                                       // premultiplied alpha
};

// Pipelines that write the presented image (swapchain format, single sample, no depth).
struct OutputPipelines {
    VkPipeline composite = VK_NULL_HANDLE; // HDR image → look → display (post layout)
    VkPipeline overlay = VK_NULL_HANDLE;   // gizmo lines on top, alpha blended (scene layout)
};

// Linear HDR scene image: raster passes render (or MSAA-resolve) into it, the composite reads it.
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

// Format of the sun shadow map (depth only, standard 0 = near depth, compare LESS_OR_EQUAL).
constexpr VkFormat kShadowMapFormat = VK_FORMAT_D32_SFLOAT;

// Owns shader modules, the descriptor set and pipeline layouts, the shadow pipeline and the scene
// pipelines per render-target format.
//
// Descriptor set 0 (renderer-wide, update-after-bind; owned by Renderer::Impl):
//   binding 0 = material sampler, binding 1 = sRGB texture views[capacity],
//   binding 2 = linear texture views[capacity] (partially bound; index = TextureHandle).
// Descriptor set 1 (per presenter): binding 0 = sun shadow map (combined image sampler).
// Post layout (composite, auto exposure): set 0 binding 0 = raster HDR image, binding 1 = path
// tracer accumulation (combined image samplers); push constants = PostPushConstants.
constexpr std::uint32_t kTextureSet = 0;
constexpr std::uint32_t kShadowSet = 1;
constexpr std::uint32_t kSamplerBinding = 0;
constexpr std::uint32_t kSrgbTexturesBinding = 1;
constexpr std::uint32_t kLinearTexturesBinding = 2;

class Pipelines {
public:
    // `textureCapacity`: array size of the bindless texture bindings in set 0. `pathTracing`: the
    // device has non-uniform texture indexing enabled (the path tracer needs it). `cachePath`:
    // compiled pipelines are kept there between runs (empty: no cache file).
    Pipelines(VkDevice device, std::uint32_t textureCapacity, bool pathTracing,
              std::filesystem::path cachePath);
    ~Pipelines();
    Pipelines(const Pipelines&) = delete;
    Pipelines& operator=(const Pipelines&) = delete;

    VkPipelineLayout layout() const { return layout_; }
    VkDescriptorSetLayout textureSetLayout() const { return textureSetLayout_; }
    VkDescriptorSetLayout shadowSetLayout() const { return shadowSetLayout_; }
    VkPipeline shadow() const { return shadow_; }
    // Creates the set on first use for a given target format. The returned reference is valid until
    // the next call with a new key.
    const PipelineSet& get(const PipelineKey& key);
    // Path tracer (compute): set 0 = textures, set 1 = accumulation image; PathPushConstants.
    // Compiled in the background: null until ready, and always null when unsupported.
    VkPipeline pathTrace();
    bool pathTracingSupported() const { return pathTracingSupported_; }
    VkPipelineLayout pathLayout() const { return pathLayout_; }
    VkDescriptorSetLayout accumulationSetLayout() const { return accumulationSetLayout_; }
    // Auto exposure measurement (compute, post layout).
    VkPipeline exposure() const { return exposure_; }
    VkPipelineLayout postLayout() const { return postLayout_; }
    VkDescriptorSetLayout postSetLayout() const { return postSetLayout_; }
    // Composite and overlay pipelines for a presentation format (created on first use, cached).
    const OutputPipelines& output(VkFormat format);

private:
    PipelineSet create(const PipelineKey& key) const;

    VkDevice device_;
    VkDescriptorSetLayout textureSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout shadowSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout postSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout postLayout_ = VK_NULL_HANDLE;
    VkShaderModule compositeFrag_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout accumulationSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pathLayout_ = VK_NULL_HANDLE;
    VkPipeline pathTrace_ = VK_NULL_HANDLE;
    std::future<VkPipeline> pathTraceBuild_;
    std::filesystem::path cachePath_;
    VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
    bool pathTracingSupported_ = false;
    VkPipeline exposure_ = VK_NULL_HANDLE;
    VkShaderModule fullscreenVert_ = VK_NULL_HANDLE;
    VkShaderModule skyFrag_ = VK_NULL_HANDLE;
    VkShaderModule gridFrag_ = VK_NULL_HANDLE;
    VkShaderModule meshVert_ = VK_NULL_HANDLE;
    VkShaderModule meshFrag_ = VK_NULL_HANDLE;
    VkShaderModule shadowVert_ = VK_NULL_HANDLE;
    VkShaderModule overlayVert_ = VK_NULL_HANDLE;
    VkShaderModule overlayFrag_ = VK_NULL_HANDLE;
    VkPipeline shadow_ = VK_NULL_HANDLE;
    std::vector<std::pair<PipelineKey, PipelineSet>> cache_;
    std::vector<std::pair<VkFormat, OutputPipelines>> outputCache_;
};

} // namespace lb::render
