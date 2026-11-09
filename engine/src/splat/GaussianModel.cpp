#include "u3d/splat/GaussianModel.h"

#include <algorithm>
#include <limits>
#include <unordered_map>

namespace u3d {

void GaussianModel::resize(std::size_t n)
{
    means.resize(n * 3);
    scales.resize(n * 3);
    quats.resize(n * 4);
    opacities.resize(n);
    colors.resize(n * 3);
}

void GaussianModel::setZero()
{
    std::fill(means.begin(), means.end(), 0.0f);
    std::fill(scales.begin(), scales.end(), 0.0f);
    std::fill(quats.begin(), quats.end(), 0.0f);
    std::fill(opacities.begin(), opacities.end(), 0.0f);
    std::fill(colors.begin(), colors.end(), 0.0f);
}

namespace {
template <int Dim>
void compactArray(std::vector<float> &v, const std::vector<std::uint8_t> &keep)
{
    std::size_t out = 0;
    for (std::size_t i = 0; i < keep.size(); ++i) {
        if (!keep[i])
            continue;
        if (out != i)
            std::copy_n(&v[i * Dim], Dim, &v[out * Dim]);
        ++out;
    }
    v.resize(out * Dim);
}

template <int Dim>
void appendArray(std::vector<float> &dst, const std::vector<float> &src, std::size_t index)
{
    dst.insert(dst.end(), src.begin() + std::ptrdiff_t(index * Dim),
               src.begin() + std::ptrdiff_t(index * Dim + Dim));
}
} // namespace

void GaussianModel::compact(const std::vector<std::uint8_t> &keep)
{
    compactArray<3>(means, keep);
    compactArray<3>(scales, keep);
    compactArray<4>(quats, keep);
    compactArray<1>(opacities, keep);
    compactArray<3>(colors, keep);
}

void GaussianModel::appendFrom(const GaussianModel &other, std::size_t index)
{
    appendArray<3>(means, other.means, index);
    appendArray<3>(scales, other.scales, index);
    appendArray<4>(quats, other.quats, index);
    appendArray<1>(opacities, other.opacities, index);
    appendArray<3>(colors, other.colors, index);
}

GaussianModel initGaussiansFromPoints(const std::vector<float> &xyz, const std::vector<float> &rgb)
{
    const std::size_t n = xyz.size() / 3;
    GaussianModel m;
    m.resize(n);
    if (n == 0)
        return m;

    // Uniform grid for the k-nearest-neighbour search.
    float lo[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max()};
    float hi[3] = {-lo[0], -lo[1], -lo[2]};
    for (std::size_t i = 0; i < n; ++i)
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], xyz[i * 3 + k]);
            hi[k] = std::max(hi[k], xyz[i * 3 + k]);
        }
    const float extent = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 1e-6f});
    const float cell = std::max(extent / std::cbrt(float(n)) * 2.0f, 1e-6f);
    auto key = [&](int x, int y, int z) {
        return (std::int64_t(x) * 73856093) ^ (std::int64_t(y) * 19349663) ^ (std::int64_t(z) * 83492791);
    };
    auto cellOf = [&](std::size_t i, int c[3]) {
        for (int k = 0; k < 3; ++k)
            c[k] = int(std::floor((xyz[i * 3 + k] - lo[k]) / cell));
    };
    std::unordered_map<std::int64_t, std::vector<std::uint32_t>> grid;
    grid.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        int c[3];
        cellOf(i, c);
        grid[key(c[0], c[1], c[2])].push_back(std::uint32_t(i));
    }

    for (std::size_t i = 0; i < n; ++i) {
        float best[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                         std::numeric_limits<float>::max()};
        int c[3];
        cellOf(i, c);
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz) {
                    auto it = grid.find(key(c[0] + dx, c[1] + dy, c[2] + dz));
                    if (it == grid.end())
                        continue;
                    for (std::uint32_t j : it->second) {
                        if (j == i)
                            continue;
                        const float ex = xyz[j * 3] - xyz[i * 3], ey = xyz[j * 3 + 1] - xyz[i * 3 + 1],
                                    ez = xyz[j * 3 + 2] - xyz[i * 3 + 2];
                        float d2 = ex * ex + ey * ey + ez * ez;
                        for (int k = 0; k < 3; ++k)
                            if (d2 < best[k])
                                std::swap(d2, best[k]);
                    }
                }
        float sum = 0;
        int count = 0;
        for (float b : best)
            if (b < std::numeric_limits<float>::max()) {
                sum += b;
                ++count;
            }
        const float meanD2 = count ? sum / float(count) : cell * cell;
        const float logScale = std::log(std::sqrt(std::max(meanD2, 1e-14f)));
        for (int k = 0; k < 3; ++k) {
            m.means[i * 3 + k] = xyz[i * 3 + k];
            m.scales[i * 3 + k] = logScale;
            const float col = rgb.size() >= (i + 1) * 3 ? rgb[i * 3 + k] : 0.5f;
            m.colors[i * 3 + k] = (col - 0.5f) / kShC0;
        }
        m.quats[i * 4 + 0] = 1.0f;
        m.quats[i * 4 + 1] = m.quats[i * 4 + 2] = m.quats[i * 4 + 3] = 0.0f;
        m.opacities[i] = logit(0.1f);
    }
    return m;
}

} // namespace u3d
