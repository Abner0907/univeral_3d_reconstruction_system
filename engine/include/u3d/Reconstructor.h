#pragma once

#include "u3d/Camera.h"

#include <atomic>
#include <cstddef>
#include <string>

namespace u3d {

enum class Quality { Fast, Balanced, High };

struct QualitySettings {
    int iterations;
    int trainMaxSide;   // training resolution (longest side, pixels)
    int maxGaussians;
    int featureMaxSide; // resolution for SIFT
    int maxFeatures;

    static QualitySettings of(Quality quality);
};

struct ReconstructOptions {
    Quality quality = Quality::Balanced;
    int threads = 0;        // 0 = all cores
    bool forceSfm = false;  // ignore ARCore poses and estimate them from the images
    int maxImages = 300;
    int iterations = 0;     // > 0 overrides the quality preset's iteration count
};

struct ReconstructResult {
    std::string splatPath;
    std::size_t gaussians = 0;
    float psnr = 0;           // mean over training views
    int usedImages = 0;
    int totalImages = 0;
    bool posesFromArcore = false;
    double seconds = 0;
};

// Full on-device pipeline for a project folder (see Project.h):
//   1. load photos (EXIF orientation applied)
//   2. camera poses: ARCore poses if recorded, otherwise SIFT matching + incremental SfM
//   3. align the scene (gravity / average camera up = +Y, centred)
//   4. train 3D Gaussian Splatting on the CPU
//   5. write output/splat.ply, output/cameras.txt, output/sparse.ply
bool reconstructProject(const std::string &projectDir, const ReconstructOptions &options,
                        ReconstructResult &result, const ProgressCallback &progress = {},
                        const LogCallback &log = {}, const std::atomic_bool *cancel = nullptr,
                        std::string *error = nullptr);

} // namespace u3d
