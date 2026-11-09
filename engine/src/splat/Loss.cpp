#include "u3d/splat/Loss.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace u3d {

namespace {

constexpr int kRadius = 5; // 11x11 window
constexpr float kC1 = 0.01f * 0.01f;
constexpr float kC2 = 0.03f * 0.03f;

struct Window {
    float w[2 * kRadius + 1];
    Window()
    {
        float sum = 0;
        for (int k = -kRadius; k <= kRadius; ++k) {
            w[k + kRadius] = std::exp(-float(k * k) / (2.0f * 1.5f * 1.5f));
            sum += w[k + kRadius];
        }
        for (float &v : w)
            v /= sum;
    }
};
const Window kWindow;

// Scratch buffers reused between calls (training calls the loss once per iteration).
struct Scratch {
    std::vector<float> h[5];  // horizontally blurred x, y, xx, yy, xy (planar: channel * P + pixel)
    std::vector<float> abc[3]; // per-pixel dL/dmu, dL/dE[xx], dL/dE[xy]
    std::vector<float> h2[3];  // horizontally blurred abc
    std::vector<double> partial;
};
thread_local Scratch tScratch;

} // namespace

// The SSIM maps are separable Gaussian blurs (zero padding). Rows of all three channels are
// processed together so the whole loss takes four parallel passes:
//   1. L1 + horizontal blur of x, y, x^2, y^2, xy
//   2. vertical blur + SSIM + its per-pixel partial derivatives
//   3. horizontal blur of the partial derivatives (the blur is self-adjoint)
//   4. vertical blur + chain rule into dL/drender
LossValue photometricLoss(const float *render, const float *target, int W, int H, float lambda,
                          float *grad, ThreadPool &pool)
{
    const std::size_t P = std::size_t(W) * H;
    const float invCount = 1.0f / float(P * 3);
    const int rows = 3 * H; // (channel, row) pairs
    Scratch &s = tScratch;
    for (auto &v : s.h)
        v.resize(3 * P);
    for (auto &v : s.abc)
        v.resize(3 * P);
    for (auto &v : s.h2)
        v.resize(3 * P);
    s.partial.assign(std::size_t(pool.size()) * 2, 0.0);
    const float *wk = kWindow.w;

    // Pass 1.
    pool.parallelFor(rows, [&](int r0, int r1, int t) {
        double l1 = 0;
        for (int r = r0; r < r1; ++r) {
            const int c = r / H, y = r % H;
            const std::size_t base = std::size_t(c) * P + std::size_t(y) * W;
            for (int x = 0; x < W; ++x) {
                const std::size_t idx = (std::size_t(y) * W + x) * 3 + std::size_t(c);
                const float d = render[idx] - target[idx];
                l1 += std::fabs(d);
                grad[idx] = (1.0f - lambda) * invCount * (d > 0 ? 1.0f : (d < 0 ? -1.0f : 0.0f));
            }
            if (lambda <= 0.0f)
                continue;
            for (int x = 0; x < W; ++x) {
                float sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
                const int k0 = std::max(-kRadius, -x), k1 = std::min(kRadius, W - 1 - x);
                for (int k = k0; k <= k1; ++k) {
                    const std::size_t idx = (std::size_t(y) * W + std::size_t(x + k)) * 3 + std::size_t(c);
                    const float a = render[idx], b = target[idx], w = wk[k + kRadius];
                    sx += w * a;
                    sy += w * b;
                    sxx += w * a * a;
                    syy += w * b * b;
                    sxy += w * a * b;
                }
                s.h[0][base + x] = sx;
                s.h[1][base + x] = sy;
                s.h[2][base + x] = sxx;
                s.h[3][base + x] = syy;
                s.h[4][base + x] = sxy;
            }
        }
        s.partial[std::size_t(t) * 2] += l1;
    }, 4);

    LossValue result;
    double l1 = 0;
    for (int t = 0; t < pool.size(); ++t)
        l1 += s.partial[std::size_t(t) * 2];
    result.l1 = float(l1 * invCount);
    if (lambda <= 0.0f) {
        result.total = result.l1;
        return result;
    }

    // Pass 2.
    const float dLds = -lambda * invCount;
    pool.parallelFor(rows, [&](int r0, int r1, int t) {
        double ssim = 0;
        for (int r = r0; r < r1; ++r) {
            const int c = r / H, y = r % H;
            const std::size_t cbase = std::size_t(c) * P;
            const int k0 = std::max(-kRadius, -y), k1 = std::min(kRadius, H - 1 - y);
            for (int x = 0; x < W; ++x) {
                float m[5] = {0, 0, 0, 0, 0};
                for (int k = k0; k <= k1; ++k) {
                    const std::size_t i = cbase + std::size_t(y + k) * W + x;
                    const float w = wk[k + kRadius];
                    for (int j = 0; j < 5; ++j)
                        m[j] += w * s.h[j][i];
                }
                const float mx = m[0], my = m[1];
                const float sx = m[2] - mx * mx, sy = m[3] - my * my, sxy = m[4] - mx * my;
                const float a1 = 2 * mx * my + kC1, a2 = 2 * sxy + kC2;
                const float b1 = mx * mx + my * my + kC1, b2 = sx + sy + kC2;
                const float v = (a1 * a2) / (b1 * b2);
                ssim += v;
                const float dmu = (2 * my * a2 - 2 * my * a1) / (b1 * b2) - v * (2 * mx / b1 - 2 * mx / b2);
                const std::size_t o = cbase + std::size_t(y) * W + x;
                s.abc[0][o] = dLds * dmu;
                s.abc[1][o] = dLds * (-v / b2);
                s.abc[2][o] = dLds * (2 * a1 / (b1 * b2));
            }
        }
        s.partial[std::size_t(t) * 2 + 1] += ssim;
    }, 4);

    // Pass 3.
    pool.parallelFor(rows, [&](int r0, int r1, int) {
        for (int r = r0; r < r1; ++r) {
            const std::size_t base = std::size_t(r / H) * P + std::size_t(r % H) * W;
            for (int x = 0; x < W; ++x) {
                float acc[3] = {0, 0, 0};
                const int k0 = std::max(-kRadius, -x), k1 = std::min(kRadius, W - 1 - x);
                for (int k = k0; k <= k1; ++k) {
                    const float w = wk[k + kRadius];
                    for (int j = 0; j < 3; ++j)
                        acc[j] += w * s.abc[j][base + std::size_t(x + k)];
                }
                for (int j = 0; j < 3; ++j)
                    s.h2[j][base + x] = acc[j];
            }
        }
    }, 4);

    // Pass 4.
    pool.parallelFor(rows, [&](int r0, int r1, int) {
        for (int r = r0; r < r1; ++r) {
            const int c = r / H, y = r % H;
            const std::size_t cbase = std::size_t(c) * P;
            const int k0 = std::max(-kRadius, -y), k1 = std::min(kRadius, H - 1 - y);
            for (int x = 0; x < W; ++x) {
                float g[3] = {0, 0, 0};
                for (int k = k0; k <= k1; ++k) {
                    const std::size_t i = cbase + std::size_t(y + k) * W + x;
                    const float w = wk[k + kRadius];
                    for (int j = 0; j < 3; ++j)
                        g[j] += w * s.h2[j][i];
                }
                const std::size_t idx = (std::size_t(y) * W + x) * 3 + std::size_t(c);
                grad[idx] += g[0] + 2.0f * render[idx] * g[1] + target[idx] * g[2];
            }
        }
    }, 4);

    double ssim = 0;
    for (int t = 0; t < pool.size(); ++t)
        ssim += s.partial[std::size_t(t) * 2 + 1];
    result.ssim = float(ssim * invCount);
    result.total = (1.0f - lambda) * result.l1 + lambda * (1.0f - result.ssim);
    return result;
}

float psnr(const float *a, const float *b, std::size_t count)
{
    double mse = 0;
    for (std::size_t k = 0; k < count; ++k) {
        const double d = double(a[k]) - double(b[k]);
        mse += d * d;
    }
    mse /= double(std::max<std::size_t>(count, 1));
    return mse <= 1e-12 ? 99.0f : float(-10.0 * std::log10(mse));
}

} // namespace u3d
