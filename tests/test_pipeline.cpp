// End-to-end: render photos of a textured box, write them as a project (JPEG files),
// and run the same reconstruction pipeline the phone runs - once with ARCore-style poses, once
// with poses from structure-from-motion.

#include "Synthetic.h"
#include "Test.h"

#include "u3d/Project.h"
#include "u3d/Reconstructor.h"
#include "u3d/splat/Rasterizer.h"
#include "u3d/splat/SplatPly.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <filesystem>
#include <random>

using namespace u3d;
namespace fs = std::filesystem;

namespace {

// Multi-scale coloured noise: sharp, photo-like texture with features at every scale.
cv::Mat makeTexture(unsigned seed)
{
    cv::RNG rng(seed);
    cv::Mat acc = cv::Mat::zeros(512, 512, CV_32FC3);
    for (int size = 4; size <= 512; size *= 2) {
        cv::Mat noise(size, size, CV_32FC3), up;
        rng.fill(noise, cv::RNG::UNIFORM, 0.0f, 1.0f);
        cv::resize(noise, up, acc.size(), 0, 0, cv::INTER_LINEAR);
        acc += up;
    }
    cv::Mat out;
    cv::normalize(acc, acc, 0, 255, cv::NORM_MINMAX);
    acc.convertTo(out, CV_8UC3);
    // Stretch the brightness contrast (SIFT works on intensity), keep the colours.
    cv::Mat ycc;
    cv::cvtColor(out, ycc, cv::COLOR_BGR2YCrCb);
    std::vector<cv::Mat> ch;
    cv::split(ycc, ch);
    cv::equalizeHist(ch[0], ch[0]);
    cv::merge(ch, ycc);
    cv::cvtColor(ycc, out, cv::COLOR_YCrCb2BGR);
    return out;
}

// Renders a textured box (exact per-face perspective warps) from `cam`.
cv::Mat renderBox(const Camera &cam, const std::vector<cv::Mat> &textures)
{
    const double h[3] = {0.8, 0.6, 0.7};
    cv::Mat img(cam.height, cam.width, CV_8UC3, cv::Scalar(40, 40, 40));
    const auto C = cam.center();
    for (int f = 0; f < 6; ++f) {
        const int axis = f / 2;
        const double sign = (f % 2) ? 1.0 : -1.0;
        const int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
        if (sign * (C[std::size_t(axis)] - sign * h[axis]) <= 0)
            continue; // back face
        std::vector<cv::Point2f> projected;
        const double uv[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        bool ok = true;
        for (const auto &c : uv) {
            std::array<double, 3> X{};
            X[std::size_t(axis)] = sign * h[axis];
            X[std::size_t(a1)] = c[0] * h[a1];
            X[std::size_t(a2)] = c[1] * h[a2];
            double u, v;
            ok = ok && cam.project(X, u, v);
            projected.emplace_back(float(u), float(v));
        }
        if (!ok)
            continue;
        const std::vector<cv::Point2f> src = {{0, 0}, {511, 0}, {511, 511}, {0, 511}};
        const cv::Mat H = cv::getPerspectiveTransform(src, projected);
        cv::warpPerspective(textures[std::size_t(f)], img, H, img.size(), cv::INTER_LINEAR, cv::BORDER_TRANSPARENT);
    }
    return img;
}

std::string makeProject(const std::string &name, bool withPoses)
{
    const fs::path dir = fs::temp_directory_path() / name;
    fs::remove_all(dir);
    fs::create_directories(dir / "images");

    std::vector<cv::Mat> textures;
    for (unsigned f = 0; f < 6; ++f)
        textures.push_back(makeTexture(100 + f));
    std::vector<std::string> names;
    std::vector<Camera> cams;
    const int n = 30;
    for (int k = 0; k < n; ++k) {
        const Camera cam = u3dtest::orbitCamera(640, 480, 560.0, 6.2831853 * k / n, 3.2, -1.2 + 0.3 * (k % 3));
        char file[32];
        std::snprintf(file, sizeof file, "IMG_%04d.jpg", k);
        cv::imwrite((dir / "images" / file).string(), renderBox(cam, textures), {cv::IMWRITE_JPEG_QUALITY, 95});
        names.push_back(file);
        cams.push_back(cam);
    }
    if (withPoses) {
        fs::create_directories(dir / "arcore");
        writeCamerasTxt((dir / "arcore" / "cameras.txt").string(), names, cams);
        // ARCore also records sparse feature points (noisy, a few thousand per session).
        std::mt19937 rng(4);
        std::uniform_real_distribution<double> u(-1.0, 1.0);
        std::normal_distribution<double> noise(0.0, 0.01);
        std::uniform_int_distribution<int> face(0, 5);
        const double h[3] = {0.8, 0.6, 0.7};
        std::vector<float> xyz, rgb;
        for (int i = 0; i < 3000; ++i) {
            const int f = face(rng), axis = f / 2;
            double X[3] = {u(rng) * h[0], u(rng) * h[1], u(rng) * h[2]};
            X[axis] = ((f % 2) ? 1.0 : -1.0) * h[axis];
            for (int k = 0; k < 3; ++k) {
                xyz.push_back(float(X[k] + noise(rng)));
                rgb.push_back(0.5f);
            }
        }
        writePointsPly((dir / "arcore" / "points.ply").string(), xyz, rgb);
    }
    return dir.string();
}

ReconstructResult reconstruct(const std::string &dir, bool expectArcore)
{
    ReconstructOptions opt;
    opt.quality = Quality::Fast;
    opt.iterations = 1000; // keeps CI (4-core runners) reasonably fast
    ReconstructResult result;
    std::string error;
    double lastFraction = -1;
    bool monotonic = true;
    const bool ok = reconstructProject(dir, opt, result,
        [&](const Progress &p) {
            if (p.fraction + 1e-9 < lastFraction)
                monotonic = false;
            lastFraction = p.fraction;
        },
        [](const std::string &line) { LOG("%s", line.c_str()); }, nullptr, &error);
    if (!ok)
        throw u3dtest::Failure("reconstruction failed: " + error);
    CHECK(monotonic);
    CHECK_NEAR(lastFraction, 1.0, 1e-9);
    CHECK(result.posesFromArcore == expectArcore);
    CHECK(fs::exists(result.splatPath));
    CHECK(fs::exists(fs::path(dir) / "output" / "cameras.txt"));
    GaussianModel loaded;
    CHECK(readSplatPly(result.splatPath, loaded));
    CHECK(loaded.size() == result.gaussians);
    return result;
}

} // namespace

TEST(pipeline_with_arcore_poses)
{
    const std::string dir = makeProject("u3d_test_project_arcore", true);
    const ReconstructResult r = reconstruct(dir, true);
    CHECK(r.usedImages == 30);
    CHECK(r.psnr > 22.0f);
    fs::remove_all(dir);
}

TEST(pipeline_with_structure_from_motion)
{
    const std::string dir = makeProject("u3d_test_project_sfm", false);
    const ReconstructResult r = reconstruct(dir, false);
    CHECK(r.usedImages >= 27);
    CHECK(r.psnr > 22.0f);
    fs::remove_all(dir);
}
