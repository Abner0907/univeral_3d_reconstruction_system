#pragma once

#include "u3d/ThreadPool.h"

#include <cstddef>

namespace u3d {

struct LossValue {
    float total = 0;
    float l1 = 0;
    float ssim = 0; // mean SSIM (1 = identical)
};

// (1 - lambda) * L1 + lambda * (1 - SSIM), the photometric loss of 3DGS / splatfacto
// (11x11 Gaussian window, sigma 1.5, zero padding). Images are height*width*3, interleaved.
// Writes dLoss/dRender into `grad`.
LossValue photometricLoss(const float *render, const float *target, int width, int height,
                          float ssimLambda, float *grad, ThreadPool &pool);

float psnr(const float *a, const float *b, std::size_t count);

} // namespace u3d
