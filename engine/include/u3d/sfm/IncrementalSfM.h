#pragma once

#include "u3d/Camera.h"
#include "u3d/sfm/Features.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace u3d {

struct SfmOptions {
    double initialFocalFactor = 1.2;   // focal = factor * max(width, height) until refined
    bool refineFocal = true;           // one shared focal factor (same phone camera)
    double maxReprojError = 4.0;       // pixels
    double minTriangulationAngle = 1.5; // degrees
    int minPnPInliers = 15;
    int baInterval = 4;                // global bundle adjustment after this many new images
};

// Keypoints and verified matches of every image (see Features.h).
struct SfmInput {
    std::vector<int> widths;
    std::vector<int> heights;
    std::vector<std::vector<cv::Point2f>> points;
    std::vector<std::vector<std::array<std::uint8_t, 3>>> colors;
    std::vector<ImagePair> pairs;
};

struct SfmResult {
    std::vector<Camera> cameras;  // one per input image (valid where registered)
    std::vector<bool> registered;
    int numRegistered = 0;
    std::vector<float> pointsXyz; // sparse point cloud
    std::vector<float> pointsRgb; // 0..1
    double focalFactor = 0;
    double rmsError = 0;          // final reprojection RMS, pixels
};

using SfmProgress = std::function<void(double fraction, const std::string &message)>;

// Incremental structure-from-motion (the approach of COLMAP): feature tracks, best initial
// pair (essential matrix), then repeated PnP registration, triangulation and global bundle
// adjustment with outlier filtering.
bool runIncrementalSfm(const SfmInput &input, const SfmOptions &options, SfmResult &result,
                       const SfmProgress &progress = {}, const std::atomic_bool *cancel = nullptr,
                       std::string *error = nullptr);

} // namespace u3d
