#include "u3d/sfm/IncrementalSfM.h"

#include "u3d/sfm/BundleAdjustment.h"

#include <Eigen/Geometry>
#include <Eigen/SVD>
#include <opencv2/calib3d.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numeric>
#include <unordered_map>

namespace u3d {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Track {
    std::vector<std::pair<int, int>> obs; // (image, keypoint)
    std::vector<char> inlier;             // per observation, valid when has3D
    bool has3D = false;
    Eigen::Vector3d X = Eigen::Vector3d::Zero();
};

// Union-find over keypoints that refuses merges which would put two keypoints of the same
// image into one track. Plain union-find lets a handful of wrong matches chain thousands of
// tracks into a few inconsistent giants.
class TrackBuilder
{
public:
    TrackBuilder(std::size_t n, std::vector<int> imageOfNode)
        : m_parent(n), m_images(n), m_imageOfNode(std::move(imageOfNode))
    {
        std::iota(m_parent.begin(), m_parent.end(), 0);
    }
    std::size_t find(std::size_t x)
    {
        while (m_parent[x] != x)
            x = m_parent[x] = m_parent[m_parent[x]];
        return x;
    }
    void unite(std::size_t a, std::size_t b)
    {
        std::size_t ra = find(a), rb = find(b);
        if (ra == rb)
            return;
        std::vector<int> &ia = images(ra), &ib = images(rb);
        // Sorted image lists: reject the merge if they intersect.
        std::size_t x = 0, y = 0;
        while (x < ia.size() && y < ib.size()) {
            if (ia[x] == ib[y])
                return;
            if (ia[x] < ib[y]) ++x; else ++y;
        }
        if (ia.size() < ib.size())
            std::swap(ra, rb);
        std::vector<int> merged;
        std::merge(images(ra).begin(), images(ra).end(), images(rb).begin(), images(rb).end(), std::back_inserter(merged));
        m_images[ra] = std::move(merged);
        m_images[rb].clear();
        m_images[rb].shrink_to_fit();
        m_parent[rb] = ra;
    }

private:
    std::vector<int> &images(std::size_t root)
    {
        if (m_images[root].empty())
            m_images[root].push_back(m_imageOfNode[root]);
        return m_images[root];
    }
    std::vector<std::size_t> m_parent;
    std::vector<std::vector<int>> m_images;
    std::vector<int> m_imageOfNode;
};

class Sfm
{
public:
    Sfm(const SfmInput &in, const SfmOptions &opt) : m_in(in), m_opt(opt) {}

    bool run(SfmResult &result, const SfmProgress &progress, const std::atomic_bool *cancel, std::string *error);

private:
    int numImages() const { return int(m_in.widths.size()); }
    double sizeScale(int i) const { return double(std::max(m_in.widths[std::size_t(i)], m_in.heights[std::size_t(i)])); }
    double focal(int i) const { return m_k * sizeScale(i); }
    double cx(int i) const { return 0.5 * (m_in.widths[std::size_t(i)] - 1); }
    double cy(int i) const { return 0.5 * (m_in.heights[std::size_t(i)] - 1); }
    Eigen::Vector2d pixel(int img, int kp) const
    {
        const cv::Point2f &p = m_in.points[std::size_t(img)][std::size_t(kp)];
        return {p.x, p.y};
    }
    Eigen::Vector3d ray(int img, int kp) const // normalised camera coordinates (z = 1)
    {
        const Eigen::Vector2d p = pixel(img, kp);
        return {(p.x() - cx(img)) / focal(img), (p.y() - cy(img)) / focal(img), 1.0};
    }
    double reprojError(int img, int kp, const Eigen::Vector3d &X, double *depth = nullptr) const
    {
        const Eigen::Vector3d pc = m_R[std::size_t(img)] * X + m_t[std::size_t(img)];
        if (depth)
            *depth = pc.z();
        if (pc.z() <= 1e-9)
            return std::numeric_limits<double>::infinity();
        const Eigen::Vector2d proj(focal(img) * pc.x() / pc.z() + cx(img), focal(img) * pc.y() / pc.z() + cy(img));
        return (proj - pixel(img, kp)).norm();
    }
    Eigen::Vector3d center(int img) const { return -m_R[std::size_t(img)].transpose() * m_t[std::size_t(img)]; }

