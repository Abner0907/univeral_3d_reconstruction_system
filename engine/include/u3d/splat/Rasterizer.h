#pragma once

#include "u3d/Camera.h"
#include "u3d/ThreadPool.h"
#include "u3d/splat/GaussianModel.h"

#include <atomic>
#include <memory>
#include <vector>

namespace u3d {

struct RenderCamera {
    int width = 0;
    int height = 0;
    float fx = 0, fy = 0, cx = 0, cy = 0;
    float R[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; // world-to-camera, row-major, OpenCV convention
    float t[3] = {0, 0, 0};

    static RenderCamera fromCamera(const Camera &camera);
};

struct RasterSettings {
    int tileSize = 16;
    float nearPlane = 0.01f;
    float eps2d = 0.3f; // low-pass filter added to the 2D covariance (as in 3DGS / gsplat)
    float background[3] = {0.0f, 0.0f, 0.0f};
    // Contributions below minAlpha are skipped and compositing stops once transmittance drops
    // below minTransmittance (3DGS defaults). Tests set both to 0 to make rendering smooth.
    float minAlpha = 1.0f / 255.0f;
    float minTransmittance = 1e-4f;
};

// Differentiable tile-based Gaussian splat rasterizer on the CPU - the algorithm of 3DGS /
// gsplat (EWA projection, per-tile depth-sorted alpha compositing, analytic backward pass),
// multithreaded over image tiles. Pixel (x, y) is sampled at its centre, which is the integer
// coordinate (x, y) in the OpenCV camera convention.
class Rasterizer
{
public:
    explicit Rasterizer(ThreadPool &pool);

    // Renders into rgb (height * width * 3, row-major) and keeps the state for backward().
    void forward(const GaussianModel &model, const RenderCamera &camera, const RasterSettings &settings,
                 std::vector<float> &rgb);

    // Given dL/d(rendered rgb) of the last forward(), ADDS the parameter gradients to `grads`
    // (which must have the model's size). If mean2dGradNdc is non-null (size N), adds
    // |dL/d mean2d| in normalised device coordinates - the densification statistic of 3DGS.
    void backward(const GaussianModel &model, const float *dLdRgb, GaussianModel &grads,
                  float *mean2dGradNdc = nullptr);

    // Screen-space radius (pixels) of every Gaussian in the last forward(); 0 = not visible.
    const std::vector<int> &radii() const { return m_radius; }

private:
    void project(const GaussianModel &model);
    void binTiles();

    ThreadPool &m_pool;
    RenderCamera m_cam;
    RasterSettings m_settings;
    std::size_t m_n = 0;

    // Per Gaussian (from the last forward).
    std::vector<float> m_mean2d;  // 2
    std::vector<float> m_conic;   // 3: A, B, C of the inverse 2D covariance
    std::vector<float> m_cov2d;   // 3: a, b, c (including eps2d)
    std::vector<float> m_depth;
    std::vector<float> m_rgb;     // 3, after clamping to >= 0
    std::vector<float> m_alpha;   // sigmoid(opacity)
    std::vector<int> m_radius;
    std::vector<int> m_rect;      // 4: tile x0, y0, x1, y1 (exclusive)
    std::vector<std::uint8_t> m_flags; // bit0/1: x/y clamped in J, bit2..4: colour clamped

    // Tiles.
    int m_tilesX = 0;
    int m_tilesY = 0;
    std::vector<int> m_tileOffsets;
    std::vector<int> m_tileList;

    // Pixels.
    std::vector<float> m_finalT;
    std::vector<int> m_nContrib;

    // Backward: per-Gaussian 2D gradients (mean2d xy, conic ABC, rgb, alpha), accumulated
    // atomically across tiles.
    std::unique_ptr<std::atomic<float>[]> m_grad2d;
    std::size_t m_grad2dCapacity = 0;
};

} // namespace u3d
