// Gaussian Splatting core: gradient checks, loss, training convergence, PLY round trip.

#include "Test.h"

#include "u3d/splat/Crop.h"
#include "u3d/splat/Loss.h"
#include "u3d/splat/Rasterizer.h"
#include "u3d/splat/SplatPly.h"
#include "u3d/splat/Trainer.h"

#include <cstdio>
#include <random>

using namespace u3d;

namespace {

RenderCamera makeCamera(int w, int h, float f)
{
    RenderCamera c;
    c.width = w;
    c.height = h;
    c.fx = c.fy = f;
    c.cx = 0.5f * float(w - 1);
    c.cy = 0.5f * float(h - 1);
    return c;
}

// Camera on a circle around the origin, looking at it (OpenCV convention, world y down).
RenderCamera orbitCamera(int w, int h, float f, float angle, float radius, float height)
{
    RenderCamera c = makeCamera(w, h, f);
    const float C[3] = {radius * std::sin(angle), height, -radius * std::cos(angle)};
    float zf[3] = {-C[0], -C[1], -C[2]};
    const float zn = std::sqrt(zf[0] * zf[0] + zf[1] * zf[1] + zf[2] * zf[2]);
    for (float &v : zf) v /= zn;
    const float down[3] = {0, 1, 0};
    // x = down x z ... choose x = y_world_down cross z to keep a right-handed frame
    float x[3] = {down[1] * zf[2] - down[2] * zf[1], down[2] * zf[0] - down[0] * zf[2], down[0] * zf[1] - down[1] * zf[0]};
    const float xn = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    for (float &v : x) v /= -xn;
    const float y[3] = {zf[1] * x[2] - zf[2] * x[1], zf[2] * x[0] - zf[0] * x[2], zf[0] * x[1] - zf[1] * x[0]};
    for (int k = 0; k < 3; ++k) {
        c.R[k] = x[k];
        c.R[3 + k] = y[k];
        c.R[6 + k] = zf[k];
    }
    for (int r = 0; r < 3; ++r)
        c.t[r] = -(c.R[r * 3] * C[0] + c.R[r * 3 + 1] * C[1] + c.R[r * 3 + 2] * C[2]);
    return c;
}

GaussianModel randomModel(std::size_t n, std::mt19937 &rng, float spread, float zCenter,
                          float minScale, float maxScale)
{
    std::uniform_real_distribution<float> u(-1.0f, 1.0f), u01(0.0f, 1.0f);
    GaussianModel m;
    m.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        m.means[i * 3] = u(rng) * spread;
        m.means[i * 3 + 1] = u(rng) * spread * 0.8f;
        m.means[i * 3 + 2] = zCenter + u(rng) * spread;
        for (int k = 0; k < 3; ++k) {
            m.scales[i * 3 + k] = std::log(minScale + (maxScale - minScale) * u01(rng));
            m.colors[i * 3 + k] = (0.15f + 0.7f * u01(rng) - 0.5f) / kShC0;
        }
        for (int k = 0; k < 4; ++k)
            m.quats[i * 4 + k] = u(rng) + (k == 0 ? 1.5f : 0.0f);
        m.opacities[i] = logit(0.25f + 0.5f * u01(rng));
    }
    return m;
}

double weightedLoss(Rasterizer &r, const GaussianModel &m, const RenderCamera &cam,
                    const RasterSettings &st, const std::vector<float> &w)
{
    std::vector<float> rgb;
    r.forward(m, cam, st, rgb);
    double s = 0;
    for (std::size_t k = 0; k < rgb.size(); ++k)
        s += double(w[k]) * double(rgb[k]);
    return s;
}

// Relative error ||analytic - numeric|| / ||numeric|| over one parameter group.
double checkGroup(const char *name, Rasterizer &r, GaussianModel &m, std::vector<float> GaussianModel::*group,
                  const std::vector<float> &analytic, const RenderCamera &cam, const RasterSettings &st,
                  const std::vector<float> &w, float eps)
{
    std::vector<float> &params = m.*group;
    double diff2 = 0, ref2 = 0;
    for (std::size_t k = 0; k < params.size(); ++k) {
        const float orig = params[k];
        params[k] = orig + eps;
        const double lp = weightedLoss(r, m, cam, st, w);
        params[k] = orig - eps;
        const double lm = weightedLoss(r, m, cam, st, w);
        params[k] = orig;
        const double numeric = (lp - lm) / (2.0 * eps);
        diff2 += (analytic[k] - numeric) * (analytic[k] - numeric);
        ref2 += numeric * numeric;
    }
    const double rel = std::sqrt(diff2) / std::max(1e-12, std::sqrt(ref2));
    LOG("%-10s |grad| = %.4g  relative error = %.2e", name, std::sqrt(ref2), rel);
    return rel;
}

} // namespace