    void buildTracks();
    bool initialize();
    static Eigen::Vector3d triangulateDlt(const Eigen::Matrix3d &R1, const Eigen::Vector3d &t1, const Eigen::Vector3d &x1,
                                          const Eigen::Matrix3d &R2, const Eigen::Vector3d &t2, const Eigen::Vector3d &x2);
    bool triangulateTrack(Track &track);
    int countObserved3D(int img) const;
    bool registerImage(int img);
    void triangulateImage(int img);
    void globalBundleAdjustment(bool refineFocal, int iterations);
    void updateInliers(double threshold);
    void filterOutliers();

    const SfmInput &m_in;
    SfmOptions m_opt;
    double m_k = 1.2;
    std::vector<std::vector<int>> m_trackOf;
    std::vector<Track> m_tracks;
    std::vector<bool> m_reg;
    std::vector<Eigen::Matrix3d> m_R;
    std::vector<Eigen::Vector3d> m_t;
    int m_firstCamera = -1;
    int m_numRegistered = 0;
};

void Sfm::buildTracks()
{
    const int n = numImages();
    std::vector<std::size_t> offset(std::size_t(n) + 1, 0);
    for (int i = 0; i < n; ++i)
        offset[std::size_t(i) + 1] = offset[std::size_t(i)] + m_in.points[std::size_t(i)].size();
    std::vector<int> imageOfNode(offset.back());
    for (int i = 0; i < n; ++i)
        std::fill(imageOfNode.begin() + std::ptrdiff_t(offset[std::size_t(i)]),
                  imageOfNode.begin() + std::ptrdiff_t(offset[std::size_t(i) + 1]), i);
    TrackBuilder uf(offset.back(), imageOfNode);
    std::vector<char> matched(offset.back(), 0);
    // Most reliable (largest) pairs first, so their matches win any conflict.
    std::vector<const ImagePair *> ordered;
    for (const ImagePair &p : m_in.pairs)
        ordered.push_back(&p);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const ImagePair *a, const ImagePair *b) { return a->matches.size() > b->matches.size(); });
    for (const ImagePair *pp : ordered)
        for (const auto &m : pp->matches) {
            const ImagePair &p = *pp;
            const std::size_t a = offset[std::size_t(p.i)] + std::size_t(m.first);
            const std::size_t b = offset[std::size_t(p.j)] + std::size_t(m.second);
            uf.unite(a, b);
            matched[a] = matched[b] = 1;
        }
    std::unordered_map<std::size_t, std::vector<std::size_t>> groups;
    for (std::size_t node = 0; node < offset.back(); ++node)
        if (matched[node])
            groups[uf.find(node)].push_back(node);

    m_trackOf.assign(std::size_t(n), {});
    for (int i = 0; i < n; ++i)
        m_trackOf[std::size_t(i)].assign(m_in.points[std::size_t(i)].size(), -1);
    for (auto &g : groups) {
        if (g.second.size() < 2)
            continue;
        Track track;
        std::vector<int> seen;
        bool conflict = false;
        for (std::size_t node : g.second) {
            const int img = int(std::upper_bound(offset.begin(), offset.end(), node) - offset.begin()) - 1;
            if (std::find(seen.begin(), seen.end(), img) != seen.end()) {
                conflict = true; // two keypoints of one image in the same track: unreliable
                break;
            }
            seen.push_back(img);
            track.obs.push_back({img, int(node - offset[std::size_t(img)])});
        }
        if (conflict)
            continue;
        const int id = int(m_tracks.size());
        for (const auto &o : track.obs)
            m_trackOf[std::size_t(o.first)][std::size_t(o.second)] = id;
        m_tracks.push_back(std::move(track));
    }
}

Eigen::Vector3d Sfm::triangulateDlt(const Eigen::Matrix3d &R1, const Eigen::Vector3d &t1, const Eigen::Vector3d &x1,
                                    const Eigen::Matrix3d &R2, const Eigen::Vector3d &t2, const Eigen::Vector3d &x2)
{
    Eigen::Matrix<double, 3, 4> P1, P2;
    P1 << R1, t1;
    P2 << R2, t2;
    Eigen::Matrix4d A;
    A.row(0) = x1.x() * P1.row(2) - P1.row(0);
    A.row(1) = x1.y() * P1.row(2) - P1.row(1);
    A.row(2) = x2.x() * P2.row(2) - P2.row(0);
    A.row(3) = x2.y() * P2.row(2) - P2.row(1);
    const Eigen::JacobiSVD<Eigen::Matrix4d> svd(A, Eigen::ComputeFullV);
    const Eigen::Vector4d h = svd.matrixV().col(3);
    return h.head<3>() / h(3);
}

