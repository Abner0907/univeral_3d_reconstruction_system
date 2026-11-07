#include "MainWindow.h"

#include "ApiService.h"
#include "JobManager.h"
#include "Pipeline.h"
#include "PointCloudViewer.h"
#include "PreferencesDialog.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

using u3d::Job;
using u3d::JobStatus;

namespace {

QString statusText(const Job &job)
{
    switch (job.status) {
    case JobStatus::Uploading: return QObject::tr("Uploading");
    case JobStatus::Queued: return QObject::tr("Queued");
    case JobStatus::Running: return QObject::tr("Running");
    case JobStatus::Succeeded: return QObject::tr("Done");
    case JobStatus::Failed: return QObject::tr("Failed");
    case JobStatus::Cancelled: return QObject::tr("Cancelled");
    }
    return {};
}

} // namespace

MainWindow::MainWindow(u3d::JobManager *jobs, u3d::ApiService *api, QWidget *parent)
    : QMainWindow(parent)
    , m_jobs(jobs)
    , m_api(api)
{
    setWindowTitle(tr("U3D Studio - Universal 3D Reconstruction"));
    setAcceptDrops(true);
    buildUi();
    buildMenus();

    for (const Job &job : m_jobs->jobs())
        refreshJobRow(job.id);
    connect(m_jobs, &u3d::JobManager::jobAdded, this, &MainWindow::refreshJobRow);
    connect(m_jobs, &u3d::JobManager::jobChanged, this, &MainWindow::refreshJobRow);
    connect(m_jobs, &u3d::JobManager::jobRemoved, this, &MainWindow::removeJobRow);
    connect(m_jobs, &u3d::JobManager::logAppended, this, [this](const QString &id, const QString &text) {
        if (id != m_logJobId)
            return;
        QScrollBar *bar = m_log->verticalScrollBar();
        const bool atBottom = bar->value() >= bar->maximum() - 4;
        m_log->moveCursor(QTextCursor::End);
        m_log->insertPlainText(text);
        if (atBottom)
            bar->setValue(bar->maximum());
    });
    connect(&m_loadWatcher, &QFutureWatcher<LoadResult>::finished, this, &MainWindow::onPlyLoaded);

    QSettings store;
    restoreGeometry(store.value(QStringLiteral("window/geometry")).toByteArray());
    setServerEnabled(store.value(QStringLiteral("server/enabled"), true).toBool());
    onSelectionChanged();
}

MainWindow::~MainWindow() = default;

// ----------------------------------------------------------------------------- UI

