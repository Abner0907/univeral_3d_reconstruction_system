#pragma once

#include "Settings.h"

#include <QPair>
#include <QString>
#include <QStringList>

// Builds the Nerfstudio command pipeline for a reconstruction job:
//
//   1. ns-process-data images|video          -> COLMAP camera poses     (processed/)
//   2. ns-train <method>                     -> trained model           (outputs/)
//      splatfacto = 3D Gaussian Splatting rasterised by gsplat; nerfacto = NeRF
//   3. ns-export gaussian-splat|pointcloud   -> result .ply              (exports/)
namespace u3d::pipeline {

struct Command {
    QString program;
    QStringList arguments;
    QString toString() const;
};

enum class Stage { Prepare, ProcessData, Train, Export };

QString stageName(Stage stage);
QString stageDescription(Stage stage, const QString &method);
// Portion of the overall job progress covered by a stage: [first, second).
QPair<double, double> stageRange(Stage stage);

QStringList methods();
QStringList splatMethods();
bool isValidMethod(const QString &method);
bool isSplatMethod(const QString &method);

bool isImageFile(const QString &fileName);
bool isVideoFile(const QString &fileName);
// Returns "images" or "video", or an empty string and sets *error.
QString classifyInputs(const QStringList &fileNames, QString *error);
// Makes an uploaded file name safe to store (no directories or odd characters).
QString sanitizeFileName(const QString &name, const QString &fallback);

QString stripAnsi(const QString &text);
// Training progress (0..1) from an `ns-train` console line, or -1 if the line has none.
double parseTrainProgress(const QString &line);

struct JobPaths {
    QString root;
    QString input() const { return root + QStringLiteral("/input"); }
    QString processed() const { return root + QStringLiteral("/processed"); }
    QString outputs() const { return root + QStringLiteral("/outputs"); }
    QString exports() const { return root + QStringLiteral("/exports"); }
    QString log() const { return root + QStringLiteral("/log.txt"); }
    QString meta() const { return root + QStringLiteral("/job.json"); }
};

// Program + leading arguments for a Nerfstudio tool (or the simulator in dry-run mode).
Command toolCommand(const Settings &settings, const QString &tool);

Command processDataCommand(const Settings &settings, const JobPaths &paths,
                           const QString &inputType);
Command trainCommand(const Settings &settings, const JobPaths &paths, const QString &jobId,
                     const QString &method, int iterations);
Command exportCommand(const Settings &settings, const JobPaths &paths, const QString &method,
                      const QString &trainConfig);

// Newest config.yml written by ns-train under outputs/, or empty.
QString findTrainConfig(const QString &outputsDir);
// The .ply written by ns-export, or empty.
QString findExport(const QString &exportsDir);

} // namespace u3d::pipeline