TEST(splat_gradients_match_finite_differences)
{
    ThreadPool pool(4);
    Rasterizer raster(pool);
    std::mt19937 rng(7);
    GaussianModel model = randomModel(8, rng, 0.6f, 3.0f, 0.08f, 0.3f);
    RenderCamera cam = makeCamera(56, 44, 45.0f);
    // A slightly rotated, translated camera exercises R and t.
    const float a = 0.15f;
    const float Rz[9] = {std::cos(a), -std::sin(a), 0, std::sin(a), std::cos(a), 0, 0, 0, 1};
    std::copy(Rz, Rz + 9, cam.R);
    cam.t[0] = 0.1f; cam.t[1] = -0.05f; cam.t[2] = 0.2f;
    RasterSettings st;
    // Single tile and no alpha/transmittance cut-offs: the rendered image is then a smooth
    // function of the parameters, so finite differences are a valid reference.
    st.tileSize = 64;
    st.minAlpha = 0.0f;
    st.minTransmittance = 0.0f;
    st.background[0] = 0.1f; st.background[1] = 0.2f; st.background[2] = 0.3f;

    std::vector<float> rgb;
    raster.forward(model, cam, st, rgb);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    std::vector<float> w(rgb.size());
    for (float &v : w) v = u(rng);

    GaussianModel grads;
    grads.resize(model.size());
    grads.setZero();
    raster.backward(model, w.data(), grads);

    const double eMeans = checkGroup("means", raster, model, &GaussianModel::means, grads.means, cam, st, w, 1e-3f);
    const double eScales = checkGroup("scales", raster, model, &GaussianModel::scales, grads.scales, cam, st, w, 1e-3f);
    const double eQuats = checkGroup("quats", raster, model, &GaussianModel::quats, grads.quats, cam, st, w, 1e-3f);
    const double eOpac = checkGroup("opacities", raster, model, &GaussianModel::opacities, grads.opacities, cam, st, w, 1e-3f);
    const double eColors = checkGroup("colors", raster, model, &GaussianModel::colors, grads.colors, cam, st, w, 1e-3f);
    CHECK(eMeans < 0.02);
    CHECK(eScales < 0.02);
    CHECK(eQuats < 0.02);
    CHECK(eOpac < 0.02);
    CHECK(eColors < 0.02);
}

TEST(splat_projection_matches_closed_form)
{
    // One Gaussian on the optical axis must render as the ellipse (f/z)^2 * Sigma_xy + eps2d.
    ThreadPool pool(1);
    Rasterizer raster(pool);
    GaussianModel m;
    m.resize(1);
    const float sx = 0.05f, sy = 0.12f, angle = 0.6f;
    m.means = {0.0f, 0.0f, 3.0f};
    m.scales = {std::log(sx), std::log(sy), std::log(0.02f)};
    m.quats = {std::cos(angle / 2), 0.0f, 0.0f, std::sin(angle / 2)};
    m.opacities = {logit(0.5f)};
    m.colors = {0.5f / kShC0, 0.5f / kShC0, 0.5f / kShC0};
    RenderCamera cam = makeCamera(201, 201, 300.0f);
    RasterSettings st;
    st.minAlpha = 0.0f;
    st.minTransmittance = 0.0f;
    std::vector<float> rgb;
    raster.forward(m, cam, st, rgb);
    double w = 0, mx = 0, my = 0;
    for (int y = 0; y < 201; ++y)
        for (int x = 0; x < 201; ++x) {
            const double v = rgb[std::size_t(y * 201 + x) * 3];
            w += v; mx += v * x; my += v * y;
        }
    mx /= w; my /= w;
    double a = 0, b = 0, c = 0;
    for (int y = 0; y < 201; ++y)
        for (int x = 0; x < 201; ++x) {
            const double v = rgb[std::size_t(y * 201 + x) * 3] / w;
            a += v * (x - mx) * (x - mx); b += v * (x - mx) * (y - my); c += v * (y - my) * (y - my);
        }
    const double k2 = (300.0 / 3.0) * (300.0 / 3.0), ca = std::cos(angle), sa = std::sin(angle);
    CHECK_NEAR(mx, 100.0, 0.01);
    CHECK_NEAR(my, 100.0, 0.01);
    CHECK_NEAR(a, k2 * (ca * ca * sx * sx + sa * sa * sy * sy) + 0.3, 0.2);
    CHECK_NEAR(b, k2 * ca * sa * (sx * sx - sy * sy), 0.2);
    CHECK_NEAR(c, k2 * (sa * sa * sx * sx + ca * ca * sy * sy) + 0.3, 0.2);
}