void MainWindow::buildUi()
{
    // --- New reconstruction
    auto *newBox = new QGroupBox(tr("New reconstruction"));
    m_fileList = new QListWidget;
    m_fileList->setMinimumHeight(110);
    m_fileList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_fileSummary = new QLabel(tr("Add 30-200 overlapping photos or one video, or drop them here."));
    m_fileSummary->setWordWrap(true);

    auto *addImagesBtn = new QPushButton(tr("Images..."));
    auto *addFolderBtn = new QPushButton(tr("Folder..."));
    auto *addVideoBtn = new QPushButton(tr("Video..."));
    auto *clearBtn = new QPushButton(tr("Clear"));
    connect(addImagesBtn, &QPushButton::clicked, this, &MainWindow::addImages);
    connect(addFolderBtn, &QPushButton::clicked, this, &MainWindow::addFolder);
    connect(addVideoBtn, &QPushButton::clicked, this, &MainWindow::addVideo);
    connect(clearBtn, &QPushButton::clicked, this, &MainWindow::clearFiles);
    auto *fileButtons = new QHBoxLayout;
    fileButtons->addWidget(addImagesBtn);
    fileButtons->addWidget(addFolderBtn);
    fileButtons->addWidget(addVideoBtn);
    fileButtons->addWidget(clearBtn);

    m_jobName = new QLineEdit;
    m_jobName->setPlaceholderText(tr("Optional"));
    m_method = new QComboBox;
    m_method->addItem(tr("Gaussian Splatting (splatfacto / gsplat)"), QStringLiteral("splatfacto"));
    m_method->addItem(tr("Gaussian Splatting, big"), QStringLiteral("splatfacto-big"));
    m_method->addItem(tr("NeRF (nerfacto)"), QStringLiteral("nerfacto"));
    m_method->addItem(tr("NeRF, big"), QStringLiteral("nerfacto-big"));
    m_method->setCurrentIndex(qMax(0, m_method->findData(m_jobs->settings().defaultMethod)));
    m_iterations = new QSpinBox;
    m_iterations->setRange(100, 1000000);
    m_iterations->setSingleStep(1000);
    m_iterations->setValue(m_jobs->settings().defaultIterations);

    auto *form = new QFormLayout;
    form->addRow(tr("Name:"), m_jobName);
    form->addRow(tr("Method:"), m_method);
    form->addRow(tr("Iterations:"), m_iterations);

    m_startButton = new QPushButton(tr("Start reconstruction"));
    m_startButton->setDefault(true);
    connect(m_startButton, &QPushButton::clicked, this, &MainWindow::startReconstruction);

    auto *newLayout = new QVBoxLayout(newBox);
    newLayout->addWidget(m_fileList);
    newLayout->addWidget(m_fileSummary);
    newLayout->addLayout(fileButtons);
    newLayout->addLayout(form);
    newLayout->addWidget(m_startButton);

    // --- Jobs
    auto *jobsBox = new QGroupBox(tr("Jobs"));
    m_jobTable = new QTableWidget(0, 4);
    m_jobTable->setHorizontalHeaderLabels({tr("Name"), tr("Method"), tr("Status"), tr("Progress")});
    m_jobTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int c = 1; c < 4; ++c)
        m_jobTable->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    m_jobTable->verticalHeader()->hide();
    m_jobTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_jobTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_jobTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_jobTable->setSortingEnabled(false);
    connect(m_jobTable, &QTableWidget::itemSelectionChanged, this, &MainWindow::onSelectionChanged);
    connect(m_jobTable, &QTableWidget::cellDoubleClicked, this, [this] { viewSelected(); });

    m_details = new QLabel;
    m_details->setWordWrap(true);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_progress = new QProgressBar;
    m_progress->setRange(0, 1000);
    m_progress->setTextVisible(false);

    m_cancelButton = new QPushButton(tr("Cancel"));
    m_deleteButton = new QPushButton(tr("Delete"));
    m_viewButton = new QPushButton(tr("View 3D"));
    m_exportButton = new QPushButton(tr("Save PLY..."));
    m_folderButton = new QPushButton(tr("Open folder"));
    m_liveButton = new QPushButton(tr("Live viewer"));
    m_liveButton->setToolTip(tr("Open the Nerfstudio web viewer of the job that is training"));
    connect(m_cancelButton, &QPushButton::clicked, this, &MainWindow::cancelSelected);
    connect(m_deleteButton, &QPushButton::clicked, this, &MainWindow::deleteSelected);
    connect(m_viewButton, &QPushButton::clicked, this, &MainWindow::viewSelected);
    connect(m_exportButton, &QPushButton::clicked, this, &MainWindow::exportSelected);
    connect(m_folderButton, &QPushButton::clicked, this, &MainWindow::openSelectedFolder);
    connect(m_liveButton, &QPushButton::clicked, this, &MainWindow::openLiveViewer);
    auto *actions1 = new QHBoxLayout;
    actions1->addWidget(m_viewButton);
    actions1->addWidget(m_exportButton);
    actions1->addWidget(m_liveButton);
    auto *actions2 = new QHBoxLayout;
    actions2->addWidget(m_folderButton);
    actions2->addWidget(m_cancelButton);
    actions2->addWidget(m_deleteButton);

    auto *jobsLayout = new QVBoxLayout(jobsBox);
    jobsLayout->addWidget(m_jobTable, 1);
    jobsLayout->addWidget(m_details);
    jobsLayout->addWidget(m_progress);
    jobsLayout->addLayout(actions1);
    jobsLayout->addLayout(actions2);

    auto *left = new QWidget;
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(newBox);
    leftLayout->addWidget(jobsBox, 1);

    // --- Viewer tab
    m_viewer = new PointCloudViewer;
    auto *openPlyBtn = new QPushButton(tr("Open PLY..."));
    connect(openPlyBtn, &QPushButton::clicked, this, &MainWindow::openPly);
    auto *pointSize = new QSlider(Qt::Horizontal);
    pointSize->setRange(1, 10);
    pointSize->setValue(2);
    pointSize->setMaximumWidth(140);
    connect(pointSize, &QSlider::valueChanged, this, [this](int v) { m_viewer->setPointSize(float(v)); });
    auto *upAxis = new QComboBox;
    upAxis->addItems({tr("Z up (Nerfstudio)"), tr("Y up"), tr("-Y up (COLMAP)")});
    connect(upAxis, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) {
        m_viewer->setUpAxis(i == 0 ? PointCloudViewer::UpAxis::ZUp
                            : i == 1 ? PointCloudViewer::UpAxis::YUp
                                     : PointCloudViewer::UpAxis::NegYUp);
    });
    auto *resetBtn = new QPushButton(tr("Reset view"));
    connect(resetBtn, &QPushButton::clicked, m_viewer, &PointCloudViewer::resetView);
    m_viewerInfo = new QLabel(tr("Select a finished job and press \"View 3D\", or open a PLY file."));

    auto *viewerBar = new QHBoxLayout;
    viewerBar->addWidget(openPlyBtn);
    viewerBar->addWidget(new QLabel(tr("Point size:")));
    viewerBar->addWidget(pointSize);
    viewerBar->addWidget(upAxis);
    viewerBar->addWidget(resetBtn);
    viewerBar->addWidget(m_viewerInfo, 1);
    auto *viewerTab = new QWidget;
    auto *viewerLayout = new QVBoxLayout(viewerTab);
    viewerLayout->addWidget(m_viewer, 1);
    viewerLayout->addLayout(viewerBar);

    // --- Log tab
    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(20000);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_log->setLineWrapMode(QPlainTextEdit::NoWrap);

    m_tabs = new QTabWidget;
    m_tabs->addTab(viewerTab, tr("3D Viewer"));
    m_tabs->addTab(m_log, tr("Log"));

    auto *splitter = new QSplitter;
    splitter->addWidget(left);
    splitter->addWidget(m_tabs);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({430, 850});
    setCentralWidget(splitter);

    // --- Phone server status
    m_serverStatus = new QLabel;
    m_serverStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusBar()->addPermanentWidget(m_serverStatus);
    resize(1300, 820);
}

