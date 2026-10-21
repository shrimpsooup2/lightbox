#include "render/path_tracer.hpp"

#include <algorithm>
#include <cmath>

#include "core/environment_map.hpp"
#include "core/log.hpp"
#include "render/frame_data.hpp"

namespace lb::render {
namespace {

struct QualityPreset {
    std::uint32_t samples;
    std::uint32_t bounces;
};

QualityPreset preset(RayQuality quality) {
    switch (quality) {
    case RayQuality::Draft: return {64, 4};
    case RayQuality::Good: return {256, 6};
    case RayQuality::Final: return {1024, 8};
    case RayQuality::Ultra: return {4096, 12};
    }
    return {256, 6};
}

// GPU milliseconds per frame the path tracer may use. Gentle leaves most of the GPU (and battery)
// alone; Fast keeps it busy but frames stay short enough for the interface to respond.
double budgetMs(RaySpeed speed) {
    switch (speed) {
    case RaySpeed::Gentle: return 5.0;
    case RaySpeed::Balanced: return 12.0;
    case RaySpeed::Fast: return 30.0;
    }
    return 12.0;
}

constexpr std::uint32_t kMaxDispatchesPerFrame = 32;
constexpr std::uint32_t kGroup = 8; // pathtrace.comp local size

void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

} // namespace

PathTracer::PathTracer(GpuContext& ctx, Pipelines& pipelines) : ctx_(ctx), pipelines_(pipelines) {
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &size;
    LB_VK_CHECK(vkCreateDescriptorPool(ctx_.device, &poolInfo, nullptr, &pool_));
    const VkDescriptorSetLayout layout = pipelines_.accumulationSetLayout();
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = pool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout;
    LB_VK_CHECK(vkAllocateDescriptorSets(ctx_.device, &allocInfo, &set_));
    if (ctx_.timestamps) {
        VkQueryPoolCreateInfo queryInfo{};
        queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = 2 * kSlots;
        LB_VK_CHECK(vkCreateQueryPool(ctx_.device, &queryInfo, nullptr, &queries_));
    }
}

PathTracer::~PathTracer() {
    destroySceneBuffers();
    ctx_.destroyBuffer(skySampling_);
    ctx_.destroyImage(accumulation_);
    if (queries_) {
        vkDestroyQueryPool(ctx_.device, queries_, nullptr);
    }
    vkDestroyDescriptorPool(ctx_.device, pool_, nullptr);
}

void PathTracer::resize(VkExtent2D extent) {
    ctx_.destroyImage(accumulation_);
    extent_ = extent;
    accumulation_ = ctx_.createImage2D(
        VK_FORMAT_R32G32B32A32_SFLOAT, extent,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    // Kept in GENERAL for its whole life (storage writes, sampled reads, clears), starting empty.
    ctx_.immediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = accumulation_.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
        const VkClearColorValue zero{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, accumulation_.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        memoryBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    const VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, accumulation_.view,
                                          VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set_;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(ctx_.device, 1, &write, 0, nullptr);
    restart();
    clear_ = false; // just cleared
}

void PathTracer::destroySceneBuffers() {
    ctx_.destroyBuffer(vertices_);
    ctx_.destroyBuffer(triangles_);
    ctx_.destroyBuffer(nodes_);
    ctx_.destroyBuffer(instances_);
    nodeCount_ = 0;
}

void PathTracer::uploadScene(const std::shared_ptr<const RayScene>& scene) {
    // Rare (after geometry edits): wait for frames in flight instead of deferring the deletion.
    LB_VK_CHECK(vkDeviceWaitIdle(ctx_.device));
    destroySceneBuffers();
    scene_ = scene;
    const auto upload = [&](Buffer& buffer, const auto& items) {
        using Item = typename std::decay_t<decltype(items)>::value_type;
        const VkDeviceSize bytes = std::max<VkDeviceSize>(items.size() * sizeof(Item), 16);
        buffer = ctx_.createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                   BufferMemory::DeviceLocal);
        if (!items.empty()) {
            ctx_.upload(buffer, items.data(), items.size() * sizeof(Item));
        }
    };
    upload(vertices_, scene->vertices);
    upload(triangles_, scene->triangles);
    upload(nodes_, scene->nodes);
    upload(instances_, scene->instances);
    nodeCount_ = static_cast<std::uint32_t>(scene->nodes.size());
    log::info("Ray tracing scene: {} triangles, {} BVH nodes", scene->triangles.size(),
              scene->nodes.size());
}

void PathTracer::restart() {
    clear_ = true;
    samples_ = 0;
    row_ = 0;
    started_ = std::chrono::steady_clock::now();
    seconds_ = 0.0;
}

bool PathTracer::prepare(const FrameInput& input, std::uint32_t slot) {
    // This slot's previous frame has finished: its timestamps tell how fast the GPU really was.
    if (queries_ && queryPending_[slot]) {
        std::array<std::uint64_t, 2> stamps{};
        if (vkGetQueryPoolResults(ctx_.device, queries_, 2 * slot, 2, sizeof(stamps), stamps.data(),
                                  sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const double ms =
                static_cast<double>(stamps[1] - stamps[0]) * ctx_.timestampPeriodNs / 1.0e6;
            if (ms > 0.05 && pixelsInSlot_[slot] > 0) {
                const double measured = static_cast<double>(pixelsInSlot_[slot]) / ms;
                rate_ = std::clamp(rate_ * 0.7 + measured * 0.3, 200.0, 5.0e7);
            }
        }
        queryPending_[slot] = false;
    }

    const RayTracingSettings& settings = input.rayTracing;
    enabled_ = settings.enabled && supported();
    preparing_ = enabled_ && !input.rayScene;
    showing_ = false;
    if (!enabled_) {
        return false;
    }
    speed_ = settings.speed;
    const QualityPreset quality = preset(settings.quality);
    target_ = quality.samples;
    bounces_ = quality.bounces;

    const glm::mat4 viewProj = input.projection * input.view;
    if (viewProj != viewProj_ || input.sceneVersion != sceneVersion_ ||
        settings.quality != quality_ || input.rayScene != scene_ ||
        input.lighting.skyImage != sky_) {
        viewProj_ = viewProj;
        sceneVersion_ = input.sceneVersion;
        quality_ = settings.quality;
        restart();
    }
    if (input.lighting.skyImage != sky_) {
        // Rare (choosing a sky): wait for frames in flight instead of deferring the deletion.
        LB_VK_CHECK(vkDeviceWaitIdle(ctx_.device));
        ctx_.destroyBuffer(skySampling_);
        sky_ = input.lighting.skyImage;
        if (sky_) {
            std::vector<float> data;
            data.reserve(sky_->rowCdf.size() + sky_->columnCdf.size() +
                         sky_->cellProbability.size());
            data.insert(data.end(), sky_->rowCdf.begin(), sky_->rowCdf.end());
            data.insert(data.end(), sky_->columnCdf.begin(), sky_->columnCdf.end());
            data.insert(data.end(), sky_->cellProbability.begin(), sky_->cellProbability.end());
            const VkDeviceSize bytes = data.size() * sizeof(float);
            skySampling_ = ctx_.createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                             BufferMemory::DeviceLocal);
            ctx_.upload(skySampling_, data.data(), bytes);
        }
    }
    if (pipelines_.pathTrace() == VK_NULL_HANDLE) {
        preparing_ = true; // the shader is still compiling (first run on this GPU)
        return false;
    }
    if (!input.rayScene) {
        return false; // still preparing: the raster view stays
    }
    if (input.rayScene != scene_) {
        uploadScene(input.rayScene);
    }
    if (settings.interacting) {
        return false; // moving: the raster view, the image restarts once things are still
    }
    if (samples_ < target_) {
        seconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    }
    showing_ = true;
    return true;
}

void PathTracer::record(VkCommandBuffer cmd, std::uint32_t slot, VkDescriptorSet textureSet,
                        VkDeviceAddress frameData, VkDeviceAddress materials,
                        std::uint32_t materialCount, VkDeviceAddress exposure) {
    pixelsThisFrame_ = 0;
    if (clear_) {
        // The previous frame's composite may still read it: order the clear after that.
        memoryBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkClearColorValue zero{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, accumulation_.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        memoryBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        clear_ = false;
    }
    if (samples_ >= target_ || extent_.width == 0 || extent_.height == 0) {
        return; // converged: the GPU rests
    }

    const std::uint64_t width = extent_.width;
    const std::uint64_t pixels = width * extent_.height;
    std::uint64_t work = static_cast<std::uint64_t>(rate_ * budgetMs(speed_));
    work = std::clamp<std::uint64_t>(work, 64 * width / 8, pixels * 16);

    // Earlier reads of the accumulation (composite, exposure) before this frame's writes.
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    if (queries_) {
        vkCmdResetQueryPool(cmd, queries_, 2 * slot, 2);
        // Starts once everything before (the raster passes) has finished: only path tracing counts.
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries_, 2 * slot);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_.pathTrace());
    const VkDescriptorSet sets[] = {textureSet, set_};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_.pathLayout(), 0, 2,
                            sets, 0, nullptr);
    PathPushConstants push{};
    push.frame = frameData;
    push.materials = materials;
    push.vertices = vertices_.address;
    push.triangles = triangles_.address;
    push.nodes = nodes_.address;
    push.instances = instances_.address;
    push.skySampling = skySampling_.address;
    push.exposure = exposure;

    for (std::uint32_t dispatch = 0;
         dispatch < kMaxDispatchesPerFrame && work > 0 && samples_ < target_; ++dispatch) {
        const std::uint32_t rows = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            extent_.height - row_, std::max<std::uint64_t>((work + width - 1) / width, 1)));
        push.tile = glm::uvec4(0u, row_, extent_.width, rows);
        push.params = glm::uvec4(static_cast<std::uint32_t>(sampleSeed_), bounces_, nodeCount_,
                                 std::max(materialCount, 1u));
        vkCmdPushConstants(cmd, pipelines_.pathLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(push), &push);
        vkCmdDispatch(cmd, (extent_.width + kGroup - 1) / kGroup, (rows + kGroup - 1) / kGroup, 1);
        const std::uint64_t done = static_cast<std::uint64_t>(rows) * width;
        pixelsThisFrame_ += done;
        work = work > done ? work - done : 0;
        row_ += rows;
        if (row_ >= extent_.height) {
            row_ = 0;
            ++samples_;
            ++sampleSeed_;
        }
        // The next dispatch may cover the same pixels (several passes per frame on fast GPUs).
        memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    if (queries_) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, queries_, 2 * slot + 1);
        queryPending_[slot] = true;
        pixelsInSlot_[slot] = pixelsThisFrame_;
    }
    showing_ = true;
}

RayTracingStatus PathTracer::status() const {
    RayTracingStatus status;
    status.supported = supported();
    status.active = enabled_;
    status.preparing = preparing_;
    status.samples = samples_;
    status.targetSamples = target_;
    status.seconds = seconds_;
    return status;
}

} // namespace lb::render
