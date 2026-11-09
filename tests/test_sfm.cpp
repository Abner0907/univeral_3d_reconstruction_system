// Structure-from-motion: bundle adjustment and incremental SfM on synthetic data.

#include "Synthetic.h"
#include "Test.h"

#include "u3d/sfm/BundleAdjustment.h"
#include "u3d/sfm/IncrementalSfM.h"

#include <Eigen/Geometry>
#include <Eigen/LU>

#include <algorithm>
#include <map>
#include <random>

using namespace u3d;

namespace {

Eigen::Matrix3d rotOf(const Camera &c)
{
    Eigen::Matrix3d R;
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k)
            R(r, k) = c.R[std::size_t(r * 3 + k)];
    return R;
}

} // namespace

TEST(bundle_adjustment_converges)
{
    std::mt19937 rng(1);
    std::normal_distribution<double> noise(0.0, 1.0);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    const int W = 640, H = 480;
    const double trueFocalFactor = 0.9;

    BAProblem p;
    std::vector<Camera> truth;
    for (int k = 0; k < 8; ++k) {
        Camera c = u3dtest::orbitCamera(W, H, trueFocalFactor * W, 0.35 * k, 5.0, -0.5);
        truth.push_back(c);
        p.R.push_back(rotOf(c));
        p.t.push_back(Eigen::Vector3d(c.t[0], c.t[1], c.t[2]));
        p.sizeScale.push_back(W);
        p.cx.push_back(c.cx);
        p.cy.push_back(c.cy);
        p.fixedCamera.push_back(k == 0);
    }
    std::vector<Eigen::Vector3d> truePoints;
    for (int i = 0; i < 300; ++i) {
        const Eigen::Vector3d X(u(rng), u(rng), u(rng));
        truePoints.push_back(X);
        for (int k = 0; k < 8; ++k) {
            double uu, vv;
            if (truth[std::size_t(k)].project({X.x(), X.y(), X.z()}, uu, vv) && uu >= 0 && vv >= 0 && uu < W && vv < H)
                p.observations.push_back({k, i, uu + 0.5 * noise(rng), vv + 0.5 * noise(rng)});
        }
    }
    // Perturb everything except the fixed camera.
    for (int k = 1; k < 8; ++k) {
        const Eigen::Vector3d axis = Eigen::Vector3d(u(rng), u(rng), u(rng)).normalized();
        p.R[std::size_t(k)] = Eigen::AngleAxisd(0.02, axis).toRotationMatrix() * p.R[std::size_t(k)];
        p.t[std::size_t(k)] += 0.05 * Eigen::Vector3d(noise(rng), noise(rng), noise(rng));
    }
    for (const Eigen::Vector3d &X : truePoints)
        p.points.push_back(X + 0.05 * Eigen::Vector3d(noise(rng), noise(rng), noise(rng)));
    p.focalFactor = 1.0;
    p.refineFocal = true;

    const BASummary s = bundleAdjust(p);
    LOG("RMS %.2f px -> %.3f px in %d iterations, focal factor %.4f (true %.4f)", s.initialRms, s.finalRms,
        s.iterations, p.focalFactor, trueFocalFactor);
    CHECK(s.initialRms > 5.0);
    CHECK(s.finalRms < 0.75);
    CHECK_NEAR(p.focalFactor, trueFocalFactor, 0.02 * trueFocalFactor);
}

TEST(incremental_sfm_recovers_cameras)
{
    std::mt19937 rng(2);
    std::normal_distribution<double> noise(0.0, 0.5);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    const int W = 640, H = 480, numCams = 20;
    const double focal = 520.0;

    std::vector<Camera> truth;
    for (int k = 0; k < numCams; ++k)
        truth.push_back(u3dtest::orbitCamera(W, H, focal, 0.12 * k, 4.0, -0.6 + 0.05 * (k % 3)));
    std::vector<Eigen::Vector3d> points;
    for (int i = 0; i < 2000; ++i)
        points.push_back(Eigen::Vector3d(u(rng), 0.7 * u(rng), u(rng)));

    SfmInput in;
    std::vector<std::map<int, int>> kpOfPoint(static_cast<std::size_t>(numCams)); // point -> keypoint index
    for (int k = 0; k < numCams; ++k) {
        in.widths.push_back(W);
        in.heights.push_back(H);
        std::vector<cv::Point2f> pts;
        std::vector<std::array<std::uint8_t, 3>> cols;
        std::vector<int> order(points.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = int(i);
        std::shuffle(order.begin(), order.end(), rng); // keypoint order unrelated to point id
        for (int i : order) {
            double uu, vv;
            const Eigen::Vector3d &X = points[std::size_t(i)];
            if (!truth[std::size_t(k)].project({X.x(), X.y(), X.z()}, uu, vv) || uu < 0 || vv < 0 || uu >= W || vv >= H)
                continue;
            kpOfPoint[std::size_t(k)][i] = int(pts.size());
            pts.emplace_back(float(uu + noise(rng)), float(vv + noise(rng)));
            cols.push_back({128, 128, 128});
        }
        in.points.push_back(pts);
        in.colors.push_back(cols);
    }
    std::uniform_int_distribution<int> any(0, 400);
    for (int i = 0; i < numCams; ++i)
        for (int j = i + 1; j < numCams; ++j) {
            ImagePair pair;
            pair.i = i;
            pair.j = j;
            for (const auto &kv : kpOfPoint[std::size_t(i)]) {
                auto it = kpOfPoint[std::size_t(j)].find(kv.first);
                if (it != kpOfPoint[std::size_t(j)].end())
                    pair.matches.push_back({kv.second, it->second});
            }
            if (pair.matches.size() < 40)
                continue;
            // A few wrong matches, as left over after RANSAC verification.
            const std::size_t outliers = pair.matches.size() / 50;
            for (std::size_t o = 0; o < outliers; ++o)
                pair.matches.push_back({any(rng) % int(in.points[std::size_t(i)].size()),
                                        any(rng) % int(in.points[std::size_t(j)].size())});
            in.pairs.push_back(pair);
        }

    SfmResult result;
    std::string error;
    const bool ok = runIncrementalSfm(in, SfmOptions(), result, {}, nullptr, &error);
    if (!ok)
        LOG("SfM error: %s", error.c_str());
    CHECK(ok);
    LOG("%d / %d registered, %zu points, %.3f px RMS, focal factor %.4f (true %.4f)", result.numRegistered, numCams,
        result.pointsXyz.size() / 3, result.rmsError, result.focalFactor, focal / W);
    CHECK(result.numRegistered == numCams);
    CHECK(result.rmsError < 1.0);
    CHECK_NEAR(result.focalFactor, focal / W, 0.03 * focal / W);

    // Camera centres match the truth up to a similarity transform.
    Eigen::Matrix3Xd est(3, numCams), gt(3, numCams);
    for (int k = 0; k < numCams; ++k) {
        const auto e = result.cameras[std::size_t(k)].center();
        const auto g = truth[std::size_t(k)].center();
        est.col(k) << e[0], e[1], e[2];
        gt.col(k) << g[0], g[1], g[2];
    }
    const Eigen::Matrix4d T = Eigen::umeyama(est, gt, true);
    double maxErr = 0;
    for (int k = 0; k < numCams; ++k) {
        const Eigen::Vector3d mapped = T.topLeftCorner<3, 3>() * est.col(k) + T.topRightCorner<3, 1>();
        maxErr = std::max(maxErr, (mapped - gt.col(k)).norm());
    }
    LOG("max camera centre error after similarity alignment: %.4f (orbit radius 4)", maxErr);
    CHECK(maxErr < 0.05);
}
