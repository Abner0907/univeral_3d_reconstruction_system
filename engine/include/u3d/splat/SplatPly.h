#pragma once

#include "u3d/splat/GaussianModel.h"

#include <string>
#include <vector>

namespace u3d {

// Writes the standard 3D Gaussian Splatting .ply layout (binary little endian:
// x y z nx ny nz f_dc_0..2 opacity scale_0..2 rot_0..3), readable by common splat viewers.
bool writeSplatPly(const std::string &path, const GaussianModel &model, std::string *error = nullptr);

// Reads a 3DGS .ply. A plain point cloud (x y z [red green blue]) is accepted too and turned
// into small opaque Gaussians. ASCII and binary (either endianness) files are supported.
// `isSplat` (optional) reports which kind of file it was.
bool readSplatPly(const std::string &path, GaussianModel &model, std::string *error = nullptr,
                  bool *isSplat = nullptr);

// Coloured point cloud (rgb 0..1) as x y z red green blue (uchar).
bool writePointsPly(const std::string &path, const std::vector<float> &xyz,
                    const std::vector<float> &rgb, std::string *error = nullptr);
bool readPointsPly(const std::string &path, std::vector<float> &xyz, std::vector<float> &rgb,
                   std::string *error = nullptr);

} // namespace u3d
