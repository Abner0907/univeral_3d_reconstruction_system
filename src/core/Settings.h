#pragma once

#include <QString>

class QSettings;

namespace u3d {

// Runtime configuration shared by the desktop app and the headless server.
struct Settings {
    // Where job inputs, intermediate data and results are stored.
    QString dataDir;
    // Folder containing the Nerfstudio executables (ns-train, ...), e.g. a conda env's bin/.
    // Empty means "find them on PATH".
    QString nsBinDir;
    // Use the bundled Nerfstudio simulator instead of the real tools (no GPU required).
    bool dryRun = false;
    // Path to u3d_fake_ns; empty means "next to the running executable".
    QString fakeNsPath;
    // Value for `ns-train --vis` ("viewer", "tensorboard", "viewer+tensorboard").
    QString vis = QStringLiteral("viewer");
    // Port of the live Nerfstudio web viewer while a job is training.
    int viewerPort = 7007;
    QString defaultMethod = QStringLiteral("splatfacto");
    int defaultIterations = 30000;
    // Frames extracted when the input is a video.
    int videoFramesTarget = 300;
    // COLMAP matching passed to ns-process-data: vocab_tree, exhaustive or sequential.
    QString matchingMethod = QStringLiteral("vocab_tree");
    // Port of the HTTP API the Android app uploads to.
    int serverPort = 8000;

    static Settings defaults();
    static Settings load(QSettings &store);
    void save(QSettings &store) const;
    // Overrides from U3D_* environment variables (U3D_DATA_DIR, U3D_NS_BIN_DIR, U3D_DRY_RUN, ...).
    void applyEnvironment();
    // Resolved path of the simulator executable.
    QString resolvedFakeNsPath() const;
};

} // namespace u3d