void MainWindow::buildMenus()
{
    QMenu *file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("Add &images..."), this, &MainWindow::addImages)->setShortcut(QKeySequence::Open);
    file->addAction(tr("Open &PLY..."), this, &MainWindow::openPly);
    file->addSeparator();
    file->addAction(tr("&Preferences..."), this, &MainWindow::showPreferences)
        ->setShortcut(QKeySequence::Preferences);
    file->addSeparator();
    file->addAction(tr("&Quit"), this, &QWidget::close)->setShortcut(QKeySequence::Quit);

    m_serverAction = new QAction(tr("Accept phone uploads"), this);
    m_serverAction->setCheckable(true);
    m_serverAction->setToolTip(tr("Run the HTTP API the Android capture app uploads to"));
    connect(m_serverAction, &QAction::toggled, this, &MainWindow::setServerEnabled);

    QMenu *phone = menuBar()->addMenu(tr("P&hone"));
    phone->addAction(m_serverAction);

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&About"), this, [this] {
        QMessageBox::about(this, tr("About U3D Studio"),
            tr("<h3>U3D Studio %1</h3>"
               "<p>Universal 3D Reconstruction System.</p>"
               "<p>Captures from the desktop or the Android app are reconstructed with "
               "Nerfstudio: COLMAP poses, then Gaussian Splatting (splatfacto, rendered with "
               "gsplat) or NeRF (nerfacto).</p>").arg(QStringLiteral(U3D_VERSION)));
    });

    QToolBar *bar = addToolBar(tr("Main"));
    bar->setObjectName(QStringLiteral("mainToolbar"));
    bar->setMovable(false);
    bar->addAction(m_serverAction);
    bar->addAction(tr("Preferences"), this, &MainWindow::showPreferences);
}

// ----------------------------------------------------------------------------- inputs

