#pragma once

#include <Eigen/Core>
#include <vector>

namespace u3d {

// Sparse bundle adjustment: Levenberg-Marquardt over camera poses, 3D points and (optionally)
// one shared focal-length factor, with a Huber loss. The point blocks are eliminated with the
// Schur complement and the reduced camera system is solved densely, which is fast for the
// few hundred images of a phone capture.
//
// Camera i projects   p = R_i X + t_i,  u = f_i p.x / p.z + cx_i,  v = f_i p.y / p.z + cy_i
// with f_i = focalFactor * sizeScale_i (sizeScale = max(width, height)).
struct BAProblem {
    struct Observation {
        int camera;
        int point;
        double u, v;
    };

    std::vector<Eigen::Matrix3d> R;
    std::vector<Eigen::Vector3d> t;
    std::vector<double> sizeScale;
    std::vector<double> cx, cy;
    std::vector<bool> fixedCamera; // gauge: keep at least one camera fixed
    std::vector<Eigen::Vector3d> points;
    std::vector<Observation> observations;
    double focalFactor = 1.2;
    bool refineFocal = false;
};

struct BAOptions {
    int maxIterations = 30;
    double huberPixels = 2.0;
};

struct BASummary {
    double initialRms = 0; // reprojection RMS in pixels (non-robust), before
    double finalRms = 0;   // ... and after
    int iterations = 0;
};

BASummary bundleAdjust(BAProblem &problem, const BAOptions &options = BAOptions());

// Reprojection error (pixels) of one observation; +inf if the point is behind the camera.
double reprojectionError(const BAProblem &problem, const BAProblem::Observation &obs);

} // namespace u3d
