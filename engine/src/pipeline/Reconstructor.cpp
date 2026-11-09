#include "u3d/Reconstructor.h"

#include "u3d/Project.h"
#include "u3d/ThreadPool.h"
#include "u3d/sfm/Features.h"
#include "u3d/sfm/IncrementalSfM.h"
#include "u3d/splat/SplatPly.h"
#include "u3d/splat/Trainer.h"

#include <Eigen/Geometry>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <random>

namespace fs = std::filesystem;

namespace u3d {

QualitySettings QualitySettings::of(Quality q)
{
    switch (q) {
    case Quality::Fast: return {2000, 320, 80000, 1200, 3000};
    case Quality::High: return {7000, 640, 300000, 2000, 6000};
    case Quality::Balanced: break;
    }
    return {4000, 480, 150000, 1600, 4000};
}

namespace {

bool fail(std::string *error, const std::string &message)
{
    if (error)
        *error = message;
    return false;
}

std::string format(const char *fmt, double a = 0, double b = 0, double c = 0, double d = 0)
{
    char buf[256];
    std::snprintf(buf, sizeof buf, fmt, a, b, c, d);
    return buf;
}

Eigen::Matrix3d toEigen(const Camera &c)
{
    Eigen::Matrix3d R;
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k)
            R(r, k) = c.R[std::size_t(r * 3 + k)];
    return R;
}

// World change X' = A (X - c): cameras R' = R A^T, t' = t + R c.
void transformScene(std::vector<Camera> &cameras, std::vector<float> &xyz, const Eigen::Matrix3d &A,
                    const Eigen::Vector3d &c)
{
    for (Camera &cam : cameras) {
        const Eigen::Matrix3d R = toEigen(cam);
        const Eigen::Vector3d t(cam.t[0], cam.t[1], cam.t[2]);
        const Eigen::Matrix3d R2 = R * A.transpose();
        const Eigen::Vector3d t2 = t + R * c;
        for (int r = 0; r < 3; ++r) {
            for (int k = 0; k < 3; ++k)
                cam.R[std::size_t(r * 3 + k)] = R2(r, k);
            cam.t[std::size_t(r)] = t2(r);
        }
    }
    for (std::size_t i = 0; i + 2 < xyz.size(); i += 3) {
        const Eigen::Vector3d X = A * (Eigen::Vector3d(xyz[i], xyz[i + 1], xyz[i + 2]) - c);
        xyz[i] = float(X.x());
        xyz[i + 1] = float(X.y());
        xyz[i + 2] = float(X.z());
    }
}

} // namespace