bool Sfm::triangulateTrack(Track &track)
{
    std::vector<int> regObs;
    for (int k = 0; k < int(track.obs.size()); ++k)
        if (m_reg[std::size_t(track.obs[std::size_t(k)].first)])
            regObs.push_back(k);
    if (regObs.size() < 2)
        return false;

    // Pick the observation pair with the widest ray angle.
    double bestCos = 2;
    int ba = -1, bb = -1;
    const std::size_t m = std::min<std::size_t>(regObs.size(), 12);
    for (std::size_t a = 0; a < m; ++a)
        for (std::size_t b = a + 1; b < m; ++b) {
            const auto &oa = track.obs[std::size_t(regObs[a])], &ob = track.obs[std::size_t(regObs[b])];
            const Eigen::Vector3d da = (m_R[std::size_t(oa.first)].transpose() * ray(oa.first, oa.second)).normalized();
            const Eigen::Vector3d db = (m_R[std::size_t(ob.first)].transpose() * ray(ob.first, ob.second)).normalized();
            const double c = da.dot(db);
            if (c < bestCos) {
                bestCos = c;
                ba = regObs[a];
                bb = regObs[b];
            }
        }
    const auto &oa = track.obs[std::size_t(ba)], &ob = track.obs[std::size_t(bb)];
    const Eigen::Vector3d X = triangulateDlt(m_R[std::size_t(oa.first)], m_t[std::size_t(oa.first)], ray(oa.first, oa.second),
                                             m_R[std::size_t(ob.first)], m_t[std::size_t(ob.first)], ray(ob.first, ob.second));
    if (!X.allFinite())
        return false;
    const Eigen::Vector3d ra = (X - center(oa.first)).normalized(), rb = (X - center(ob.first)).normalized();
    if (std::acos(std::clamp(ra.dot(rb), -1.0, 1.0)) * 180.0 / kPi < m_opt.minTriangulationAngle)
        return false;

    track.inlier.assign(track.obs.size(), 0);
    int inliers = 0;
    for (int k : regObs) {
        double depth = 0;
        const double err = reprojError(track.obs[std::size_t(k)].first, track.obs[std::size_t(k)].second, X, &depth);
        if (depth > 0 && err <= m_opt.maxReprojError) {
            track.inlier[std::size_t(k)] = 1;
            ++inliers;
        }
    }
    if (inliers < 2)
        return false;
    track.X = X;
    track.has3D = true;
    return true;
}

