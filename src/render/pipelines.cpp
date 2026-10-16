#include "render/pipelines.hpp"

#include <cstdint>

#include "render/frame_data.hpp"

// Generated at build time by lightbox_add_shaders() (cmake/LightBoxShaders.cmake).
#include "shaders/fullscreen.vert.h"
#include "shaders/grid.frag.h"
#include "shaders/mesh.frag.h"
#include "shaders/mesh.vert.h"
#include "shaders/shadow.vert.h"
#include "shaders/sky.frag.h"

namespace lb::render {
namespace {

template <std::size_t N>
VkShaderModule createModule(VkDevice device, const std::uint32_t (&code)[N]) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = sizeof(code);
    info.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    LB_VK_CHECK(vkCreateShaderModule(device, &info, nullptr, &module));
    return module;
}

struct PipelineDesc {
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE; // none = depth-only
    VkFormat colorFormat = VK_FORMAT_UNDEFINED; // undefined = no color attachment
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    bool meshVertices = false;
    bool depthTest = false;
    bool depthWrite = false;
    bool alphaBlend = false;
    // Shadow pass: standard depth (closer = smaller, compare LESS_OR_EQUAL) plus slope-scaled bias.
    // Scene passes use reverse-Z (compare GREATER).
    bool shadowDepth = false;
};

VkPipeline buildPipeline(VkDevice device, VkPipelineLayout layout, const PipelineDesc& desc) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    std::uint32_t stageCount = 1;
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = desc.vertex;
    stages[0].pName = "main";
    if (desc.fragment) {
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = desc.fragment;
        stages[1].pName = "main";
        stageCount = 2;
    }

    const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    if (desc.meshVertices) {
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        // The shadow pass reads positions only (same stride, so the same vertex buffers bind).
        vertexInput.vertexAttributeDescriptionCount = desc.shadowDepth ? 1 : 3;
        vertexInput.pVertexAttributeDescriptions = attributes;
    }

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    // No culling: CAD geometry has unreliable winding, shading is two-sided (see AGENTS.md).
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    if (desc.shadowDepth) {
        raster.depthBiasEnable = VK_TRUE;
        raster.depthBiasConstantFactor = 1.5f;
        raster.depthBiasSlopeFactor = 2.0f;
    }

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = desc.samples;

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = desc.depthTest ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = desc.depthWrite ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp = desc.shadowDepth ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_GREATER;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (desc.alphaBlend) {
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    const bool hasColor = desc.colorFormat != VK_FORMAT_UNDEFINED;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = hasColor ? 1 : 0;
    blend.pAttachments = &blendAttachment;

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = hasColor ? 1 : 0;
    rendering.pColorAttachmentFormats = hasColor ? &desc.colorFormat : nullptr;
    rendering.depthAttachmentFormat = desc.depthFormat;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = stageCount;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    LB_VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline));
    return pipeline;
}

} // namespace

