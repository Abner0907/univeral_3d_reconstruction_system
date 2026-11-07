#pragma once

#include "PlyLoader.h"

#include <QFutureWatcher>
#include <QMainWindow>
#include <QStringList>

class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class PointCloudViewer;

namespace u3d {
class ApiService;
class JobManager;
} // namespace u3d

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(u3d::JobManager *jobs, u3d::ApiService *api, QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    struct LoadResult {
        u3d::PointCloud cloud;
        QString error;
        QString title;
    };

    void buildUi();
    void buildMenus();

    void addFiles(const QStringList &paths);
    void addImages();
    void addFolder();
    void addVideo();
    void clearFiles();
    void startReconstruction();

    void refreshJobRow(const QString &id);
    void removeJobRow(const QString &id);
    int rowForJob(const QString &id) const;
    QString selectedJobId() const;
    void onSelectionChanged();
    void updateDetails();

    void cancelSelected();
    void deleteSelected();
    void viewSelected();
    void exportSelected();
    void openSelectedFolder();
    void openLiveViewer();

    void openPly();
    void loadPly(const QString &path, const QString &title);
    void onPlyLoaded();

    void showPreferences();
    void setServerEnabled(bool enabled);
    void updateServerStatus();

    u3d::JobManager *m_jobs;
    u3d::ApiService *m_api;

    // new reconstruction
    QListWidget *m_fileList = nullptr;
    QLabel *m_fileSummary = nullptr;
    QLineEdit *m_jobName = nullptr;
    QComboBox *m_method = nullptr;
    QSpinBox *m_iterations = nullptr;
    QPushButton *m_startButton = nullptr;
    QStringList m_files;

    // jobs
    QTableWidget *m_jobTable = nullptr;
    QLabel *m_details = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QPushButton *m_viewButton = nullptr;
    QPushButton *m_exportButton = nullptr;
    QPushButton *m_folderButton = nullptr;
    QPushButton *m_liveButton = nullptr;

    // centre
    QTabWidget *m_tabs = nullptr;
    PointCloudViewer *m_viewer = nullptr;
    QLabel *m_viewerInfo = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QString m_logJobId;

    QAction *m_serverAction = nullptr;
    QLabel *m_serverStatus = nullptr;

    QFutureWatcher<LoadResult> m_loadWatcher;
};