TEST(ssim_loss_gradient)
{
    ThreadPool pool(2);
    const int W = 21, H = 17;
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    std::vector<float> render(std::size_t(W) * H * 3), target(render.size()), grad(render.size()), g2(render.size());
    for (std::size_t k = 0; k < render.size(); ++k) {
        target[k] = u(rng);
        render[k] = 0.5f * target[k] + 0.5f * u(rng);
    }
    photometricLoss(render.data(), target.data(), W, H, 1.0f, grad.data(), pool); // pure D-SSIM
    double diff2 = 0, ref2 = 0;
    const float eps = 1e-3f;
    for (std::size_t k = 0; k < render.size(); k += 7) {
        const float o = render[k];
        render[k] = o + eps;
        const double lp = photometricLoss(render.data(), target.data(), W, H, 1.0f, g2.data(), pool).total;
        render[k] = o - eps;
        const double lm = photometricLoss(render.data(), target.data(), W, H, 1.0f, g2.data(), pool).total;
        render[k] = o;
        const double numeric = (lp - lm) / (2 * eps);
        diff2 += (grad[k] - numeric) * (grad[k] - numeric);
        ref2 += numeric * numeric;
    }
    const double rel = std::sqrt(diff2 / ref2);
    LOG("D-SSIM gradient relative error = %.2e", rel);
    CHECK(rel < 0.02);

    const LossValue same = photometricLoss(target.data(), target.data(), W, H, 0.2f, grad.data(), pool);
    CHECK_NEAR(same.ssim, 1.0, 1e-4);
    CHECK_NEAR(same.total, 0.0, 1e-4);
}

TEST(training_recovers_a_synthetic_scene)
{
    ThreadPool pool;
    std::mt19937 rng(11);
    // Ground truth: a cloud of coloured Gaussians around the origin.
    GaussianModel truth = randomModel(300, rng, 1.0f, 0.0f, 0.03f, 0.12f);
    for (float &o : truth.opacities) o = logit(0.8f);

    std::vector<TrainImage> images;
    Rasterizer raster(pool);
    RasterSettings st;
    for (int k = 0; k < 16; ++k) {
        TrainImage img;
        img.camera = orbitCamera(96, 72, 80.0f, 6.2831853f * float(k) / 16.0f, 4.0f, -1.0f + 0.15f * float(k % 4));
        std::vector<float> rendered;
        raster.forward(truth, img.camera, st, rendered);
        img.rgb = TrainImage::quantize(rendered);
        images.push_back(std::move(img));
    }

    // Start from noisy positions and grey colours (like a sparse SfM point cloud).
    std::normal_distribution<float> noise(0.0f, 0.08f);
    std::vector<float> xyz, rgb;
    for (std::size_t i = 0; i < truth.size(); i += 2)
        for (int k = 0; k < 3; ++k) {
            xyz.push_back(truth.means[i * 3 + k] + noise(rng));
            rgb.push_back(0.5f);
        }
    GaussianModel init = initGaussiansFromPoints(xyz, rgb);

    TrainOptions opt = TrainOptions::forIterations(1500);
    opt.maxGaussians = 4000;
    Trainer trainer(init, images, 4.4f, opt, pool);
    const float before = trainer.evaluatePsnr();
    while (!trainer.done())
        trainer.step();
    const float after = trainer.evaluatePsnr();
    LOG("PSNR %.2f dB -> %.2f dB, %zu -> %zu Gaussians", before, after, init.size(), trainer.model().size());
    CHECK(after > before + 8.0f);
    CHECK(after > 26.0f);
    CHECK(trainer.model().size() != init.size()); // density control ran
}