Pipelines::Pipelines(VkDevice device, std::uint32_t textureCapacity) : device_(device) {
    fullscreenVert_ = createModule(device_, lb_spv_fullscreen_vert);
    skyFrag_ = createModule(device_, lb_spv_sky_frag);
    gridFrag_ = createModule(device_, lb_spv_grid_frag);
    meshVert_ = createModule(device_, lb_spv_mesh_vert);
    meshFrag_ = createModule(device_, lb_spv_mesh_frag);
    shadowVert_ = createModule(device_, lb_spv_shadow_vert);

    // Set 0: bindless material textures. Slots are written as textures are uploaded (also while
    // earlier frames are in flight) and only slots referenced by materials are ever read.
    const VkDescriptorSetLayoutBinding textureBindings[] = {
        {kSamplerBinding, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {kSrgbTexturesBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, textureCapacity,
         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {kLinearTexturesBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, textureCapacity,
         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    constexpr VkDescriptorBindingFlags kBindlessFlags =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    const VkDescriptorBindingFlags textureBindingFlags[] = {0, kBindlessFlags, kBindlessFlags};
    VkDescriptorSetLayoutBindingFlagsCreateInfo textureFlagsInfo{};
    textureFlagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    textureFlagsInfo.bindingCount = 3;
    textureFlagsInfo.pBindingFlags = textureBindingFlags;
    VkDescriptorSetLayoutCreateInfo textureSetInfo{};
    textureSetInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    textureSetInfo.pNext = &textureFlagsInfo;
    textureSetInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    textureSetInfo.bindingCount = 3;
    textureSetInfo.pBindings = textureBindings;
    LB_VK_CHECK(
        vkCreateDescriptorSetLayout(device_, &textureSetInfo, nullptr, &textureSetLayout_));

    // Set 1: the presenter's sun shadow map.
    const VkDescriptorSetLayoutBinding shadowBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                     VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo shadowSetInfo{};
    shadowSetInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    shadowSetInfo.bindingCount = 1;
    shadowSetInfo.pBindings = &shadowBinding;
    LB_VK_CHECK(vkCreateDescriptorSetLayout(device_, &shadowSetInfo, nullptr, &shadowSetLayout_));

    const VkDescriptorSetLayout setLayouts[] = {textureSetLayout_, shadowSetLayout_};
    static_assert(kTextureSet == 0 && kShadowSet == 1, "setLayouts order");
    const VkPushConstantRange pushRange{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                        sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = setLayouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    LB_VK_CHECK(vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &layout_));

    shadow_ = buildPipeline(device_, layout_,
                            {.vertex = shadowVert_,
                             .depthFormat = kShadowMapFormat,
                             .meshVertices = true,
                             .depthTest = true,
                             .depthWrite = true,
                             .shadowDepth = true});
}

Pipelines::~Pipelines() {
    for (auto& [key, set] : cache_) {
        vkDestroyPipeline(device_, set.sky, nullptr);
        vkDestroyPipeline(device_, set.mesh, nullptr);
        vkDestroyPipeline(device_, set.grid, nullptr);
    }
    vkDestroyPipeline(device_, shadow_, nullptr);
    vkDestroyPipelineLayout(device_, layout_, nullptr);
    vkDestroyDescriptorSetLayout(device_, shadowSetLayout_, nullptr);
    vkDestroyDescriptorSetLayout(device_, textureSetLayout_, nullptr);
    for (VkShaderModule module :
         {fullscreenVert_, skyFrag_, gridFrag_, meshVert_, meshFrag_, shadowVert_}) {
        vkDestroyShaderModule(device_, module, nullptr);
    }
}

const PipelineSet& Pipelines::get(const PipelineKey& key) {
    for (const auto& [existing, set] : cache_) {
        if (existing == key) {
            return set;
        }
    }
    cache_.emplace_back(key, create(key));
    return cache_.back().second;
}

PipelineSet Pipelines::create(const PipelineKey& key) const {
    PipelineDesc base;
    base.colorFormat = key.colorFormat;
    base.depthFormat = key.depthFormat;
    base.samples = key.samples;

    PipelineSet set;
    PipelineDesc sky = base;
    sky.vertex = fullscreenVert_;
    sky.fragment = skyFrag_;
    set.sky = buildPipeline(device_, layout_, sky);

    PipelineDesc mesh = base;
    mesh.vertex = meshVert_;
    mesh.fragment = meshFrag_;
    mesh.meshVertices = true;
    mesh.depthTest = true;
    mesh.depthWrite = true;
    set.mesh = buildPipeline(device_, layout_, mesh);

    PipelineDesc grid = base;
    grid.vertex = fullscreenVert_;
    grid.fragment = gridFrag_;
    grid.depthTest = true;
    grid.alphaBlend = true;
    set.grid = buildPipeline(device_, layout_, grid);
    return set;
}

} // namespace lb::render
