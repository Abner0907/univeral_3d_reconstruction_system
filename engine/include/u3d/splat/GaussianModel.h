#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace u3d {

constexpr float kShC0 = 0.28209479177387814f; // degree-0 spherical harmonic

inline float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }
inline float logit(float p) { return std::log(p / (1.0f - p)); }

// 3D Gaussian Splatting parameters in the same parameterisation as splatfacto / the original
// 3DGS (so exported .ply files open in standard splat viewers):
//   means      xyz
//   scales     log of the per-axis standard deviation
//   quats      rotation quaternion (w, x, y, z), not necessarily normalised
//   opacities  logit of opacity
//   colors     SH degree-0 coefficients (rgb = 0.5 + C0 * coeff)
// The same struct holds gradients and Adam moments.
struct GaussianModel {
    std::vector<float> means;
    std::vector<float> scales;
    std::vector<float> quats;
    std::vector<float> opacities;
    std::vector<float> colors;

    std::size_t size() const { return opacities.size(); }
    void resize(std::size_t n);
    void setZero();
    // Keeps the entries with keep[i] != 0, preserving order.
    void compact(const std::vector<std::uint8_t> &keep);
    // Appends entry `index` of `other`.
    void appendFrom(const GaussianModel &other, std::size_t index);
};

// Initial Gaussians from a coloured point cloud (rgb in 0..1): isotropic, scale from the mean
// distance to the 3 nearest neighbours, opacity 0.1 - as in the 3DGS paper.
GaussianModel initGaussiansFromPoints(const std::vector<float> &xyz, const std::vector<float> &rgb);

} // namespace u3d
