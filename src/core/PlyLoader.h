#pragma once

#include <QByteArray>
#include <QString>

#include <vector>

namespace u3d {

// Points to display: positions (xyz) and colours (rgb, 0..1), tightly packed.
struct PointCloud {
    std::vector<float> positions;
    std::vector<float> colors;
    bool isGaussianSplat = false; // loaded from a 3DGS .ply (splatfacto / gsplat)

    std::size_t size() const { return positions.size() / 3; }
    bool isEmpty() const { return positions.empty(); }
};

struct PlyLoadOptions {
    // Gaussian splats with sigmoid(opacity) below this are skipped (mostly floaters).
    float minOpacity = 0.05f;
};

// Reads ASCII and binary (little/big endian) PLY files: Nerfstudio point clouds
// (x y z red green blue) and Gaussian-splat exports (x y z f_dc_0..2 opacity ...).
// Splat colours come from the degree-0 spherical-harmonic coefficients.
class PlyLoader
{
public:
    static bool load(const QString &path, PointCloud &out, QString *error = nullptr,
                     const PlyLoadOptions &options = PlyLoadOptions());
    static bool loadFromMemory(const char *data, qint64 size, PointCloud &out,
                               QString *error = nullptr,
                               const PlyLoadOptions &options = PlyLoadOptions());
};

} // namespace u3d
