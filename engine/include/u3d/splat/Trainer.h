#pragma once

#include "u3d/splat/Adam.h"
#include "u3d/splat/GaussianModel.h"
#include "u3d/splat/Rasterizer.h"

#include <cstdint>
#include <random>
#include <vector>

namespace u3d {

struct TrainImage {
    RenderCamera camera;
    std::vector<std::uint8_t> rgb; // height * width * 3 (8-bit keeps memory low on phones)

    static std::vector<std::uint8_t> quantize(const std::vector<float> &rgb01);
};

struct TrainOptions {
    int iterations = 3000;
    int maxGaussians = 200000;
    float ssimLambda = 0.2f;
    LearningRates lr;                // lr.means is relative to the scene extent
    float meansLrFinal = 1.6e-6f;    // exponential decay target for lr.means
    float densifyGradThreshold = 2e-4f;
    float percentDense = 0.01f;      // clone (small) vs split (large) threshold, x scene extent
    int refineStart = 500;
    int refineStop = 15000;
    int refineInterval = 100;
    int opacityResetInterval = 3000; // 0 = never
    float pruneOpacity = 0.005f;
    unsigned seed = 42;
    RasterSettings raster;

    // 3DGS schedule compressed to a shorter (on-device) run.
    static TrainOptions forIterations(int iterations);
};

struct TrainStats {
    int iteration = 0;
    float loss = 0;  // exponential moving average
    float psnr = 0;  // exponential moving average over training views
    std::size_t gaussians = 0;
};

// Gaussian Splatting optimisation (3DGS / splatfacto algorithm): one random training view per
// iteration, L1 + D-SSIM loss, Adam, adaptive density control (clone / split / prune) and
// periodic opacity reset.
class Trainer
{
public:
    Trainer(GaussianModel initial, std::vector<TrainImage> images, float sceneExtent,
            const TrainOptions &options, ThreadPool &pool);

    void step();
    bool done() const { return m_iteration >= m_options.iterations; }
    int iteration() const { return m_iteration; }
    const GaussianModel &model() const { return m_model; }
    const TrainStats &stats() const { return m_stats; }
    const std::vector<TrainImage> &images() const { return m_images; }

    // Mean PSNR over all training views.
    float evaluatePsnr();
    void render(const RenderCamera &camera, std::vector<float> &rgb);

private:
    void densifyAndPrune();
    void resetOpacity();

    GaussianModel m_model;
    GaussianModel m_grads;
    std::vector<TrainImage> m_images;
    float m_extent;
    TrainOptions m_options;
    ThreadPool &m_pool;
    Rasterizer m_raster;
    Adam m_adam;
    std::mt19937 m_rng;
    std::vector<int> m_order;
    std::size_t m_orderPos = 0;
    int m_iteration = 0;
    bool m_opacityWasReset = false;

    void loadTarget(const TrainImage &img);

    std::vector<float> m_target;
    std::vector<float> m_render;
    std::vector<float> m_dLoss;
    std::vector<float> m_gradAccum; // densification statistics
    std::vector<int> m_gradCount;
    TrainStats m_stats;
};

} // namespace u3d