bool Sfm::initialize()
{
    std::vector<const ImagePair *> pairs;
    for (const ImagePair &p : m_in.pairs)
        pairs.push_back(&p);
    std::sort(pairs.begin(), pairs.end(),
              [](const ImagePair *a, const ImagePair *b) { return a->matches.size() > b->matches.size(); });
    if (pairs.size() > 40)
        pairs.resize(40);

    struct Candidate {
        int i = -1, j = -1;
        Eigen::Matrix3d R;
        Eigen::Vector3d t;
        int good = 0;
        double medianAngle = 0;
    };
    Candidate best;
    double bestScore = -1;
    for (const ImagePair *p : pairs) {
        std::vector<cv::Point2d> a, b;
        for (const auto &m : p->matches) {
            const Eigen::Vector3d ra = ray(p->i, m.first), rb = ray(p->j, m.second);
            a.emplace_back(ra.x(), ra.y());
            b.emplace_back(rb.x(), rb.y());
        }
        const double thr = m_opt.maxReprojError / std::max(focal(p->i), focal(p->j));
        cv::Mat mask;
        const cv::Mat E = cv::findEssentialMat(a, b, 1.0, cv::Point2d(0, 0), cv::RANSAC, 0.999, thr, mask);
        if (E.rows != 3 || E.cols != 3)
            continue;
        cv::Mat Rcv, tcv;
        cv::recoverPose(E, a, b, Rcv, tcv, 1.0, cv::Point2d(0, 0), mask);
        Eigen::Matrix3d R;
        Eigen::Vector3d t;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c)
                R(r, c) = Rcv.at<double>(r, c);
            t(r) = tcv.at<double>(r);
        }
        // Count well-conditioned triangulations.
        std::vector<double> angles;
        int good = 0;
        for (std::size_t k = 0; k < p->matches.size(); ++k) {
            if (!mask.at<unsigned char>(int(k)))
                continue;
            const Eigen::Vector3d x1 = ray(p->i, p->matches[k].first), x2 = ray(p->j, p->matches[k].second);
            const Eigen::Vector3d X = triangulateDlt(Eigen::Matrix3d::Identity(), Eigen::Vector3d::Zero(), x1, R, t, x2);
            const Eigen::Vector3d X2 = R * X + t;
            if (!X.allFinite() || X.z() <= 0 || X2.z() <= 0)
                continue;
            const Eigen::Vector3d c2 = -R.transpose() * t;
            const double angle = std::acos(std::clamp(X.normalized().dot((X - c2).normalized()), -1.0, 1.0)) * 180.0 / kPi;
            angles.push_back(angle);
            if (angle >= m_opt.minTriangulationAngle)
                ++good;
        }
        if (angles.empty())
            continue;
        std::nth_element(angles.begin(), angles.begin() + std::ptrdiff_t(angles.size() / 2), angles.end());
        const double median = angles[angles.size() / 2];
        // Prefer many points with a decent baseline; tiny baselines give unstable scale.
        const double score = double(good) * std::min(1.0, median / 4.0);
        if (good >= 30 && median >= 1.0 && score > bestScore) {
            bestScore = score;
            best = {p->i, p->j, R, t, good, median};
        }
    }
    if (best.i < 0)
        return false;

    m_reg[std::size_t(best.i)] = m_reg[std::size_t(best.j)] = true;
    m_R[std::size_t(best.i)] = Eigen::Matrix3d::Identity();
    m_t[std::size_t(best.i)] = Eigen::Vector3d::Zero();
    m_R[std::size_t(best.j)] = best.R;
    m_t[std::size_t(best.j)] = best.t;
    m_firstCamera = best.i;
    m_numRegistered = 2;
    triangulateImage(best.j);
    globalBundleAdjustment(false, 20);
    filterOutliers();
    return true;
}

int Sfm::countObserved3D(int img) const
{
    int count = 0;
    for (int tid : m_trackOf[std::size_t(img)])
        if (tid >= 0 && m_tracks[std::size_t(tid)].has3D)
            ++count;
    return count;
}

bool Sfm::registerImage(int img)
{
    std::vector<cv::Point3d> object;
    std::vector<cv::Point2d> image;
    for (std::size_t kp = 0; kp < m_trackOf[std::size_t(img)].size(); ++kp) {
        const int tid = m_trackOf[std::size_t(img)][kp];
        if (tid < 0 || !m_tracks[std::size_t(tid)].has3D)
            continue;
        const Eigen::Vector3d &X = m_tracks[std::size_t(tid)].X;
        object.emplace_back(X.x(), X.y(), X.z());
        const Eigen::Vector2d p = pixel(img, int(kp));
        image.emplace_back(p.x(), p.y());
    }
    if (int(object.size()) < m_opt.minPnPInliers)
        return false;
    const cv::Matx33d K(focal(img), 0, cx(img), 0, focal(img), cy(img), 0, 0, 1);
    cv::Mat rvec, tvec;
    std::vector<int> inliers;
    if (!cv::solvePnPRansac(object, image, K, cv::noArray(), rvec, tvec, false, 1000,
                            float(m_opt.maxReprojError), 0.999, inliers, cv::SOLVEPNP_EPNP))
        return false;
    if (int(inliers.size()) < m_opt.minPnPInliers)
        return false;
    std::vector<cv::Point3d> inObj;
    std::vector<cv::Point2d> inImg;
    for (int k : inliers) {
        inObj.push_back(object[std::size_t(k)]);
        inImg.push_back(image[std::size_t(k)]);
    }
    cv::solvePnP(inObj, inImg, K, cv::noArray(), rvec, tvec, true, cv::SOLVEPNP_ITERATIVE);
    cv::Mat Rcv;
    cv::Rodrigues(rvec, Rcv);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c)
            m_R[std::size_t(img)](r, c) = Rcv.at<double>(r, c);
        m_t[std::size_t(img)](r) = tvec.at<double>(r);
    }
    m_reg[std::size_t(img)] = true;
    ++m_numRegistered;
    return true;
}

