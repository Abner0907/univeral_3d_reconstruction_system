#include "u3d/sfm/BundleAdjustment.h"

#include <Eigen/Cholesky>
#include <Eigen/Geometry>

#include <cmath>
#include <limits>

namespace u3d {

namespace {

Eigen::Matrix3d skew(const Eigen::Vector3d &v)
{
    Eigen::Matrix3d m;
    m << 0, -v.z(), v.y(), v.z(), 0, -v.x(), -v.y(), v.x(), 0;
    return m;
}

double huberCost(double r2, double delta)
{
    const double r = std::sqrt(r2);
    return r <= delta ? 0.5 * r2 : delta * (r - 0.5 * delta);
}

struct Eval {
    double robustCost = 0;
    double sq = 0;
    int count = 0;
};

Eval evaluate(const BAProblem &p, double huber)
{
    Eval e;
    for (const auto &o : p.observations) {
        const Eigen::Vector3d pc = p.R[std::size_t(o.camera)] * p.points[std::size_t(o.point)] + p.t[std::size_t(o.camera)];
        if (pc.z() <= 1e-9) {
            e.robustCost += huber * 50.0; // strongly discourage points behind cameras
            continue;
        }
        const double f = p.focalFactor * p.sizeScale[std::size_t(o.camera)];
        const double du = f * pc.x() / pc.z() + p.cx[std::size_t(o.camera)] - o.u;
        const double dv = f * pc.y() / pc.z() + p.cy[std::size_t(o.camera)] - o.v;
        const double r2 = du * du + dv * dv;
        e.robustCost += huberCost(r2, huber);
        e.sq += r2;
        ++e.count;
    }
    return e;
}

} // namespace

double reprojectionError(const BAProblem &p, const BAProblem::Observation &o)
{
    const Eigen::Vector3d pc = p.R[std::size_t(o.camera)] * p.points[std::size_t(o.point)] + p.t[std::size_t(o.camera)];
    if (pc.z() <= 1e-9)
        return std::numeric_limits<double>::infinity();
    const double f = p.focalFactor * p.sizeScale[std::size_t(o.camera)];
    const double du = f * pc.x() / pc.z() + p.cx[std::size_t(o.camera)] - o.u;
    const double dv = f * pc.y() / pc.z() + p.cy[std::size_t(o.camera)] - o.v;
    return std::sqrt(du * du + dv * dv);
}

BASummary bundleAdjust(BAProblem &p, const BAOptions &opt)
{
    BASummary summary;
    const int nc = int(p.R.size());
    const int np = int(p.points.size());

    // Parameter layout of the reduced (camera) system.
    std::vector<int> camIndex(std::size_t(nc), -1);
    int dim = 0;
    for (int c = 0; c < nc; ++c)
        if (!p.fixedCamera[std::size_t(c)]) {
            camIndex[std::size_t(c)] = dim;
            dim += 6;
        }
    const int focalIndex = p.refineFocal ? dim++ : -1;

    std::vector<std::vector<int>> obsOfPoint(static_cast<std::size_t>(np));
    for (int k = 0; k < int(p.observations.size()); ++k)
        obsOfPoint[std::size_t(p.observations[std::size_t(k)].point)].push_back(k);

    Eval current = evaluate(p, opt.huberPixels);
    summary.initialRms = current.count ? std::sqrt(current.sq / current.count) : 0;
    summary.finalRms = summary.initialRms;
    if (dim == 0 && np == 0)
        return summary;

    double lambda = 1e-4;
    const std::size_t nObs = p.observations.size();
    // Per observation: Jacobian wrt camera params (6 + focal) and point (3), residual, weight.
    std::vector<Eigen::Matrix<double, 2, 7>> Jc(nObs);
    std::vector<Eigen::Matrix<double, 2, 3>> Jp(nObs);
    std::vector<Eigen::Vector2d> res(nObs);
    std::vector<double> weight(nObs);

    for (int iter = 0; iter < opt.maxIterations; ++iter) {
        summary.iterations = iter + 1;
        // --- linearise
        for (std::size_t k = 0; k < nObs; ++k) {
            const auto &o = p.observations[k];
            const std::size_t c = std::size_t(o.camera);
            const Eigen::Vector3d RX = p.R[c] * p.points[std::size_t(o.point)];
            const Eigen::Vector3d pc = RX + p.t[c];
            if (pc.z() <= 1e-9) {
                weight[k] = 0;
                continue;
            }
            const double s = p.sizeScale[c];
            const double f = p.focalFactor * s;
            const double iz = 1.0 / pc.z();
            res[k] << f * pc.x() * iz + p.cx[c] - o.u, f * pc.y() * iz + p.cy[c] - o.v;
            Eigen::Matrix<double, 2, 3> dProj;
            dProj << f * iz, 0, -f * pc.x() * iz * iz, 0, f * iz, -f * pc.y() * iz * iz;
            Jc[k].block<2, 3>(0, 0) = dProj * -skew(RX);
            Jc[k].block<2, 3>(0, 3) = dProj;
            Jc[k](0, 6) = s * pc.x() * iz;
            Jc[k](1, 6) = s * pc.y() * iz;
            Jp[k] = dProj * p.R[c];
            const double r = res[k].norm();
            weight[k] = r <= opt.huberPixels ? 1.0 : opt.huberPixels / r;
        }

        // Index of each of an observation's 7 camera-side parameters in the reduced system.
        auto paramIndex = [&](std::size_t k, int j) -> int {
            if (j == 6)
                return focalIndex;
            const int base = camIndex[std::size_t(p.observations[k].camera)];
            return base < 0 ? -1 : base + j;
        };

        bool improved = false;
        for (int attempt = 0; attempt < 10 && !improved; ++attempt) {
            Eigen::MatrixXd S = Eigen::MatrixXd::Zero(dim, dim);
            Eigen::VectorXd rhs = Eigen::VectorXd::Zero(dim);

            // Camera-camera blocks and camera gradient.
            for (std::size_t k = 0; k < nObs; ++k) {
                if (weight[k] == 0)
                    continue;
                const Eigen::Matrix<double, 7, 7> H = weight[k] * Jc[k].transpose() * Jc[k];
                const Eigen::Matrix<double, 7, 1> g = -weight[k] * Jc[k].transpose() * res[k];
                for (int a = 0; a < 7; ++a) {
                    const int ia = paramIndex(k, a);
                    if (ia < 0)
                        continue;
                    rhs(ia) += g(a);
                    for (int b = 0; b < 7; ++b) {
                        const int ib = paramIndex(k, b);
                        if (ib >= 0)
                            S(ia, ib) += H(a, b);
                    }
                }
            }
            // Marquardt damping on the camera diagonal.
            for (int d = 0; d < dim; ++d)
                S(d, d) += lambda * S(d, d) + 1e-9;

            // Eliminate points.
            std::vector<Eigen::Matrix3d> Vinv(static_cast<std::size_t>(np));
            std::vector<Eigen::Vector3d> bp(static_cast<std::size_t>(np));
            for (int pt = 0; pt < np; ++pt) {
                Eigen::Matrix3d V = Eigen::Matrix3d::Zero();
                Eigen::Vector3d b = Eigen::Vector3d::Zero();
                for (int k : obsOfPoint[std::size_t(pt)]) {
                    if (weight[std::size_t(k)] == 0)
                        continue;
                    V += weight[std::size_t(k)] * Jp[std::size_t(k)].transpose() * Jp[std::size_t(k)];
                    b += -weight[std::size_t(k)] * Jp[std::size_t(k)].transpose() * res[std::size_t(k)];
                }
                for (int d = 0; d < 3; ++d)
                    V(d, d) += lambda * V(d, d) + 1e-9;
                Vinv[std::size_t(pt)] = V.inverse();
                bp[std::size_t(pt)] = b;

                const auto &list = obsOfPoint[std::size_t(pt)];
                for (int ka : list) {
                    if (weight[std::size_t(ka)] == 0)
                        continue;
                    const Eigen::Matrix<double, 7, 3> Wa =
                        weight[std::size_t(ka)] * Jc[std::size_t(ka)].transpose() * Jp[std::size_t(ka)];
                    const Eigen::Matrix<double, 7, 3> WaVinv = Wa * Vinv[std::size_t(pt)];
                    const Eigen::Matrix<double, 7, 1> r = WaVinv * b;
                    for (int a = 0; a < 7; ++a) {
                        const int ia = paramIndex(std::size_t(ka), a);
                        if (ia >= 0)
                            rhs(ia) -= r(a);
                    }
                    for (int kb : list) {
                        if (weight[std::size_t(kb)] == 0)
                            continue;
                        const Eigen::Matrix<double, 7, 3> Wb =
                            weight[std::size_t(kb)] * Jc[std::size_t(kb)].transpose() * Jp[std::size_t(kb)];
                        const Eigen::Matrix<double, 7, 7> block = WaVinv * Wb.transpose();
                        for (int a = 0; a < 7; ++a) {
                            const int ia = paramIndex(std::size_t(ka), a);
                            if (ia < 0)
                                continue;
                            for (int bb = 0; bb < 7; ++bb) {
                                const int ib = paramIndex(std::size_t(kb), bb);
                                if (ib >= 0)
                                    S(ia, ib) -= block(a, bb);
                            }
                        }
                    }
                }
            }

            const Eigen::VectorXd dc = dim > 0 ? Eigen::VectorXd(S.ldlt().solve(rhs)) : Eigen::VectorXd();
            if (dim > 0 && !dc.allFinite()) {
                lambda *= 10;
                continue;
            }

            // Candidate update.
            BAProblem candidate = p;
            for (int c = 0; c < nc; ++c) {
                const int base = camIndex[std::size_t(c)];
                if (base < 0)
                    continue;
                const Eigen::Vector3d dtheta = dc.segment<3>(base);
                const double angle = dtheta.norm();
                if (angle > 1e-15)
                    candidate.R[std::size_t(c)] = Eigen::AngleAxisd(angle, dtheta / angle).toRotationMatrix() * p.R[std::size_t(c)];
                candidate.t[std::size_t(c)] = p.t[std::size_t(c)] + dc.segment<3>(base + 3);
            }
            if (focalIndex >= 0)
                candidate.focalFactor = p.focalFactor + dc(focalIndex);
            for (int pt = 0; pt < np; ++pt) {
                Eigen::Vector3d b = bp[std::size_t(pt)];
                for (int k : obsOfPoint[std::size_t(pt)]) {
                    if (weight[std::size_t(k)] == 0)
                        continue;
                    const Eigen::Matrix<double, 7, 3> Wk =
                        weight[std::size_t(k)] * Jc[std::size_t(k)].transpose() * Jp[std::size_t(k)];
                    for (int a = 0; a < 7; ++a) {
                        const int ia = paramIndex(std::size_t(k), a);
                        if (ia >= 0)
                            b -= Wk.row(a).transpose() * dc(ia);
                    }
                }
                candidate.points[std::size_t(pt)] = p.points[std::size_t(pt)] + Vinv[std::size_t(pt)] * b;
            }

            const Eval next = evaluate(candidate, opt.huberPixels);
            if (next.robustCost < current.robustCost && candidate.focalFactor > 0.05) {
                const double relDecrease = (current.robustCost - next.robustCost) / std::max(current.robustCost, 1e-12);
                p = std::move(candidate);
                current = next;
                lambda = std::max(lambda / 3.0, 1e-10);
                improved = true;
                if (relDecrease < 1e-7)
                    iter = opt.maxIterations; // converged
            } else {
                lambda *= 4.0;
            }
        }
        if (!improved)
            break;
    }
    summary.finalRms = current.count ? std::sqrt(current.sq / current.count) : 0;
    return summary;
}

} // namespace u3d