void MainWindow::addFiles(const QStringList &paths)
{
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (info.isDir()) {
            QStringList inDir;
            QDirIterator it(path, QDir::Files);
            while (it.hasNext()) {
                const QString f = it.next();
                if (u3d::pipeline::isImageFile(f))
                    inDir << f;
            }
            inDir.sort();
            addFiles(inDir);
            continue;
        }
        if (!u3d::pipeline::isImageFile(path) && !u3d::pipeline::isVideoFile(path))
            continue;
        const QString abs = info.absoluteFilePath();
        if (m_files.contains(abs))
            continue;
        m_files << abs;
        m_fileList->addItem(info.fileName());
    }
    QString error;
    const QString kind = u3d::pipeline::classifyInputs(m_files, &error);
    if (m_files.isEmpty())
        m_fileSummary->setText(tr("Add 30-200 overlapping photos or one video, or drop them here."));
    else if (kind.isEmpty())
        m_fileSummary->setText(QStringLiteral("<span style='color:#c0392b'>%1</span>").arg(error.toHtmlEscaped()));
    else if (kind == QLatin1String("video"))
        m_fileSummary->setText(tr("1 video selected."));
    else
        m_fileSummary->setText(tr("%n image(s) selected.", nullptr, int(m_files.size())));
    m_startButton->setEnabled(!kind.isEmpty());
}

void MainWindow::addImages()
{
    QSettings store;
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Add images"), store.value(QStringLiteral("lastDir")).toString(),
        tr("Images (*.jpg *.jpeg *.png *.tif *.tiff *.JPG *.JPEG *.PNG)"));
    if (!files.isEmpty())
        store.setValue(QStringLiteral("lastDir"), QFileInfo(files.first()).absolutePath());
    addFiles(files);
}

void MainWindow::addFolder()
{
    QSettings store;
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Add all images in folder"), store.value(QStringLiteral("lastDir")).toString());
    if (dir.isEmpty())
        return;
    store.setValue(QStringLiteral("lastDir"), dir);
    addFiles({dir});
}

void MainWindow::addVideo()
{
    QSettings store;
    const QString file = QFileDialog::getOpenFileName(
        this, tr("Add video"), store.value(QStringLiteral("lastDir")).toString(),
        tr("Videos (*.mp4 *.mov *.avi *.mkv *.webm *.MP4 *.MOV)"));
    if (file.isEmpty())
        return;
    store.setValue(QStringLiteral("lastDir"), QFileInfo(file).absolutePath());
    addFiles({file});
}

void MainWindow::clearFiles()
{
    m_files.clear();
    m_fileList->clear();
    addFiles({});
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    QStringList paths;
    const auto urls = event->mimeData()->urls();
    for (const QUrl &url : urls)
        if (url.isLocalFile())
            paths << url.toLocalFile();
    if (paths.size() == 1 && paths.first().endsWith(QLatin1String(".ply"), Qt::CaseInsensitive)) {
        loadPly(paths.first(), QFileInfo(paths.first()).fileName());
        return;
    }
    addFiles(paths);
}

void MainWindow::startReconstruction()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const QString id = m_jobs->submit(m_jobName->text(), m_method->currentData().toString(),
                                      m_iterations->value(), m_files, &error);
    QApplication::restoreOverrideCursor();
    if (id.isEmpty()) {
        QMessageBox::warning(this, tr("Cannot start"), error);
        return;
    }
    clearFiles();
    m_jobName->clear();
    const int row = rowForJob(id);
    if (row >= 0)
        m_jobTable->selectRow(row);
    m_tabs->setCurrentIndex(1);
}

// ----------------------------------------------------------------------------- job list

int MainWindow::rowForJob(const QString &id) const
{
    for (int r = 0; r < m_jobTable->rowCount(); ++r)
        if (m_jobTable->item(r, 0)->data(Qt::UserRole).toString() == id)
            return r;
    return -1;
}

void MainWindow::refreshJobRow(const QString &id)
{
    if (!m_jobs->hasJob(id))
        return;
    const Job job = m_jobs->job(id);
    int row = rowForJob(id);
    if (row < 0) {
        // Newest first: insert above older jobs.
        row = 0;
        while (row < m_jobTable->rowCount()
               && m_jobs->job(m_jobTable->item(row, 0)->data(Qt::UserRole).toString()).createdAt > job.createdAt)
            ++row;
        m_jobTable->insertRow(row);
        for (int c = 0; c < 4; ++c)
            m_jobTable->setItem(row, c, new QTableWidgetItem);
        m_jobTable->item(row, 0)->setData(Qt::UserRole, id);
    }
    m_jobTable->item(row, 0)->setText(job.name);
    m_jobTable->item(row, 0)->setToolTip(job.message);
    m_jobTable->item(row, 1)->setText(job.method);
    m_jobTable->item(row, 2)->setText(statusText(job));
    m_jobTable->item(row, 3)->setText(QStringLiteral("%1%").arg(int(job.progress * 100.0)));
    if (id == selectedJobId())
        updateDetails();
}

