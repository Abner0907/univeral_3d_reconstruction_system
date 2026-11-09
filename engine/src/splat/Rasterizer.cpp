#include "u3d/splat/Rasterizer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numeric>

namespace u3d {

namespace {

constexpr float kMaxAlpha = 0.99f;
constexpr int kGrad2dStride = 9; // dmx dmy dA dB dC dr dg db dalpha

inline void atomicAdd(std::atomic<float> &target, float value)
{
    float current = target.load(std::memory_order_relaxed);
    while (!target.compare_exchange_weak(current, current + value, std::memory_order_relaxed)) {
    }
}

// Rotation matrix (row-major) of a normalised quaternion (w, x, y, z).
inline void quatToRot(float w, float x, float y, float z, float R[9])
{
    R[0] = 1 - 2 * (y * y + z * z); R[1] = 2 * (x * y - w * z);     R[2] = 2 * (x * z + w * y);
    R[3] = 2 * (x * y + w * z);     R[4] = 1 - 2 * (x * x + z * z); R[5] = 2 * (y * z - w * x);
    R[6] = 2 * (x * z - w * y);     R[7] = 2 * (y * z + w * x);     R[8] = 1 - 2 * (x * x + y * y);
}

// Everything the forward projection computes for one Gaussian; recomputed in backward.
struct Projection {
    float pc[3];      // camera-space mean
    float q[4];       // normalised quaternion
    float qNorm;
    float Rq[9];
    float s[3];       // exp(log scales)
    float M[9];       // Rq * diag(s)
    float Sigma[9];   // M M^T
    float tx, ty;     // possibly clamped x, y used in the Jacobian
    float limTx, limTy;
    bool clampX, clampY;
    float T0[3], T1[3]; // rows of J * W
    float a, b, c;      // 2D covariance (with eps)
};

// Returns false if the Gaussian is behind the near plane.
inline bool computeProjection(const GaussianModel &m, std::size_t i, const RenderCamera &cam,
                              const RasterSettings &st, Projection &p)
{
    const float *mu = &m.means[i * 3];
    const float *R = cam.R;
    p.pc[0] = R[0] * mu[0] + R[1] * mu[1] + R[2] * mu[2] + cam.t[0];
    p.pc[1] = R[3] * mu[0] + R[4] * mu[1] + R[5] * mu[2] + cam.t[1];
    p.pc[2] = R[6] * mu[0] + R[7] * mu[1] + R[8] * mu[2] + cam.t[2];
    const float z = p.pc[2];
    if (z < st.nearPlane)
        return false;

    const float *qr = &m.quats[i * 4];
    p.qNorm = std::sqrt(qr[0] * qr[0] + qr[1] * qr[1] + qr[2] * qr[2] + qr[3] * qr[3]);
    if (p.qNorm < 1e-12f) {
        p.qNorm = 1.0f;
        p.q[0] = 1; p.q[1] = p.q[2] = p.q[3] = 0;
    } else {
        for (int k = 0; k < 4; ++k)
            p.q[k] = qr[k] / p.qNorm;
    }
    quatToRot(p.q[0], p.q[1], p.q[2], p.q[3], p.Rq);
    for (int k = 0; k < 3; ++k)
        p.s[k] = std::exp(m.scales[i * 3 + k]);
    for (int r = 0; r < 3; ++r)
        for (int col = 0; col < 3; ++col)
            p.M[r * 3 + col] = p.Rq[r * 3 + col] * p.s[col];
    for (int r = 0; r < 3; ++r)
        for (int col = 0; col < 3; ++col)
            p.Sigma[r * 3 + col] = p.M[r * 3] * p.M[col * 3] + p.M[r * 3 + 1] * p.M[col * 3 + 1]
                                   + p.M[r * 3 + 2] * p.M[col * 3 + 2];

    // Clamp the Jacobian's x/z, y/z slightly outside the image (gsplat) for stability.
    const float limXPos = (float(cam.width) - cam.cx) / cam.fx + 0.3f * 0.5f * float(cam.width) / cam.fx;
    const float limXNeg = cam.cx / cam.fx + 0.3f * 0.5f * float(cam.width) / cam.fx;
    const float limYPos = (float(cam.height) - cam.cy) / cam.fy + 0.3f * 0.5f * float(cam.height) / cam.fy;
    const float limYNeg = cam.cy / cam.fy + 0.3f * 0.5f * float(cam.height) / cam.fy;
    const float xz = p.pc[0] / z, yz = p.pc[1] / z;
    const float cxz = std::min(limXPos, std::max(-limXNeg, xz));
    const float cyz = std::min(limYPos, std::max(-limYNeg, yz));
    p.clampX = cxz != xz;
    p.clampY = cyz != yz;
    p.limTx = cxz;
    p.limTy = cyz;
    p.tx = z * cxz;
    p.ty = z * cyz;

    const float J00 = cam.fx / z, J02 = -cam.fx * p.tx / (z * z);
    const float J11 = cam.fy / z, J12 = -cam.fy * p.ty / (z * z);
    for (int k = 0; k < 3; ++k) {
        p.T0[k] = J00 * R[k] + J02 * R[6 + k];
        p.T1[k] = J11 * R[3 + k] + J12 * R[6 + k];
    }
    float S0[3], S1[3]; // Sigma * T0, Sigma * T1
    for (int r = 0; r < 3; ++r) {
        S0[r] = p.Sigma[r * 3] * p.T0[0] + p.Sigma[r * 3 + 1] * p.T0[1] + p.Sigma[r * 3 + 2] * p.T0[2];
        S1[r] = p.Sigma[r * 3] * p.T1[0] + p.Sigma[r * 3 + 1] * p.T1[1] + p.Sigma[r * 3 + 2] * p.T1[2];
    }
    p.a = p.T0[0] * S0[0] + p.T0[1] * S0[1] + p.T0[2] * S0[2] + st.eps2d;
    p.b = p.T0[0] * S1[0] + p.T0[1] * S1[1] + p.T0[2] * S1[2];
    p.c = p.T1[0] * S1[0] + p.T1[1] * S1[1] + p.T1[2] * S1[2] + st.eps2d;
    return true;
}

} // namespace

RenderCamera RenderCamera::fromCamera(const Camera &c)
{
    RenderCamera r;
    r.width = c.width;
    r.height = c.height;
    r.fx = float(c.fx);
    r.fy = float(c.fy);
    r.cx = float(c.cx);
    r.cy = float(c.cy);
    for (int k = 0; k < 9; ++k)
        r.R[k] = float(c.R[k]);
    for (int k = 0; k < 3; ++k)
        r.t[k] = float(c.t[k]);
    return r;
}

Rasterizer::Rasterizer(ThreadPool &pool)
    : m_pool(pool)
{
}

// ----------------------------------------------------------------------------- forward

void Rasterizer::project(const GaussianModel &model)
{
    const std::size_t n = model.size();
    m_n = n;
    m_mean2d.resize(n * 2);
    m_conic.resize(n * 3);
    m_cov2d.resize(n * 3);
    m_depth.resize(n);
    m_rgb.resize(n * 3);
    m_alpha.resize(n);
    m_radius.assign(n, 0);
    m_rect.resize(n * 4);
    m_flags.assign(n, 0);

    const RenderCamera cam = m_cam;
    const RasterSettings st = m_settings;
    const int ts = st.tileSize;
    m_pool.parallelFor(int(n), [&](int begin, int end, int) {
        for (int ii = begin; ii < end; ++ii) {
            const std::size_t i = std::size_t(ii);
            Projection p;
            if (!computeProjection(model, i, cam, st, p))
                continue;
            const float det = p.a * p.c - p.b * p.b;
            if (!(det > 0.0f))
                continue;
            const float invDet = 1.0f / det;
            const float z = p.pc[2];
            const float u = cam.fx * p.pc[0] / z + cam.cx;
            const float v = cam.fy * p.pc[1] / z + cam.cy;
            const float mid = 0.5f * (p.a + p.c);
            const float lambda = mid + std::sqrt(std::max(0.1f, mid * mid - det));
            const int radius = int(std::ceil(3.0f * std::sqrt(lambda)));
            if (u + radius < 0 || v + radius < 0 || u - radius >= cam.width || v - radius >= cam.height)
                continue;
            const int tx0 = std::max(0, int(std::floor((u - radius) / ts)));
            const int ty0 = std::max(0, int(std::floor((v - radius) / ts)));
            const int tx1 = std::min(m_tilesX, int(std::floor((u + radius) / ts)) + 1);
            const int ty1 = std::min(m_tilesY, int(std::floor((v + radius) / ts)) + 1);
            if (tx0 >= tx1 || ty0 >= ty1)
                continue;

            m_mean2d[i * 2] = u;
            m_mean2d[i * 2 + 1] = v;
            m_cov2d[i * 3] = p.a;
            m_cov2d[i * 3 + 1] = p.b;
            m_cov2d[i * 3 + 2] = p.c;
            m_conic[i * 3] = p.c * invDet;
            m_conic[i * 3 + 1] = -p.b * invDet;
            m_conic[i * 3 + 2] = p.a * invDet;
            m_depth[i] = z;
            std::uint8_t flags = std::uint8_t((p.clampX ? 1 : 0) | (p.clampY ? 2 : 0));
            for (int ch = 0; ch < 3; ++ch) {
                const float col = 0.5f + kShC0 * model.colors[i * 3 + ch];
                if (col < 0.0f)
                    flags |= std::uint8_t(4 << ch);
                m_rgb[i * 3 + ch] = std::max(col, 0.0f);
            }
            m_flags[i] = flags;
            m_alpha[i] = sigmoid(model.opacities[i]);
            m_rect[i * 4] = tx0;
            m_rect[i * 4 + 1] = ty0;
            m_rect[i * 4 + 2] = tx1;
            m_rect[i * 4 + 3] = ty1;
            m_radius[i] = radius;
        }
    }, 256);
}

void Rasterizer::binTiles()
{
    std::vector<int> visible;
    visible.reserve(m_n);
    for (std::size_t i = 0; i < m_n; ++i)
        if (m_radius[i] > 0)
            visible.push_back(int(i));
    // Global depth sort, so that bucketing in this order leaves every tile list depth-sorted.
    // LSD radix sort on the float bits (depths are positive, so the bit patterns are ordered).
    {
        const std::size_t v = visible.size();
        std::vector<std::uint32_t> keys(v), keys2(v);
        std::vector<int> tmp(v);
        for (std::size_t k = 0; k < v; ++k)
            std::memcpy(&keys[k], &m_depth[std::size_t(visible[k])], 4);
        std::vector<std::uint32_t> count(65536);
        for (int pass = 0; pass < 2; ++pass) {
            const int shift = pass * 16;
            std::fill(count.begin(), count.end(), 0u);
            for (std::size_t k = 0; k < v; ++k)
                ++count[(keys[k] >> shift) & 0xFFFF];
            std::uint32_t sum = 0;
            for (std::uint32_t &c : count) {
                const std::uint32_t c0 = c;
                c = sum;
                sum += c0;
            }
            for (std::size_t k = 0; k < v; ++k) {
                const std::uint32_t pos = count[(keys[k] >> shift) & 0xFFFF]++;
                tmp[pos] = visible[k];
                keys2[pos] = keys[k];
            }
            visible.swap(tmp);
            keys.swap(keys2);
        }
    }

    const int tiles = m_tilesX * m_tilesY;
    m_tileOffsets.assign(std::size_t(tiles) + 1, 0);
    for (int g : visible) {
        const int *r = &m_rect[std::size_t(g) * 4];
        for (int ty = r[1]; ty < r[3]; ++ty)
            for (int tx = r[0]; tx < r[2]; ++tx)
                ++m_tileOffsets[std::size_t(ty * m_tilesX + tx) + 1];
    }
    for (int t = 0; t < tiles; ++t)
        m_tileOffsets[std::size_t(t) + 1] += m_tileOffsets[std::size_t(t)];
    m_tileList.resize(std::size_t(m_tileOffsets.back()));
    std::vector<int> cursor(m_tileOffsets.begin(), m_tileOffsets.end() - 1);
    for (int g : visible) {
        const int *r = &m_rect[std::size_t(g) * 4];
        for (int ty = r[1]; ty < r[3]; ++ty)
            for (int tx = r[0]; tx < r[2]; ++tx)
                m_tileList[std::size_t(cursor[std::size_t(ty * m_tilesX + tx)]++)] = g;
    }
}

void Rasterizer::forward(const GaussianModel &model, const RenderCamera &camera,
                         const RasterSettings &settings, std::vector<float> &rgb)
{
    m_cam = camera;
    m_settings = settings;
    const int W = camera.width, H = camera.height, ts = settings.tileSize;
    m_tilesX = (W + ts - 1) / ts;
    m_tilesY = (H + ts - 1) / ts;

    project(model);
    binTiles();

    const float kMinAlpha = settings.minAlpha, kMinTransmittance = settings.minTransmittance;
    rgb.resize(std::size_t(W) * H * 3);
    m_finalT.resize(std::size_t(W) * H);
    m_nContrib.resize(std::size_t(W) * H);
    const float *bg = settings.background;

    m_pool.parallelFor(m_tilesX * m_tilesY, [&](int begin, int end, int) {
        std::vector<float> local;
        for (int tile = begin; tile < end; ++tile) {
            const int tx = tile % m_tilesX, ty = tile / m_tilesX;
            const int listBegin = m_tileOffsets[std::size_t(tile)];
            const int count = m_tileOffsets[std::size_t(tile) + 1] - listBegin;
            // Pack this tile's Gaussians contiguously: mx my A B C alpha r g b.
            local.resize(std::size_t(count) * 9);
            for (int j = 0; j < count; ++j) {
                const std::size_t g = std::size_t(m_tileList[std::size_t(listBegin + j)]);
                float *d = &local[std::size_t(j) * 9];
                d[0] = m_mean2d[g * 2]; d[1] = m_mean2d[g * 2 + 1];
                d[2] = m_conic[g * 3]; d[3] = m_conic[g * 3 + 1]; d[4] = m_conic[g * 3 + 2];
                d[5] = m_alpha[g];
                d[6] = m_rgb[g * 3]; d[7] = m_rgb[g * 3 + 1]; d[8] = m_rgb[g * 3 + 2];
            }
            const int x0 = tx * ts, y0 = ty * ts;
            const int x1 = std::min(W, x0 + ts), y1 = std::min(H, y0 + ts);
            for (int py = y0; py < y1; ++py) {
                for (int px = x0; px < x1; ++px) {
                    float T = 1.0f, cr = 0, cg = 0, cb = 0;
                    int last = 0;
                    for (int j = 0; j < count; ++j) {
                        const float *d = &local[std::size_t(j) * 9];
                        const float dx = d[0] - float(px), dy = d[1] - float(py);
                        const float power = -0.5f * (d[2] * dx * dx + d[4] * dy * dy) - d[3] * dx * dy;
                        if (power > 0.0f)
                            continue;
                        const float alpha = std::min(kMaxAlpha, d[5] * std::exp(power));
                        if (alpha < kMinAlpha)
                            continue;
                        const float nextT = T * (1.0f - alpha);
                        if (nextT < kMinTransmittance)
                            break;
                        const float w = alpha * T;
                        cr += d[6] * w; cg += d[7] * w; cb += d[8] * w;
                        T = nextT;
                        last = j + 1;
                    }
                    const std::size_t pix = std::size_t(py) * W + px;
                    rgb[pix * 3] = cr + T * bg[0];
                    rgb[pix * 3 + 1] = cg + T * bg[1];
                    rgb[pix * 3 + 2] = cb + T * bg[2];
                    m_finalT[pix] = T;
                    m_nContrib[pix] = last;
                }
            }
        }
    }, 1);
}

// ----------------------------------------------------------------------------- backward

void Rasterizer::backward(const GaussianModel &model, const float *dLdRgb, GaussianModel &grads,
                          float *mean2dGradNdc)
{
    const std::size_t n = m_n;
    if (m_grad2dCapacity < n * kGrad2dStride) {
        m_grad2dCapacity = n * kGrad2dStride + n * kGrad2dStride / 2 + 64;
        m_grad2d.reset(new std::atomic<float>[m_grad2dCapacity]);
    }
    std::atomic<float> *g2d = m_grad2d.get();
    m_pool.parallelFor(int(n * kGrad2dStride), [&](int b, int e, int) {
        for (int k = b; k < e; ++k)
            g2d[k].store(0.0f, std::memory_order_relaxed);
    }, 4096);

    const int W = m_cam.width, H = m_cam.height, ts = m_settings.tileSize;
    const float *bg = m_settings.background;
    const float kMinAlpha = m_settings.minAlpha;

    // 1) Per tile, per pixel: gradients wrt each Gaussian's 2D parameters.
    m_pool.parallelFor(m_tilesX * m_tilesY, [&](int begin, int end, int) {
        std::vector<float> local, acc;
        for (int tile = begin; tile < end; ++tile) {
            const int tx = tile % m_tilesX, ty = tile / m_tilesX;
            const int listBegin = m_tileOffsets[std::size_t(tile)];
            const int count = m_tileOffsets[std::size_t(tile) + 1] - listBegin;
            if (count == 0)
                continue;
            local.resize(std::size_t(count) * 9);
            acc.assign(std::size_t(count) * kGrad2dStride, 0.0f);
            for (int j = 0; j < count; ++j) {
                const std::size_t g = std::size_t(m_tileList[std::size_t(listBegin + j)]);
                float *d = &local[std::size_t(j) * 9];
                d[0] = m_mean2d[g * 2]; d[1] = m_mean2d[g * 2 + 1];
                d[2] = m_conic[g * 3]; d[3] = m_conic[g * 3 + 1]; d[4] = m_conic[g * 3 + 2];
                d[5] = m_alpha[g];
                d[6] = m_rgb[g * 3]; d[7] = m_rgb[g * 3 + 1]; d[8] = m_rgb[g * 3 + 2];
            }
            const int x0 = tx * ts, y0 = ty * ts;
            const int x1 = std::min(W, x0 + ts), y1 = std::min(H, y0 + ts);
            for (int py = y0; py < y1; ++py) {
                for (int px = x0; px < x1; ++px) {
                    const std::size_t pix = std::size_t(py) * W + px;
                    const float *dpix = &dLdRgb[pix * 3];
                    const float finalT = m_finalT[pix];
                    const float bgDot = bg[0] * dpix[0] + bg[1] * dpix[1] + bg[2] * dpix[2];
                    float T = finalT;
                    float accum[3] = {0, 0, 0};
                    float lastColor[3] = {0, 0, 0};
                    float lastAlpha = 0;
                    for (int j = m_nContrib[pix] - 1; j >= 0; --j) {
                        const float *d = &local[std::size_t(j) * 9];
                        const float dx = d[0] - float(px), dy = d[1] - float(py);
                        const float power = -0.5f * (d[2] * dx * dx + d[4] * dy * dy) - d[3] * dx * dy;
                        if (power > 0.0f)
                            continue;
                        const float G = std::exp(power);
                        const float rawAlpha = d[5] * G;
                        const float alpha = std::min(kMaxAlpha, rawAlpha);
                        if (alpha < kMinAlpha)
                            continue;
                        T = T / (1.0f - alpha);
                        const float w = alpha * T;
                        float *a = &acc[std::size_t(j) * kGrad2dStride];
                        float dAlpha = 0.0f;
                        for (int ch = 0; ch < 3; ++ch) {
                            accum[ch] = lastAlpha * lastColor[ch] + (1.0f - lastAlpha) * accum[ch];
                            lastColor[ch] = d[6 + ch];
                            dAlpha += (d[6 + ch] - accum[ch]) * dpix[ch];
                            a[5 + ch] += w * dpix[ch];
                        }
                        dAlpha *= T;
                        lastAlpha = alpha;
                        dAlpha += -finalT / (1.0f - alpha) * bgDot;
                        if (rawAlpha > kMaxAlpha)
                            continue; // clamped: no gradient through alpha
                        a[8] += G * dAlpha;                // d alpha / d opacity = G
                        const float dPower = d[5] * dAlpha * G; // dL/dG * dG/dpower
                        a[0] += dPower * -(d[2] * dx + d[3] * dy);
                        a[1] += dPower * -(d[4] * dy + d[3] * dx);
                        a[2] += dPower * -0.5f * dx * dx;
                        a[3] += dPower * -dx * dy;
                        a[4] += dPower * -0.5f * dy * dy;
                    }
                }
            }
            for (int j = 0; j < count; ++j) {
                const float *a = &acc[std::size_t(j) * kGrad2dStride];
                std::atomic<float> *dst = &g2d[std::size_t(m_tileList[std::size_t(listBegin + j)]) * kGrad2dStride];
                for (int k = 0; k < kGrad2dStride; ++k)
                    if (a[k] != 0.0f)
                        atomicAdd(dst[k], a[k]);
            }
        }
    }, 1);

    // 2) Per Gaussian: chain the 2D gradients back to the 3D parameters.
    const RenderCamera cam = m_cam;
    const RasterSettings st = m_settings;
    m_pool.parallelFor(int(n), [&](int begin, int end, int) {
        for (int ii = begin; ii < end; ++ii) {
            const std::size_t i = std::size_t(ii);
            if (m_radius[i] == 0)
                continue;
            float g[kGrad2dStride];
            for (int k = 0; k < kGrad2dStride; ++k)
                g[k] = g2d[i * kGrad2dStride + k].load(std::memory_order_relaxed);
            const float dmx = g[0], dmy = g[1], dA = g[2], dB = g[3], dC = g[4];

            if (mean2dGradNdc) {
                const float nx = dmx * 0.5f * float(cam.width), ny = dmy * 0.5f * float(cam.height);
                mean2dGradNdc[i] += std::sqrt(nx * nx + ny * ny);
            }

            // Colour and opacity.
            for (int ch = 0; ch < 3; ++ch)
                if (!(m_flags[i] & (4 << ch)))
                    grads.colors[i * 3 + ch] += kShC0 * g[5 + ch];
            const float op = m_alpha[i];
            grads.opacities[i] += g[8] * op * (1.0f - op);

            Projection p;
            computeProjection(model, i, cam, st, p);
            const float z = p.pc[2];

            // Conic (inverse covariance) -> 2D covariance.
            const float a = p.a, b = p.b, c = p.c;
            const float det = a * c - b * b;
            const float det2 = det * det;
            const float da = (dA * -c * c + dB * b * c + dC * -b * b) / det2;
            const float db = (dA * 2 * b * c + dC * 2 * a * b) / det2 + dB * (-1.0f / det - 2 * b * b / det2);
            const float dc = (dA * -b * b + dB * a * b + dC * -a * a) / det2;

            // 2D covariance = T Sigma T^T.  G = dL/dcov2d (symmetric, off-diagonal halved).
            const float G00 = da, G01 = 0.5f * db, G11 = dc;
            float dSigma[9];
            for (int k = 0; k < 3; ++k)
                for (int l = 0; l < 3; ++l)
                    dSigma[k * 3 + l] = p.T0[k] * (G00 * p.T0[l] + G01 * p.T1[l])
                                        + p.T1[k] * (G01 * p.T0[l] + G11 * p.T1[l]);
            // dT = 2 G T Sigma
            float TS0[3], TS1[3];
            for (int col = 0; col < 3; ++col) {
                TS0[col] = p.T0[0] * p.Sigma[col] + p.T0[1] * p.Sigma[3 + col] + p.T0[2] * p.Sigma[6 + col];
                TS1[col] = p.T1[0] * p.Sigma[col] + p.T1[1] * p.Sigma[3 + col] + p.T1[2] * p.Sigma[6 + col];
            }
            float dT0[3], dT1[3];
            for (int col = 0; col < 3; ++col) {
                dT0[col] = 2.0f * (G00 * TS0[col] + G01 * TS1[col]);
                dT1[col] = 2.0f * (G01 * TS0[col] + G11 * TS1[col]);
            }

            // Sigma = M M^T -> dM = 2 dSigma M;  M = Rq diag(s).
            float dM[9];
            for (int r = 0; r < 3; ++r)
                for (int col = 0; col < 3; ++col)
                    dM[r * 3 + col] = 2.0f * (dSigma[r * 3] * p.M[col] + dSigma[r * 3 + 1] * p.M[3 + col]
                                              + dSigma[r * 3 + 2] * p.M[6 + col]);
            float dRq[9];
            for (int col = 0; col < 3; ++col) {
                float ds = 0;
                for (int r = 0; r < 3; ++r) {
                    ds += dM[r * 3 + col] * p.Rq[r * 3 + col];
                    dRq[r * 3 + col] = dM[r * 3 + col] * p.s[col];
                }
                grads.scales[i * 3 + col] += ds * p.s[col]; // d exp(x)/dx
            }

            // Rotation matrix -> normalised quaternion -> raw quaternion.
            const float w = p.q[0], x = p.q[1], y = p.q[2], zq = p.q[3];
            const float dw = 2 * (-zq * dRq[1] + y * dRq[2] + zq * dRq[3] - x * dRq[5] - y * dRq[6] + x * dRq[7]);
            const float dx = 2 * (y * dRq[1] + zq * dRq[2] + y * dRq[3] - 2 * x * dRq[4] - w * dRq[5]
                                  + zq * dRq[6] + w * dRq[7] - 2 * x * dRq[8]);
            const float dy = 2 * (-2 * y * dRq[0] + x * dRq[1] + w * dRq[2] + x * dRq[3] + zq * dRq[5]
                                  - w * dRq[6] + zq * dRq[7] - 2 * y * dRq[8]);
            const float dz = 2 * (-2 * zq * dRq[0] - w * dRq[1] + x * dRq[2] + w * dRq[3] - 2 * zq * dRq[4]
                                  + y * dRq[5] + x * dRq[6] + y * dRq[7]);
            const float dq[4] = {dw, dx, dy, dz};
            const float qdot = dq[0] * p.q[0] + dq[1] * p.q[1] + dq[2] * p.q[2] + dq[3] * p.q[3];
            for (int k = 0; k < 4; ++k)
                grads.quats[i * 4 + k] += (dq[k] - qdot * p.q[k]) / p.qNorm;

            // T = J W -> J entries -> camera-space mean.
            const float *R = cam.R;
            const float dJ00 = dT0[0] * R[0] + dT0[1] * R[1] + dT0[2] * R[2];
            const float dJ02 = dT0[0] * R[6] + dT0[1] * R[7] + dT0[2] * R[8];
            const float dJ11 = dT1[0] * R[3] + dT1[1] * R[4] + dT1[2] * R[5];
            const float dJ12 = dT1[0] * R[6] + dT1[1] * R[7] + dT1[2] * R[8];
            const float z2 = z * z, z3 = z2 * z;
            float dpc[3] = {0, 0, 0};
            dpc[2] += dJ00 * -cam.fx / z2 + dJ02 * 2 * cam.fx * p.tx / z3
                      + dJ11 * -cam.fy / z2 + dJ12 * 2 * cam.fy * p.ty / z3;
            const float dtx = dJ02 * -cam.fx / z2;
            const float dty = dJ12 * -cam.fy / z2;
            if (p.clampX)
                dpc[2] += dtx * p.limTx;
            else
                dpc[0] += dtx;
            if (p.clampY)
                dpc[2] += dty * p.limTy;
            else
                dpc[1] += dty;
            // Projected mean.
            dpc[0] += dmx * cam.fx / z;
            dpc[1] += dmy * cam.fy / z;
            dpc[2] += -dmx * cam.fx * p.pc[0] / z2 - dmy * cam.fy * p.pc[1] / z2;

            // p_cam = R p_world + t
            for (int k = 0; k < 3; ++k)
                grads.means[i * 3 + k] += R[k] * dpc[0] + R[3 + k] * dpc[1] + R[6 + k] * dpc[2];
        }
    }, 256);
}

} // namespace u3d
