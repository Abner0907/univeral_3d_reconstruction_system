#include "u3d/Camera.h"

namespace u3d {

std::array<double, 3> Camera::center() const
{
    return {-(R[0] * t[0] + R[3] * t[1] + R[6] * t[2]),
            -(R[1] * t[0] + R[4] * t[1] + R[7] * t[2]),
            -(R[2] * t[0] + R[5] * t[1] + R[8] * t[2])};
}

Camera Camera::scaled(double scale) const
{
    Camera c = *this;
    c.width = int(width * scale + 0.5);
    c.height = int(height * scale + 0.5);
    // Pixel centres: continuous coordinate x maps to (x + 0.5) * s - 0.5.
    c.fx = fx * scale;
    c.fy = fy * scale;
    c.cx = (cx + 0.5) * scale - 0.5;
    c.cy = (cy + 0.5) * scale - 0.5;
    return c;
}

bool Camera::project(const std::array<double, 3> &w, double &u, double &v) const
{
    const double x = R[0] * w[0] + R[1] * w[1] + R[2] * w[2] + t[0];
    const double y = R[3] * w[0] + R[4] * w[1] + R[5] * w[2] + t[1];
    const double z = R[6] * w[0] + R[7] * w[1] + R[8] * w[2] + t[2];
    if (z <= 1e-9)
        return false;
    u = fx * x / z + cx;
    v = fy * y / z + cy;
    return true;
}

} // namespace u3d
