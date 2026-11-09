#pragma once

// Synthetic scenes and cameras for the SfM / pipeline tests.

#include "u3d/Camera.h"
#include "u3d/splat/GaussianModel.h"

#include <cmath>
#include <random>

namespace u3dtest {

// Camera on a circle around the origin looking at it (OpenCV convention, world y down).
inline u3d::Camera orbitCamera(int w, int h, double f, double angle, double radius, double height)
{
    u3d::Camera c;
    c.width = w;
    c.height = h;
    c.fx = c.fy = f;
    c.cx = 0.5 * (w - 1);
    c.cy = 0.5 * (h - 1);
    const double C[3] = {radius * std::sin(angle), height, -radius * std::cos(angle)};
    double z[3] = {-C[0], -C[1], -C[2]};
    const double zn = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    for (double &v : z) v /= zn;
    // Upright camera: image y = world down (+y), x = y x z points right.
    double x[3] = {1 * z[2] - 0 * z[1], 0 * z[0] - 0 * z[2], 0 * z[1] - 1 * z[0]};
    const double xn = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    for (double &v : x) v /= xn;
    const double y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    for (int k = 0; k < 3; ++k) {
        c.R[std::size_t(k)] = x[k];
        c.R[std::size_t(3 + k)] = y[k];
        c.R[std::size_t(6 + k)] = z[k];
    }
    for (int r = 0; r < 3; ++r)
        c.t[std::size_t(r)] = -(c.R[std::size_t(r * 3)] * C[0] + c.R[std::size_t(r * 3 + 1)] * C[1] + c.R[std::size_t(r * 3 + 2)] * C[2]);
    return c;
}

// Randomly coloured Gaussian blobs on the surface of a box: rich texture for SIFT.
inline u3d::GaussianModel texturedBox(std::size_t n, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f), u01(0.0f, 1.0f);
    std::uniform_int_distribution<int> face(0, 5);
    u3d::GaussianModel m;
    m.resize(n);
    const float half[3] = {0.8f, 0.6f, 0.7f};
    for (std::size_t i = 0; i < n; ++i) {
        const int f = face(rng);
        const int axis = f / 2;
        for (int k = 0; k < 3; ++k)
            m.means[i * 3 + k] = u(rng) * half[k];
        m.means[i * 3 + std::size_t(axis)] = (f % 2 ? 1.0f : -1.0f) * half[axis];
        // Strong light/dark contrast like real surface texture (SIFT works on intensity).
        const float brightness = u01(rng) < 0.5f ? 0.15f : 1.0f;
        for (int k = 0; k < 3; ++k) {
            m.scales[i * 3 + k] = std::log(0.015f + 0.04f * u01(rng));
            m.colors[i * 3 + k] = (brightness * (0.4f + 0.6f * u01(rng)) - 0.5f) / u3d::kShC0;
        }
        m.quats[i * 4] = 1.0f;
        m.quats[i * 4 + 1] = m.quats[i * 4 + 2] = m.quats[i * 4 + 3] = 0.0f;
        m.opacities[i] = u3d::logit(0.9f);
    }
    return m;
}

} // namespace u3dtest
