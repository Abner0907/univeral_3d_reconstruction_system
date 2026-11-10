#include "u3d/splat/Crop.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace u3d {

namespace {

// Per-axis quantiles of the centres of Gaussians at least `minOpacity` opaque.
CropBox quantileBox(const GaussianModel &m, float lo, float hi, float margin, float minOpacity)
{
    CropBox box;
    std::vector<float> axis;
    axis.reserve(m.size());
    for (int k = 0; k < 3; ++k) {
        axis.clear();
        for (std::size_t i = 0; i < m.size(); ++i)
            if (sigmoid(m.opacities[i]) >= minOpacity)
                axis.push_back(m.means[i * 3 + std::size_t(k)]);
        if (axis.empty()) // everything faint: fall back to all centres
            for (std::size_t i = 0; i < m.size(); ++i)
                axis.push_back(m.means[i * 3 + std::size_t(k)]);
        if (axis.empty()) {
            box.min[k] = -1.0f;
            box.max[k] = 1.0f;
            continue;
        }
        auto at = [&](float q) {
            const std::size_t idx = std::min(axis.size() - 1, std::size_t(q * float(axis.size() - 1) + 0.5f));
            std::nth_element(axis.begin(), axis.begin() + std::ptrdiff_t(idx), axis.end());
            return axis[idx];
        };
        float a = at(lo), b = at(hi);
        const float pad = std::max((b - a) * margin, 1e-4f);
        box.min[k] = a - pad;
        box.max[k] = b + pad;
    }
    return box;
}

} // namespace

GaussianModel cropModel(const GaussianModel &model, const CropBox &box, float minOpacity)
{
    std::vector<std::uint8_t> keep(model.size(), 0);
    for (std::size_t i = 0; i < model.size(); ++i)
        keep[i] = box.contains(&model.means[i * 3]) && sigmoid(model.opacities[i]) >= minOpacity;
    GaussianModel out = model;
    out.compact(keep);
    return out;
}

CropBox suggestCropBox(const GaussianModel &model, float lowFraction, float highFraction, float margin)
{
    return quantileBox(model, lowFraction, highFraction, margin, 0.3f);
}

CropBox modelBounds(const GaussianModel &model)
{
    return quantileBox(model, 0.005f, 0.995f, 0.1f, 0.0f);
}

} // namespace u3d
