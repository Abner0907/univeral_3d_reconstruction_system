#pragma once

#include "u3d/ThreadPool.h"
#include "u3d/splat/GaussianModel.h"

namespace u3d {

struct LearningRates {
    float means = 1.6e-4f;
    float scales = 5e-3f;
    float quats = 1e-3f;
    float opacities = 5e-2f;
    float colors = 2.5e-3f;
};

// Adam (beta1 0.9, beta2 0.999, eps 1e-15 as in 3DGS) with one learning rate per parameter
// group. Moments are stored in GaussianModel layout and follow densification/pruning.
class Adam
{
public:
    void reset(std::size_t n);
    void compact(const std::vector<std::uint8_t> &keep);
    // Appends zero moments for `count` new Gaussians.
    void grow(std::size_t count);
    void step(GaussianModel &params, const GaussianModel &grads, const LearningRates &lr, ThreadPool &pool);
    // Clears the moments of the opacity group (used after an opacity reset).
    void resetOpacityMoments();

private:
    GaussianModel m_m;
    GaussianModel m_v;
    int m_step = 0;
};

} // namespace u3d
