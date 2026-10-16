#pragma once

#include <cstdint>
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
};

// Format of the sun shadow map (depth only, standard 0 = near depth, compare LESS_OR_EQUAL).
constexpr VkFormat kShadowMapFormat = VK_FORMAT_D32_SFLOAT;

// Owns shader modules, the descriptor set and pipeline layouts, the shadow pipeline and the scene
// pipelines per render-target format.
//
// Descriptor set 0 (renderer-wide, update-after-bind; owned by Renderer::Impl):
//   binding 0 = material sampler, binding 1 = sRGB texture views[capacity],
//   binding 2 = linear texture views[capacity] (partially bound; index = TextureHandle).
// Descriptor set 1 (per presenter): binding 0 = sun shadow map (combined image sampler).
constexpr std::uint32_t kTextureSet = 0;
constexpr std::uint32_t kShadowSet = 1;
constexpr std::uint32_t kSamplerBinding = 0;
constexpr std::uint32_t kSrgbTexturesBinding = 1;
constexpr std::uint32_t kLinearTexturesBinding = 2;

class Pipelines {
public:
    // `textureCapacity`: array size of the bindless texture bindings in set 0.
    Pipelines(VkDevice device, std::uint32_t textureCapacity);
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

private:
    PipelineSet create(const PipelineKey& key) const;

    VkDevice device_;
    VkDescriptorSetLayout textureSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout shadowSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule fullscreenVert_ = VK_NULL_HANDLE;
    VkShaderModule skyFrag_ = VK_NULL_HANDLE;
    VkShaderModule gridFrag_ = VK_NULL_HANDLE;
    VkShaderModule meshVert_ = VK_NULL_HANDLE;
    VkShaderModule meshFrag_ = VK_NULL_HANDLE;
    VkShaderModule shadowVert_ = VK_NULL_HANDLE;
    VkPipeline shadow_ = VK_NULL_HANDLE;
    std::vector<std::pair<PipelineKey, PipelineSet>> cache_;
};

} // namespace lb::render
