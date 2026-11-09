#include "u3d/sfm/Features.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/flann.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <set>

namespace u3d {

ImageFeatures extractFeatures(const cv::Mat &bgr, int maxFeatures, int maxSide)
{
    ImageFeatures f;
    f.width = bgr.cols;
    f.height = bgr.rows;
    const double scale = std::min(1.0, double(maxSide) / double(std::max(bgr.cols, bgr.rows)));
    cv::Mat small;
    if (scale < 1.0)
        cv::resize(bgr, small, cv::Size(), scale, scale, cv::INTER_AREA);
    else
        small = bgr;
    cv::Mat gray;
    cv::cvtColor(small, gray, cv::COLOR_BGR2GRAY);

    cv::Ptr<cv::SIFT> sift = cv::SIFT::create(maxFeatures);
    std::vector<cv::KeyPoint> keypoints;
    sift->detectAndCompute(gray, cv::noArray(), keypoints, f.descriptors);

    // RootSIFT (L1-normalise, square root) matches noticeably better than plain SIFT.
    for (int r = 0; r < f.descriptors.rows; ++r) {
        float *d = f.descriptors.ptr<float>(r);
        float sum = 1e-12f;
        for (int c = 0; c < f.descriptors.cols; ++c)
            sum += std::abs(d[c]);
        for (int c = 0; c < f.descriptors.cols; ++c)
            d[c] = std::sqrt(std::abs(d[c]) / sum);
    }

    f.points.reserve(keypoints.size());
    f.colors.reserve(keypoints.size());
    for (const cv::KeyPoint &kp : keypoints) {
        // Continuous coordinates with pixel centres at integers survive rescaling this way.
        const cv::Point2f p((kp.pt.x + 0.5f) / float(scale) - 0.5f, (kp.pt.y + 0.5f) / float(scale) - 0.5f);
        f.points.push_back(p);
        const int x = std::clamp(int(std::lround(p.x)), 0, bgr.cols - 1);
        const int y = std::clamp(int(std::lround(p.y)), 0, bgr.rows - 1);
        const cv::Vec3b c = bgr.at<cv::Vec3b>(y, x);
        f.colors.push_back({c[2], c[1], c[0]});
    }
    return f;
}

std::vector<std::pair<int, int>> candidatePairs(int n, int window, int exhaustiveBelow)
{
    std::set<std::pair<int, int>> pairs;
    if (n <= exhaustiveBelow) {
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j)
                pairs.insert({i, j});
    } else {
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j <= std::min(n - 1, i + window); ++j)
                pairs.insert({i, j});
        // A walk around an object usually ends where it started.
        for (int i = 0; i < window; ++i)
            for (int j = std::max(i + 1, n - window); j < n; ++j)
                pairs.insert({i, j});
        // Sparse long-range pairs help when the capture order is not a single sweep.
        const int stride = std::max(window, n / 8);
        for (int i = 0; i < n; i += stride)
            for (int j = i + 2 * window; j < n; j += stride)
                pairs.insert({i, j});
    }
    return {pairs.begin(), pairs.end()};
}

bool matchImages(const ImageFeatures &a, const ImageFeatures &b, int minInliers, ImagePair &out)
{
    out.matches.clear();
    if (a.descriptors.rows < minInliers || b.descriptors.rows < minInliers)
        return false;

    cv::FlannBasedMatcher matcher(cv::makePtr<cv::flann::KDTreeIndexParams>(4),
                                  cv::makePtr<cv::flann::SearchParams>(64));
    std::vector<std::vector<cv::DMatch>> knn;
    matcher.knnMatch(a.descriptors, b.descriptors, knn, 2);

    std::vector<std::pair<int, int>> candidates;
    std::vector<int> usedB(std::size_t(b.descriptors.rows), -1);
    for (const auto &m : knn) {
        if (m.size() < 2 || m[0].distance > 0.8f * m[1].distance)
            continue;
        // One-to-one: keep only the first query that claims a train keypoint.
        if (usedB[std::size_t(m[0].trainIdx)] >= 0)
            continue;
        usedB[std::size_t(m[0].trainIdx)] = m[0].queryIdx;
        candidates.push_back({m[0].queryIdx, m[0].trainIdx});
    }
    if (int(candidates.size()) < minInliers)
        return false;

    std::vector<cv::Point2f> pa, pb;
    for (const auto &c : candidates) {
        pa.push_back(a.points[std::size_t(c.first)]);
        pb.push_back(b.points[std::size_t(c.second)]);
    }
    const double threshold = 1.0 + 1.5 * double(std::max(a.width, a.height)) / 1600.0;
    std::vector<unsigned char> mask;
    const cv::Mat F = cv::findFundamentalMat(pa, pb, cv::FM_RANSAC, threshold, 0.999, mask);
    if (F.empty())
        return false;
    for (std::size_t k = 0; k < candidates.size(); ++k)
        if (mask[k])
            out.matches.push_back(candidates[k]);
    return int(out.matches.size()) >= minInliers;
}

} // namespace u3d