void Sfm::triangulateImage(int img)
{
    for (int tid : m_trackOf[std::size_t(img)])
        if (tid >= 0 && !m_tracks[std::size_t(tid)].has3D)
            triangulateTrack(m_tracks[std::size_t(tid)]);
}

void Sfm::updateInliers(double threshold)
{
    for (Track &t : m_tracks) {
        if (!t.has3D)
            continue;
        t.inlier.assign(t.obs.size(), 0);
        for (std::size_t k = 0; k < t.obs.size(); ++k) {
            if (!m_reg[std::size_t(t.obs[k].first)])
                continue;
            double depth = 0;
            const double err = reprojError(t.obs[k].first, t.obs[k].second, t.X, &depth);
            t.inlier[k] = depth > 0 && err <= threshold;
        }
    }
}

void Sfm::globalBundleAdjustment(bool refineFocal, int iterations)
{
    updateInliers(2.0 * m_opt.maxReprojError);
    BAProblem p;
    std::vector<int> camOf(std::size_t(numImages()), -1);
    for (int i = 0; i < numImages(); ++i) {
        if (!m_reg[std::size_t(i)])
            continue;
        camOf[std::size_t(i)] = int(p.R.size());
        p.R.push_back(m_R[std::size_t(i)]);
        p.t.push_back(m_t[std::size_t(i)]);
        p.sizeScale.push_back(sizeScale(i));
        p.cx.push_back(cx(i));
        p.cy.push_back(cy(i));
        p.fixedCamera.push_back(i == m_firstCamera);
    }
    std::vector<int> trackIds;
    for (int tid = 0; tid < int(m_tracks.size()); ++tid) {
        const Track &t = m_tracks[std::size_t(tid)];
        if (!t.has3D)
            continue;
        const int pid = int(p.points.size());
        int count = 0;
        for (std::size_t k = 0; k < t.obs.size(); ++k)
            if (t.inlier[k]) {
                const Eigen::Vector2d px = pixel(t.obs[k].first, t.obs[k].second);
                p.observations.push_back({camOf[std::size_t(t.obs[k].first)], pid, px.x(), px.y()});
                ++count;
            }
        if (count == 0)
            continue;
        p.points.push_back(t.X);
        trackIds.push_back(tid);
    }
    p.focalFactor = m_k;
    p.refineFocal = refineFocal;
    BAOptions opt;
    opt.maxIterations = iterations;
    bundleAdjust(p, opt);

    for (int i = 0; i < numImages(); ++i)
        if (camOf[std::size_t(i)] >= 0) {
            m_R[std::size_t(i)] = p.R[std::size_t(camOf[std::size_t(i)])];
            m_t[std::size_t(i)] = p.t[std::size_t(camOf[std::size_t(i)])];
        }
    for (std::size_t k = 0; k < trackIds.size(); ++k)
        m_tracks[std::size_t(trackIds[k])].X = p.points[k];
    m_k = p.focalFactor;
}

void Sfm::filterOutliers()
{
    updateInliers(m_opt.maxReprojError);
    for (Track &t : m_tracks) {
        if (!t.has3D)
            continue;
        int inliers = 0;
        double maxAngle = 0;
        std::vector<Eigen::Vector3d> dirs;
        for (std::size_t k = 0; k < t.obs.size(); ++k)
            if (t.inlier[k]) {
                ++inliers;
                dirs.push_back((t.X - center(t.obs[k].first)).normalized());
            }
        for (std::size_t a = 0; a < dirs.size(); ++a)
            for (std::size_t b = a + 1; b < dirs.size(); ++b)
                maxAngle = std::max(maxAngle, std::acos(std::clamp(dirs[a].dot(dirs[b]), -1.0, 1.0)));
        if (inliers < 2 || maxAngle * 180.0 / kPi < m_opt.minTriangulationAngle)
            t.has3D = false;
    }
}