TEST(ply_round_trip)
{
    std::mt19937 rng(5);
    GaussianModel m = randomModel(50, rng, 1.0f, 0.0f, 0.05f, 0.2f);
    const std::string path = "u3d_test_splat.ply";
    std::string err;
    CHECK(writeSplatPly(path, m, &err));
    GaussianModel r;
    bool isSplat = false;
    CHECK(readSplatPly(path, r, &err, &isSplat));
    CHECK(isSplat);
    CHECK(r.size() == m.size());
    for (std::size_t k = 0; k < m.means.size(); ++k) CHECK(r.means[k] == m.means[k]);
    for (std::size_t k = 0; k < m.quats.size(); ++k) CHECK(r.quats[k] == m.quats[k]);
    for (std::size_t k = 0; k < m.size(); ++k) CHECK(r.opacities[k] == m.opacities[k]);

    const std::vector<float> xyz = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
    const std::vector<float> col = {1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1};
    CHECK(writePointsPly("u3d_test_points.ply", xyz, col, &err));
    std::vector<float> x2, c2;
    CHECK(readPointsPly("u3d_test_points.ply", x2, c2, &err));
    CHECK(x2 == xyz);
    CHECK_NEAR(c2[0], 1.0, 1e-6);
    CHECK(readSplatPly("u3d_test_points.ply", r, &err, &isSplat));
    CHECK(!isSplat);
    CHECK(r.size() == 4);
    std::remove(path.c_str());
    std::remove("u3d_test_points.ply");
}

TEST(crop_keeps_the_object_and_removes_background)
{
    // An opaque object in [-0.5, 0.5]^3, a ring of background splats far around it, a few
    // stray floaters further out, and faint Gaussians inside the object.
    std::mt19937 rng(21);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    GaussianModel m;
    auto add = [&](float x, float y, float z, float opacity) {
        GaussianModel one;
        one.resize(1);
        one.means = {x, y, z};
        one.scales = {-4, -4, -4};
        one.quats = {1, 0, 0, 0};
        one.opacities = {logit(opacity)};
        one.colors = {0, 0, 0};
        m.appendFrom(one, 0);
    };
    for (int i = 0; i < 2000; ++i)
        add(0.5f * u(rng), 0.5f * u(rng), 0.5f * u(rng), 0.9f);
    for (int i = 0; i < 200; ++i) {
        const float a = 3.14159f * u(rng);
        add(3.0f * std::cos(a), 0.3f * u(rng), 3.0f * std::sin(a), 0.9f); // background ring
    }
    for (int i = 0; i < 5; ++i)
        add(20.0f + u(rng), 15.0f, -30.0f, 0.9f); // a few far floaters (0.2 %)
    for (int i = 0; i < 100; ++i)
        add(0.2f * u(rng), 0.2f * u(rng), 0.2f * u(rng), 0.01f); // faint

    const CropBox box = suggestCropBox(m);
    LOG("suggested box: [%.2f %.2f %.2f] - [%.2f %.2f %.2f]", box.min[0], box.min[1], box.min[2], box.max[0],
        box.max[1], box.max[2]);
    for (int k = 0; k < 3; ++k) {
        CHECK(box.min[k] < -0.4f && box.min[k] > -1.0f); // hugs the object, excludes the ring
        CHECK(box.max[k] > 0.4f && box.max[k] < 1.0f);
    }

    const GaussianModel cropped = cropModel(m, box, 0.05f);
    LOG("%zu -> %zu Gaussians", m.size(), cropped.size());
    CHECK(cropped.size() >= 1900 && cropped.size() <= 2000); // object kept, faint ones removed
    for (std::size_t i = 0; i < cropped.size(); ++i)
        CHECK(box.contains(&cropped.means[i * 3]));

    // The slider range ignores the far floaters but still covers the background ring.
    const CropBox bounds = modelBounds(m);
    CHECK(bounds.max[0] > 2.9f && bounds.max[0] < 15.0f);
}
