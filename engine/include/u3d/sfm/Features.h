#pragma once

#include <opencv2/core.hpp>

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace u3d {

struct ImageFeatures {
    int width = 0;  // full-resolution image size; keypoints are in these pixel coordinates
    int height = 0;
    std::vector<cv::Point2f> points;
    cv::Mat descriptors; // SIFT, CV_32F
    std::vector<std::array<std::uint8_t, 3>> colors; // rgb at each keypoint
};

// SIFT keypoints of a BGR image, detected on a copy downscaled to at most maxSide pixels.
ImageFeatures extractFeatures(const cv::Mat &bgr, int maxFeatures, int maxSide);

struct ImagePair {
    int i = 0;
    int j = 0;
    std::vector<std::pair<int, int>> matches; // keypoint index in i, in j (geometrically verified)
};

// Image pairs worth matching for n images in capture order: all pairs for small sets,
// otherwise a sliding window plus loop-closure pairs between the start and end of the capture.
std::vector<std::pair<int, int>> candidatePairs(int n, int window, int exhaustiveBelow);

// Ratio-test matching followed by fundamental-matrix RANSAC. Returns false if fewer than
// minInliers survive.
bool matchImages(const ImageFeatures &a, const ImageFeatures &b, int minInliers, ImagePair &out);

} // namespace u3d