bool Sfm::run(SfmResult &result, const SfmProgress &progress, const std::atomic_bool *cancel, std::string *error)
{
    auto fail = [error](const std::string &msg) {
        if (error)
            *error = msg;
        return false;
    };
    const int n = numImages();
    if (n < 2)
        return fail("At least two images are needed.");
    m_k = m_opt.initialFocalFactor;
    m_reg.assign(std::size_t(n), false);
    m_R.assign(std::size_t(n), Eigen::Matrix3d::Identity());
    m_t.assign(std::size_t(n), Eigen::Vector3d::Zero());

    buildTracks();
    if (m_tracks.size() < 50)
        return fail("Too few feature matches between the photos. Use more overlap and textured surfaces.");
    if (!initialize())
        return fail("Could not find a good initial image pair. Move the camera more between shots "
                    "(rotating in place does not work) and keep good overlap.");
    if (progress)
        progress(double(m_numRegistered) / n, "Initial pair registered");

    std::vector<int> attempts(std::size_t(n), 0);
    int sinceBA = 0;
    for (;;) {
        if (cancel && cancel->load())
            return fail("Cancelled");
        int bestImg = -1, bestCount = 0;
        for (int i = 0; i < n; ++i) {
            if (m_reg[std::size_t(i)] || attempts[std::size_t(i)] >= 3)
                continue;
            const int c = countObserved3D(i);
            if (c > bestCount) {
                bestCount = c;
                bestImg = i;
            }
        }
        if (bestImg < 0 || bestCount < m_opt.minPnPInliers)
            break;
        if (!registerImage(bestImg)) {
            ++attempts[std::size_t(bestImg)];
            continue;
        }
        triangulateImage(bestImg);
        if (++sinceBA >= (m_numRegistered < 10 ? 1 : m_opt.baInterval)) {
            globalBundleAdjustment(m_opt.refineFocal && m_numRegistered >= 6, 12);
            filterOutliers();
            for (Track &t : m_tracks)
                if (!t.has3D)
                    triangulateTrack(t);
            sinceBA = 0;
            // Give previously failed images another chance with the improved model.
            std::fill(attempts.begin(), attempts.end(), 0);
        }
        if (progress)
            progress(double(m_numRegistered) / n,
                     "Registered " + std::to_string(m_numRegistered) + " of " + std::to_string(n) + " images");
    }

    // Final refinement.
    for (int round = 0; round < 2; ++round) {
        globalBundleAdjustment(m_opt.refineFocal && m_numRegistered >= 3, 40);
        filterOutliers();
        for (Track &t : m_tracks)
            if (!t.has3D)
                triangulateTrack(t);
    }
    globalBundleAdjustment(m_opt.refineFocal && m_numRegistered >= 3, 40);
    filterOutliers();

    // Output.
    result = SfmResult();
    result.focalFactor = m_k;
    result.registered = m_reg;
    result.numRegistered = m_numRegistered;
    result.cameras.resize(std::size_t(n));
    for (int i = 0; i < n; ++i) {
        Camera &c = result.cameras[std::size_t(i)];
        c.width = m_in.widths[std::size_t(i)];
        c.height = m_in.heights[std::size_t(i)];
        c.fx = c.fy = focal(i);
        c.cx = cx(i);
        c.cy = cy(i);
        for (int r = 0; r < 3; ++r) {
            for (int col = 0; col < 3; ++col)
                c.R[std::size_t(r * 3 + col)] = m_R[std::size_t(i)](r, col);
            c.t[std::size_t(r)] = m_t[std::size_t(i)](r);
        }
    }
    double sq = 0;
    std::size_t count = 0;
    for (const Track &t : m_tracks) {
        if (!t.has3D)
            continue;
        double rgb[3] = {0, 0, 0};
        int nIn = 0;
        for (std::size_t k = 0; k < t.obs.size(); ++k) {
            if (!t.inlier[k])
                continue;
            const auto &c = m_in.colors[std::size_t(t.obs[k].first)][std::size_t(t.obs[k].second)];
            for (int ch = 0; ch < 3; ++ch)
                rgb[ch] += c[std::size_t(ch)];
            const double e = reprojError(t.obs[k].first, t.obs[k].second, t.X);
            sq += e * e;
            ++count;
            ++nIn;
        }
        for (int k = 0; k < 3; ++k) {
            result.pointsXyz.push_back(float(t.X(k)));
            result.pointsRgb.push_back(float(rgb[k] / (255.0 * std::max(1, nIn))));
        }
    }
    result.rmsError = count ? std::sqrt(sq / double(count)) : 0;
    if (m_numRegistered < 3)
        return fail("Only " + std::to_string(m_numRegistered) + " images could be registered.");
    return true;
}

} // namespace

bool runIncrementalSfm(const SfmInput &input, const SfmOptions &options, SfmResult &result,
                       const SfmProgress &progress, const std::atomic_bool *cancel, std::string *error)
{
    Sfm sfm(input, options);
    return sfm.run(result, progress, cancel, error);
}

} // namespace u3d