void MainWindow::removeJobRow(const QString &id)
{
    const int row = rowForJob(id);
    if (row >= 0)
        m_jobTable->removeRow(row);
    if (m_logJobId == id) {
        m_logJobId.clear();
        m_log->clear();
    }
}

QString MainWindow::selectedJobId() const
{
    const auto rows = m_jobTable->selectionModel()->selectedRows();
    if (rows.isEmpty())
        return {};
    return m_jobTable->item(rows.first().row(), 0)->data(Qt::UserRole).toString();
}

void MainWindow::onSelectionChanged()
{
    const QString id = selectedJobId();
    if (id != m_logJobId) {
        m_logJobId = id;
        m_log->setPlainText(id.isEmpty() ? QString() : m_jobs->logTail(id, 512 * 1024));
        m_log->verticalScrollBar()->setValue(m_log->verticalScrollBar()->maximum());
    }
    updateDetails();
}

void MainWindow::updateDetails()
{
    const QString id = selectedJobId();
    const bool has = !id.isEmpty() && m_jobs->hasJob(id);
    const Job job = has ? m_jobs->job(id) : Job();
    if (!has) {
        m_details->setText(tr("No job selected."));
        m_progress->setValue(0);
    } else {
        m_details->setText(tr("<b>%1</b> &nbsp;%2<br>%3 %4 input(s), %5 iterations<br>"
                              "Created %6<br><i>%7</i>")
                               .arg(job.name.toHtmlEscaped(), statusText(job))
                               .arg(job.numInputs)
                               .arg(job.inputType.isEmpty() ? tr("file") : job.inputType)
                               .arg(job.iterations)
                               .arg(QDateTime::fromMSecsSinceEpoch(job.createdAt).toString(Qt::ISODate))
                               .arg(job.message.toHtmlEscaped()));
        m_progress->setValue(int(job.progress * 1000));
    }
    const bool active = has && !job.isFinished();
    m_cancelButton->setEnabled(active);
    m_deleteButton->setEnabled(has);
    m_viewButton->setEnabled(has && job.hasResult());
    m_exportButton->setEnabled(has && job.hasResult());
    m_folderButton->setEnabled(has);
    m_liveButton->setEnabled(has && job.status == JobStatus::Running && job.stage == QLatin1String("train")
                             && m_jobs->settings().vis.contains(QLatin1String("viewer"))
                             && !m_jobs->settings().dryRun);
}

void MainWindow::cancelSelected()
{
    const QString id = selectedJobId();
    if (!id.isEmpty())
        m_jobs->cancelJob(id);
}

void MainWindow::deleteSelected()
{
    const QString id = selectedJobId();
    if (id.isEmpty())
        return;
    const Job job = m_jobs->job(id);
    if (QMessageBox::question(this, tr("Delete job"),
                              tr("Delete \"%1\" and all of its files?").arg(job.name))
        != QMessageBox::Yes)
        return;
    m_jobs->removeJob(id);
}

void MainWindow::viewSelected()
{
    const QString id = selectedJobId();
    const QString path = id.isEmpty() ? QString() : m_jobs->resultPath(id);
    if (path.isEmpty())
        return;
    loadPly(path, m_jobs->job(id).name);
}

void MainWindow::exportSelected()
{
    const QString id = selectedJobId();
    const QString path = id.isEmpty() ? QString() : m_jobs->resultPath(id);
    if (path.isEmpty())
        return;
    const Job job = m_jobs->job(id);
    const QString suggested = u3d::pipeline::sanitizeFileName(job.name, job.id) + QLatin1Char('_')
                              + job.method + QStringLiteral(".ply");
    const QString target = QFileDialog::getSaveFileName(
        this, tr("Save result"), QDir::home().filePath(suggested), tr("PLY (*.ply)"));
    if (target.isEmpty())
        return;
    QFile::remove(target);
    if (!QFile::copy(path, target))
        QMessageBox::warning(this, tr("Save failed"), tr("Could not write %1").arg(target));
}

