#pragma once

#include "u3d/splat/GaussianModel.h"

namespace u3d {

// Axis-aligned box in model coordinates (the reconstruction is aligned with +Y up, centred on
// the scanned object).
struct CropBox {
    float min[3] = {-1, -1, -1};
    float max[3] = {1, 1, 1};

    bool contains(const float *p) const
    {
        return p[0] >= min[0] && p[0] <= max[0] && p[1] >= min[1] && p[1] <= max[1] && p[2] >= min[2]
               && p[2] <= max[2];
    }
};

// Keeps the Gaussians whose centre lies inside `box` and whose opacity is at least
// `minOpacity` (0..1). Everything else (background, floaters) is removed.
GaussianModel cropModel(const GaussianModel &model, const CropBox &box, float minOpacity = 0.0f);

// A box around the main object: per axis, the [lowFraction, highFraction] quantiles of the
// Gaussian centres (weighted towards opaque Gaussians, ignoring nearly transparent ones),
// enlarged by `margin` of the box size on every side. Scattered background splats and
// floaters fall outside it.
CropBox suggestCropBox(const GaussianModel &model, float lowFraction = 0.05f, float highFraction = 0.95f,
                       float margin = 0.1f);

// Bounds of the bulk of the model (0.5 % / 99.5 % quantiles plus 10 % margin) - the range the
// crop sliders cover, so a few far-away floaters do not make them unusable.
CropBox modelBounds(const GaussianModel &model);

} // namespace u3d
