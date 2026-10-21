#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>

#include <glm/mat4x4.hpp>

#include "render/gpu_context.hpp"
#include "render/pipelines.hpp"
#include "render/ray_scene.hpp"
#include "render/types.hpp"

namespace lb::render {

// The progressive path tracer of one presenter (D-027): uploads the ray scene, owns the
// accumulation image (rgb = sum of samples, a = sample count per pixel), restarts it when anything
// that affects the image changes, and sizes each frame's work to a GPU time budget measured with
// timestamp queries, so slow GPUs take longer instead of stalling and fast ones finish sooner.
class PathTracer {
public:
    static constexpr std::uint32_t kSlots = 2; // frames in flight

    PathTracer(GpuContext& ctx, Pipelines& pipelines);
    ~PathTracer();
    PathTracer(const PathTracer&) = delete;
    PathTracer& operator=(const PathTracer&) = delete;

    bool supported() const { return pipelines_.pathTracingSupported(); }

    // Recreates the accumulation image (call with the device idle).
    void resize(VkExtent2D extent);
    const Image& accumulation() const { return accumulation_; }

    // Before recording frame `slot` (its fence has been waited on): reads that slot's timestamps,
    // uploads a new ray scene, and decides whether accumulation restarts. Returns true when the
    // path tracer runs or shows this frame.
    bool prepare(const FrameInput& input, std::uint32_t slot);
    // Records this frame's sample dispatches (only after prepare returned true). `exposure` is the
    // auto exposure state (ExposureState) the firefly clamp follows.
    void record(VkCommandBuffer cmd, std::uint32_t slot, VkDescriptorSet textureSet,
                VkDeviceAddress frameData, VkDeviceAddress materials,
                std::uint32_t materialCount, VkDeviceAddress exposure);

    // The composite should read the accumulation (some pixels have samples).
    bool showing() const { return showing_; }
    // Every pixel has at least one sample: the raster passes can be skipped.
    bool covered() const { return showing_ && samples_ >= 1; }
    RayTracingStatus status() const;

private:
    void uploadScene(const std::shared_ptr<const RayScene>& scene);
    void destroySceneBuffers();
    void restart();

    GpuContext& ctx_;
    Pipelines& pipelines_;
    VkExtent2D extent_{0, 0};
    Image accumulation_;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkQueryPool queries_ = VK_NULL_HANDLE;

    // Uploaded scene.
    std::shared_ptr<const RayScene> scene_;
    Buffer vertices_;
    Buffer triangles_;
    Buffer nodes_;
    Buffer instances_;
    std::uint32_t nodeCount_ = 0;

    // Importance sampling data of the current sky image (none: procedural sky).
    std::shared_ptr<const EnvironmentMap> sky_;
    Buffer skySampling_;

    // What the current image was started from (any change restarts it).
    glm::mat4 viewProj_{0.0f};
    std::uint64_t sceneVersion_ = 0;
    RayQuality quality_ = RayQuality::Good;
    bool clear_ = true; // clear the accumulation before the next dispatch

    // Progress.
    bool enabled_ = false;
    bool preparing_ = false;
    bool showing_ = false;
    std::uint32_t samples_ = 0;   // complete passes over the image
    std::uint32_t target_ = 0;    // passes wanted (quality)
    std::uint32_t bounces_ = 6;
    std::uint32_t row_ = 0;       // next row of the pass in progress
    std::uint64_t sampleSeed_ = 0;
    RaySpeed speed_ = RaySpeed::Balanced;
    std::chrono::steady_clock::time_point started_;
    double seconds_ = 0.0;

    // Adaptive budget: measured pixel-samples per GPU millisecond.
    double rate_ = 6000.0;
    std::array<std::uint64_t, kSlots> pixelsInSlot_{};
    std::array<bool, kSlots> queryPending_{};
    std::uint64_t pixelsThisFrame_ = 0;
};

} // namespace lb::render
