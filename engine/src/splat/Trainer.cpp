#include "u3d/splat/Trainer.h"

#include "u3d/splat/Loss.h"

#include <algorithm>
#include <cmath>

namespace u3d {

std::vector<std::uint8_t> TrainImage::quantize(const std::vector<float> &rgb01)
{
    std::vector<std::uint8_t> out(rgb01.size());
    for (std::size_t k = 0; k < rgb01.size(); ++k)
        out[k] = std::uint8_t(std::lround(std::min(1.0f, std::max(0.0f, rgb01[k])) * 255.0f));
    return out;
}

void Trainer::loadTarget(const TrainImage &img)
{
    m_target.resize(img.rgb.size());
    constexpr float inv = 1.0f / 255.0f;
    for (std::size_t k = 0; k < img.rgb.size(); ++k)
        m_target[k] = float(img.rgb[k]) * inv;
}

TrainOptions TrainOptions::forIterations(int iterations)
{
    TrainOptions o;
    o.iterations = std::max(1, iterations);
    o.refineStart = std::min(500, o.iterations / 10);
    o.refineStop = o.iterations * 6 / 10;
    o.refineInterval = 100;
    o.opacityResetInterval = o.iterations >= 6000 ? 3000 : 0;
    return o;
}

Trainer::Trainer(GaussianModel initial, std::vector<TrainImage> images, float sceneExtent,
                 const TrainOptions &options, ThreadPool &pool)
    : m_model(std::move(initial))
    , m_images(std::move(images))
    , m_extent(std::max(sceneExtent, 1e-6f))
    , m_options(options)
    , m_pool(pool)
    , m_raster(pool)
    , m_rng(options.seed)
{
    m_adam.reset(m_model.size());
    m_gradAccum.assign(m_model.size(), 0.0f);
    m_gradCount.assign(m_model.size(), 0);
    m_stats.gaussians = m_model.size();
}

void Trainer::step()
{
    if (m_images.empty() || done())
        return;
    if (m_orderPos >= m_order.size()) {
        m_order.resize(m_images.size());
        for (std::size_t k = 0; k < m_order.size(); ++k)
            m_order[k] = int(k);
        std::shuffle(m_order.begin(), m_order.end(), m_rng);
        m_orderPos = 0;
    }
    const TrainImage &img = m_images[std::size_t(m_order[m_orderPos++])];
    const int W = img.camera.width, H = img.camera.height;

    loadTarget(img);
    m_raster.forward(m_model, img.camera, m_options.raster, m_render);
    m_dLoss.resize(m_render.size());
    const LossValue loss = photometricLoss(m_render.data(), m_target.data(), W, H,
                                           m_options.ssimLambda, m_dLoss.data(), m_pool);

    m_grads.resize(m_model.size());
    m_grads.setZero();
    m_raster.backward(m_model, m_dLoss.data(), m_grads, m_gradAccum.data());
    const std::vector<int> &radii = m_raster.radii();
    for (std::size_t i = 0; i < radii.size(); ++i)
        if (radii[i] > 0)
            ++m_gradCount[i];

    // Exponentially decaying position learning rate, scaled by the scene size.
    LearningRates lr = m_options.lr;
    const float t = float(m_iteration) / float(std::max(1, m_options.iterations - 1));
    lr.means = std::exp(std::log(m_options.lr.means) * (1 - t) + std::log(m_options.meansLrFinal) * t) * m_extent;
    m_adam.step(m_model, m_grads, lr, m_pool);
    ++m_iteration;

    const float p = psnr(m_render.data(), m_target.data(), m_render.size());
    const float k = m_stats.iteration == 0 ? 1.0f : 0.05f;
    m_stats.loss += k * (loss.total - m_stats.loss);
    m_stats.psnr += k * (p - m_stats.psnr);
    m_stats.iteration = m_iteration;

    if (m_iteration >= m_options.refineStart && m_iteration < m_options.refineStop
        && m_iteration % m_options.refineInterval == 0) {
        densifyAndPrune();
    }
    if (m_options.opacityResetInterval > 0 && m_iteration < m_options.refineStop
        && m_iteration % m_options.opacityResetInterval == 0) {
        resetOpacity();
    }
    m_stats.gaussians = m_model.size();
}

void Trainer::densifyAndPrune()
{
    const std::size_t n = m_model.size();
    const float splitScale = m_options.percentDense * m_extent;
    const std::size_t budget = m_options.maxGaussians > int(n) ? std::size_t(m_options.maxGaussians) - n : 0;

    std::vector<std::uint8_t> keep(n, 1);
    GaussianModel extra;
    std::size_t added = 0;
    std::normal_distribution<float> normal(0.0f, 1.0f);

    for (std::size_t i = 0; i < n; ++i) {
        const float maxLogScale = std::max({m_model.scales[i * 3], m_model.scales[i * 3 + 1], m_model.scales[i * 3 + 2]});
        const float maxScale = std::exp(maxLogScale);
        const float alpha = sigmoid(m_model.opacities[i]);
        if (alpha < m_options.pruneOpacity || (m_opacityWasReset && maxScale > 0.1f * m_extent)) {
            keep[i] = 0;
            continue;
        }
        if (m_gradCount[i] == 0)
            continue;
        const float avgGrad = m_gradAccum[i] / float(m_gradCount[i]);
        if (avgGrad < m_options.densifyGradThreshold)
            continue;

        if (maxScale <= splitScale) {
            if (added + 1 > budget)
                continue;
            extra.appendFrom(m_model, i); // clone
            added += 1;
        } else {
            if (added + 2 > budget)
                continue;
            // Split into two smaller Gaussians sampled from the original distribution.
            const float *q = &m_model.quats[i * 4];
            const float qn = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]) + 1e-12f;
            const float w = q[0] / qn, x = q[1] / qn, y = q[2] / qn, z = q[3] / qn;
            const float R[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
                                2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
                                2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)};
            for (int c = 0; c < 2; ++c) {
                const std::size_t j = extra.size();
                extra.appendFrom(m_model, i);
                float local[3];
                for (int k = 0; k < 3; ++k)
                    local[k] = normal(m_rng) * std::exp(m_model.scales[i * 3 + k]);
                for (int r = 0; r < 3; ++r)
                    extra.means[j * 3 + r] += R[r * 3] * local[0] + R[r * 3 + 1] * local[1] + R[r * 3 + 2] * local[2];
                for (int k = 0; k < 3; ++k)
                    extra.scales[j * 3 + k] -= std::log(1.6f);
            }
            keep[i] = 0;
            added += 2;
        }
    }

    m_model.compact(keep);
    m_adam.compact(keep);
    for (std::size_t j = 0; j < extra.size(); ++j)
        m_model.appendFrom(extra, j);
    m_adam.grow(extra.size());

    m_gradAccum.assign(m_model.size(), 0.0f);
    m_gradCount.assign(m_model.size(), 0);
}

void Trainer::resetOpacity()
{
    const float cap = logit(0.01f);
    for (float &o : m_model.opacities)
        o = std::min(o, cap);
    m_adam.resetOpacityMoments();
    m_opacityWasReset = true;
}

float Trainer::evaluatePsnr()
{
    if (m_images.empty())
        return 0;
    double sum = 0;
    std::vector<float> rgb;
    for (const TrainImage &img : m_images) {
        loadTarget(img);
        m_raster.forward(m_model, img.camera, m_options.raster, rgb);
        sum += psnr(rgb.data(), m_target.data(), rgb.size());
    }
    return float(sum / double(m_images.size()));
}

void Trainer::render(const RenderCamera &camera, std::vector<float> &rgb)
{
    m_raster.forward(m_model, camera, m_options.raster, rgb);
}

} // namespace u3d