bool reconstructProject(const std::string &projectDir, const ReconstructOptions &options,
                        ReconstructResult &result, const ProgressCallback &progress, const LogCallback &log,
                        const std::atomic_bool *cancel, std::string *error)
{
    const auto start = std::chrono::steady_clock::now();
    auto report = [&](const char *stage, double fraction, const std::string &message) {
        if (progress)
            progress({stage, std::min(1.0, std::max(0.0, fraction)), message});
    };
    auto say = [&](const std::string &line) {
        if (log)
            log(line);
    };
    auto cancelled = [&] { return cancel && cancel->load(); };

    result = ReconstructResult();
    ProjectData project;
    if (!loadProject(projectDir, project, error))
        return false;
    if (project.images.size() < 3)
        return fail(error, "At least 3 photos are needed (30-200 recommended).");

    // Even subsampling of very large captures.
    std::vector<std::size_t> pick;
    const std::size_t total = project.images.size();
    const std::size_t useCount = std::min<std::size_t>(total, std::size_t(std::max(3, options.maxImages)));
    for (std::size_t k = 0; k < useCount; ++k)
        pick.push_back(k * total / useCount);
    result.totalImages = int(total);

    QualitySettings q = QualitySettings::of(options.quality);
    if (options.iterations > 0)
        q.iterations = options.iterations;
    const bool usePoses = project.hasPoses && !options.forceSfm;
    result.posesFromArcore = usePoses;
    ThreadPool pool(options.threads);
    say("Project: " + projectDir);
    say(format("%.0f photos (%.0f used), %.0f threads, ", double(total), double(pick.size()), double(pool.size()))
        + (usePoses ? "camera poses from ARCore" : "camera poses from structure-from-motion"));

    const double trainStart = usePoses ? 0.05 : 0.30;

    // ------------------------------------------------------------------ 1. images (+ features)
    const std::size_t n = pick.size();
    std::vector<TrainImage> train(n);
    std::vector<int> fullW(n), fullH(n);
    std::vector<ImageFeatures> features(usePoses ? 0 : n);
    std::vector<std::string> names(n);
    std::vector<char> loaded(n, 0);
    std::atomic<int> done{0};
    std::mutex progressMutex;
    pool.parallelFor(int(n), [&](int b, int e, int) {
        for (int k = b; k < e; ++k) {
            if (cancelled())
                return;
            const std::string name = project.images[pick[std::size_t(k)]];
            names[std::size_t(k)] = name;
            const cv::Mat bgr = cv::imread(joinPath(joinPath(projectDir, "images"), name), cv::IMREAD_COLOR);
            if (bgr.empty())
                continue;
            fullW[std::size_t(k)] = bgr.cols;
            fullH[std::size_t(k)] = bgr.rows;
            const double s = std::min(1.0, double(q.trainMaxSide) / double(std::max(bgr.cols, bgr.rows)));
            cv::Mat small, rgb;
            cv::resize(bgr, small, cv::Size(std::max(1, int(std::lround(bgr.cols * s))), std::max(1, int(std::lround(bgr.rows * s)))),
                       0, 0, cv::INTER_AREA);
            cv::cvtColor(small, rgb, cv::COLOR_BGR2RGB);
            TrainImage &ti = train[std::size_t(k)];
            ti.rgb.assign(rgb.data, rgb.data + rgb.total() * 3);
            ti.camera.width = rgb.cols;
            ti.camera.height = rgb.rows;
            if (!usePoses)
                features[std::size_t(k)] = extractFeatures(bgr, q.maxFeatures, q.featureMaxSide);
            loaded[std::size_t(k)] = 1;
            std::lock_guard<std::mutex> lock(progressMutex);
            const int d = ++done;
            report("images", (usePoses ? 0.05 : 0.12) * d / double(n),
                   format(usePoses ? "Loading photos %.0f / %.0f" : "Finding features %.0f / %.0f", d, double(n)));
        }
    }, 1);
    if (cancelled())
        return fail(error, "Cancelled");

    // ------------------------------------------------------------------ 2. camera poses
    std::vector<Camera> cameras(n);
    std::vector<bool> valid(n, false);
    std::vector<float> xyz, rgb;
    if (usePoses) {
        for (std::size_t k = 0; k < n; ++k) {
            cameras[k] = project.poses[pick[k]];
            valid[k] = loaded[k] != 0;
            // Intrinsics refer to the stored photo; adapt if the decoded size differs.
            if (valid[k] && cameras[k].width != fullW[k] && cameras[k].width > 0) {
                const double s = double(fullW[k]) / cameras[k].width;
                cameras[k] = cameras[k].scaled(s);
                cameras[k].width = fullW[k];
                cameras[k].height = fullH[k];
            }
        }
        xyz = project.pointsXyz;
        rgb = project.pointsRgb;
        say(format("ARCore: %.0f sparse points", double(xyz.size() / 3)));
    } else {
        SfmInput in;
        std::vector<int> sfmIndex; // SfM image index -> k
        for (std::size_t k = 0; k < n; ++k) {
            if (!loaded[k])
                continue;
            sfmIndex.push_back(int(k));
            in.widths.push_back(fullW[k]);
            in.heights.push_back(fullH[k]);
            in.points.push_back(features[k].points);
            in.colors.push_back(features[k].colors);
        }
        const auto pairs = candidatePairs(int(sfmIndex.size()), 10, 35);
        say(format("Matching %.0f image pairs", double(pairs.size())));
        std::atomic<int> matched{0};
        pool.parallelFor(int(pairs.size()), [&](int b, int e, int) {
            for (int p = b; p < e; ++p) {
                if (cancelled())
                    return;
                ImagePair ip;
                ip.i = pairs[std::size_t(p)].first;
                ip.j = pairs[std::size_t(p)].second;
                const bool ok = matchImages(features[std::size_t(sfmIndex[std::size_t(ip.i)])],
                                            features[std::size_t(sfmIndex[std::size_t(ip.j)])], 30, ip);
                std::lock_guard<std::mutex> lock(progressMutex);
                const int d = ++matched;
                if (ok)
                    in.pairs.push_back(std::move(ip));
                report("poses", 0.12 + 0.10 * d / double(pairs.size()),
                       format("Matching photos %.0f / %.0f", d, double(pairs.size())));
            }
        }, 1);
        if (cancelled())
            return fail(error, "Cancelled");
        features.clear();
        features.shrink_to_fit();
        // Deterministic order regardless of thread timing.
        std::sort(in.pairs.begin(), in.pairs.end(),
                  [](const ImagePair &a, const ImagePair &b) { return a.i != b.i ? a.i < b.i : a.j < b.j; });
        say(format("%.0f verified pairs", double(in.pairs.size())));

        SfmResult sfm;
        std::string sfmError;
        const bool ok = runIncrementalSfm(in, SfmOptions(), sfm,
            [&](double f, const std::string &msg) { report("poses", 0.22 + 0.08 * f, msg); }, cancel, &sfmError);
        if (!ok)
            return fail(error, sfmError);
        for (std::size_t s = 0; s < sfmIndex.size(); ++s) {
            cameras[std::size_t(sfmIndex[s])] = sfm.cameras[s];
            valid[std::size_t(sfmIndex[s])] = sfm.registered[s];
        }
        xyz = sfm.pointsXyz;
        rgb = sfm.pointsRgb;
        say(format("Structure-from-motion: %.0f / %.0f photos registered, %.0f points, %.2f px RMS",
                   sfm.numRegistered, double(sfmIndex.size()), double(xyz.size() / 3), sfm.rmsError)
            + format(", focal %.3f x image size", sfm.focalFactor));
    }

    // Keep the posed images only.
    {
        std::vector<TrainImage> keptTrain;
        std::vector<Camera> keptCams;
        std::vector<std::string> keptNames;
        std::vector<int> keptW;
        for (std::size_t k = 0; k < n; ++k)
            if (valid[k]) {
                keptTrain.push_back(std::move(train[k]));
                keptCams.push_back(cameras[k]);
                keptNames.push_back(names[k]);
                keptW.push_back(fullW[k]);
            }
        train = std::move(keptTrain);
        cameras = std::move(keptCams);
        names = std::move(keptNames);
        fullW = std::move(keptW);
    }
    if (train.size() < 2)
        return fail(error, "Not enough photos with a known camera pose.");
    result.usedImages = int(train.size());

    // ------------------------------------------------------------------ 3. align the scene
    Eigen::Matrix3d A = Eigen::Matrix3d::Identity();
    if (!usePoses) {
        // Photos are upright (EXIF applied), so the average camera "up" is the world up.
        Eigen::Vector3d up = Eigen::Vector3d::Zero();
        for (const Camera &c : cameras)
            up += toEigen(c).transpose() * Eigen::Vector3d(0, -1, 0);
        if (up.norm() > 1e-9)
            A = Eigen::Quaterniond::FromTwoVectors(up.normalized(), Eigen::Vector3d(0, 1, 0)).toRotationMatrix();
    } // ARCore's world is already gravity aligned with +Y up.
    Eigen::Vector3d centre = Eigen::Vector3d::Zero();
    if (xyz.size() >= 30) {
        for (int k = 0; k < 3; ++k) {
            std::vector<float> axis;
            for (std::size_t i = std::size_t(k); i < xyz.size(); i += 3)
                axis.push_back(xyz[i]);
            std::nth_element(axis.begin(), axis.begin() + std::ptrdiff_t(axis.size() / 2), axis.end());
            centre(k) = axis[axis.size() / 2];
        }
    } else {
        for (const Camera &c : cameras) {
            const auto cc = c.center();
            centre += Eigen::Vector3d(cc[0], cc[1], cc[2]) / double(cameras.size());
        }
    }
    transformScene(cameras, xyz, A, centre);

    Eigen::Vector3d camMean = Eigen::Vector3d::Zero();
    for (const Camera &c : cameras) {
        const auto cc = c.center();
        camMean += Eigen::Vector3d(cc[0], cc[1], cc[2]) / double(cameras.size());
    }
    double extent = 0;
    for (const Camera &c : cameras) {
        const auto cc = c.center();
        extent = std::max(extent, (Eigen::Vector3d(cc[0], cc[1], cc[2]) - camMean).norm());
    }
    extent = std::max(extent * 1.1, 1e-3);

    // Too few points (e.g. a short ARCore session): add random ones, as 3DGS does.
    if (xyz.size() / 3 < 1000) {
        std::mt19937 rng(1);
        std::uniform_real_distribution<float> u(-1.0f, 1.0f), u01(0.0f, 1.0f);
        const float r = float(extent) * 0.6f;
        for (int k = 0; k < 10000; ++k) {
            xyz.insert(xyz.end(), {u(rng) * r, u(rng) * r, u(rng) * r});
            rgb.insert(rgb.end(), {u01(rng), u01(rng), u01(rng)});
        }
        say("Few sparse points: added random initial points");
    }

    for (std::size_t k = 0; k < train.size(); ++k) {
        Camera c = cameras[k].scaled(double(train[k].camera.width) / double(fullW[k]));
        c.width = train[k].camera.width;
        c.height = train[k].camera.height;
        train[k].camera = RenderCamera::fromCamera(c);
    }

    // ------------------------------------------------------------------ 4. train
    GaussianModel init = initGaussiansFromPoints(xyz, rgb);
    TrainOptions topt = TrainOptions::forIterations(q.iterations);
    topt.maxGaussians = q.maxGaussians;
    say(format("Training %.0f iterations at %.0f px from %.0f Gaussians (scene extent %.3f)",
               q.iterations, q.trainMaxSide, double(init.size()), extent));
    Trainer trainer(std::move(init), std::move(train), float(extent), topt, pool);
    const auto trainBegin = std::chrono::steady_clock::now();
    while (!trainer.done()) {
        if (cancelled())
            return fail(error, "Cancelled");
        trainer.step();
        const TrainStats &st = trainer.stats();
        if (st.iteration % 10 == 0 || trainer.done()) {
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - trainBegin).count();
            const double eta = elapsed / st.iteration * (q.iterations - st.iteration);
            report("training", trainStart + (0.98 - trainStart) * st.iteration / double(q.iterations),
                   format("Training %.0f / %.0f - %.1f dB - ", st.iteration, q.iterations, st.psnr)
                       + format("%.0f Gaussians - %.0f s left", double(st.gaussians), eta));
        }
        if (st.iteration % 250 == 0)
            say(format("iter %5.0f  loss %.4f  psnr %.2f dB  gaussians %.0f", st.iteration, st.loss, st.psnr,
                       double(st.gaussians)));
    }

    // ------------------------------------------------------------------ 5. export
    report("export", 0.98, "Saving result");
    const fs::path outDir = fs::path(projectDir) / "output";
    std::error_code ec;
    fs::create_directories(outDir, ec);
    result.splatPath = (outDir / "splat.ply").string();
    if (!writeSplatPly(result.splatPath, trainer.model(), error))
        return false;
    writeCamerasTxt((outDir / "cameras.txt").string(), names, cameras);
    writePointsPly((outDir / "sparse.ply").string(), xyz, rgb);
    result.gaussians = trainer.model().size();
    result.psnr = trainer.evaluatePsnr();
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    say(format("Done: %.0f Gaussians, %.2f dB mean training PSNR, %.0f s", double(result.gaussians), result.psnr,
               result.seconds));
    report("export", 1.0, "Done");
    return true;
}

} // namespace u3d