void MainWindow::openSelectedFolder()
{
    const QString id = selectedJobId();
    if (!id.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_jobs->jobDir(id)));
}

void MainWindow::openLiveViewer()
{
    QDesktopServices::openUrl(
        QUrl(QStringLiteral("http://localhost:%1").arg(m_jobs->settings().viewerPort)));
}

// ----------------------------------------------------------------------------- viewer

void MainWindow::openPly()
{
    const QString file = QFileDialog::getOpenFileName(this, tr("Open PLY"), QString(),
                                                      tr("PLY point clouds / splats (*.ply)"));
    if (!file.isEmpty())
        loadPly(file, QFileInfo(file).fileName());
}

void MainWindow::loadPly(const QString &path, const QString &title)
{
    if (m_loadWatcher.isRunning())
        return;
    m_viewerInfo->setText(tr("Loading %1...").arg(title));
    m_tabs->setCurrentIndex(0);
    m_loadWatcher.setFuture(QtConcurrent::run([path, title] {
        LoadResult result;
        result.title = title;
        u3d::PlyLoader::load(path, result.cloud, &result.error);
        return result;
    }));
}

void MainWindow::onPlyLoaded()
{
    const LoadResult result = m_loadWatcher.result();
    if (!result.error.isEmpty()) {
        m_viewerInfo->setText(tr("Failed to load %1").arg(result.title));
        QMessageBox::warning(this, tr("Cannot open PLY"), result.error);
        return;
    }
    m_viewer->setPointCloud(result.cloud);
    m_viewerInfo->setText(tr("%1 - %2 %3")
                              .arg(result.title)
                              .arg(QLocale().toString(qulonglong(result.cloud.size())))
                              .arg(result.cloud.isGaussianSplat ? tr("Gaussians (shown as points)")
                                                                : tr("points")));
}

// ----------------------------------------------------------------------------- settings / server

void MainWindow::showPreferences()
{
    PreferencesDialog dialog(m_jobs->settings(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const u3d::Settings previous = m_jobs->settings();
    const u3d::Settings updated = dialog.settings();
    QSettings store;
    updated.save(store);
    m_jobs->setSettings(updated);
    if (updated.serverPort != previous.serverPort && m_serverAction->isChecked())
        setServerEnabled(true);
    if (updated.dataDir != previous.dataDir)
        QMessageBox::information(this, tr("Restart required"),
                                 tr("The new data folder is used after restarting U3D Studio."));
    updateDetails();
    updateServerStatus();
}

void MainWindow::setServerEnabled(bool enabled)
{
    if (enabled) {
        QString error;
        if (!m_api->start(quint16(m_jobs->settings().serverPort), &error)) {
            QMessageBox::warning(this, tr("Phone uploads"),
                                 tr("Cannot listen on port %1: %2")
                                     .arg(m_jobs->settings().serverPort)
                                     .arg(error));
            enabled = false;
        }
    } else {
        m_api->stop();
    }
    {
        const QSignalBlocker block(m_serverAction);
        m_serverAction->setChecked(enabled);
    }
    QSettings().setValue(QStringLiteral("server/enabled"), enabled);
    updateServerStatus();
}

void MainWindow::updateServerStatus()
{
    QString text;
    if (m_api->isRunning())
        text = tr("Phone uploads: %1").arg(m_api->lanUrls().join(QStringLiteral("  ")));
    else
        text = tr("Phone uploads: off");
    if (m_jobs->settings().dryRun)
        text += tr("   |   DRY RUN (Nerfstudio simulated)");
    m_serverStatus->setText(text);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    bool running = false;
    for (const Job &job : m_jobs->jobs())
        running |= job.status == JobStatus::Running;
    if (running
        && QMessageBox::question(this, tr("Quit"),
                                 tr("A reconstruction is running and will be stopped. Quit anyway?"))
               != QMessageBox::Yes) {
        event->ignore();
        return;
    }
    QSettings().setValue(QStringLiteral("window/geometry"), saveGeometry());
    event->accept();
}
