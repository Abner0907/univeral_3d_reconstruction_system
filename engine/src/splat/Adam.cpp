#include "u3d/splat/Adam.h"

#include <algorithm>
#include <cmath>

namespace u3d {

namespace {
constexpr float kBeta1 = 0.9f;
constexpr float kBeta2 = 0.999f;
constexpr float kEps = 1e-15f;

void adamArray(std::vector<float> &p, const std::vector<float> &g, std::vector<float> &m,
               std::vector<float> &v, float lr, float bc1, float bc2, ThreadPool &pool)
{
    const float stepSize = lr / bc1;
    const float invSqrtBc2 = 1.0f / std::sqrt(bc2);
    pool.parallelFor(int(p.size()), [&](int b, int e, int) {
        for (int k = b; k < e; ++k) {
            const float gk = g[std::size_t(k)];
            float &mk = m[std::size_t(k)];
            float &vk = v[std::size_t(k)];
            mk = kBeta1 * mk + (1.0f - kBeta1) * gk;
            vk = kBeta2 * vk + (1.0f - kBeta2) * gk * gk;
            p[std::size_t(k)] -= stepSize * mk / (std::sqrt(vk) * invSqrtBc2 + kEps);
        }
    }, 8192);
}
} // namespace

void Adam::reset(std::size_t n)
{
    m_m.resize(n);
    m_v.resize(n);
    m_m.setZero();
    m_v.setZero();
    m_step = 0;
}

void Adam::compact(const std::vector<std::uint8_t> &keep)
{
    m_m.compact(keep);
    m_v.compact(keep);
}

void Adam::grow(std::size_t count)
{
    const std::size_t n = m_m.size();
    m_m.resize(n + count);
    m_v.resize(n + count);
    auto zeroTail = [n](GaussianModel &g) {
        std::fill(g.means.begin() + std::ptrdiff_t(n * 3), g.means.end(), 0.0f);
        std::fill(g.scales.begin() + std::ptrdiff_t(n * 3), g.scales.end(), 0.0f);
        std::fill(g.quats.begin() + std::ptrdiff_t(n * 4), g.quats.end(), 0.0f);
        std::fill(g.opacities.begin() + std::ptrdiff_t(n), g.opacities.end(), 0.0f);
        std::fill(g.colors.begin() + std::ptrdiff_t(n * 3), g.colors.end(), 0.0f);
    };
    zeroTail(m_m);
    zeroTail(m_v);
}

void Adam::resetOpacityMoments()
{
    std::fill(m_m.opacities.begin(), m_m.opacities.end(), 0.0f);
    std::fill(m_v.opacities.begin(), m_v.opacities.end(), 0.0f);
}

void Adam::step(GaussianModel &p, const GaussianModel &g, const LearningRates &lr, ThreadPool &pool)
{
    ++m_step;
    const float bc1 = 1.0f - std::pow(kBeta1, float(m_step));
    const float bc2 = 1.0f - std::pow(kBeta2, float(m_step));
    adamArray(p.means, g.means, m_m.means, m_v.means, lr.means, bc1, bc2, pool);
    adamArray(p.scales, g.scales, m_m.scales, m_v.scales, lr.scales, bc1, bc2, pool);
    adamArray(p.quats, g.quats, m_m.quats, m_v.quats, lr.quats, bc1, bc2, pool);
    adamArray(p.opacities, g.opacities, m_m.opacities, m_v.opacities, lr.opacities, bc1, bc2, pool);
    adamArray(p.colors, g.colors, m_m.colors, m_v.colors, lr.colors, bc1, bc2, pool);
}

} // namespace u3d
