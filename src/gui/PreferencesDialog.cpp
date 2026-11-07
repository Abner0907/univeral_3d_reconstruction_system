#include "PreferencesDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {

QWidget *withBrowse(QLineEdit *edit, QWidget *parent, const QString &title)
{
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *browse = new QPushButton(QObject::tr("Browse..."), row);
    layout->addWidget(edit, 1);
    layout->addWidget(browse);
    QObject::connect(browse, &QPushButton::clicked, row, [edit, parent, title] {
        const QString dir = QFileDialog::getExistingDirectory(parent, title, edit->text());
        if (!dir.isEmpty())
            edit->setText(dir);
    });
    return row;
}

} // namespace

PreferencesDialog::PreferencesDialog(const u3d::Settings &settings, QWidget *parent)
    : QDialog(parent)
    , m_settings(settings)
{
    setWindowTitle(tr("Preferences"));
    auto *form = new QFormLayout;

    m_nsBinDir = new QLineEdit(settings.nsBinDir, this);
    m_nsBinDir->setPlaceholderText(tr("Empty = use PATH (e.g. ~/miniconda3/envs/nerfstudio/bin)"));
    form->addRow(tr("Nerfstudio bin folder:"), withBrowse(m_nsBinDir, this, tr("Nerfstudio bin folder")));

    m_dataDir = new QLineEdit(settings.dataDir, this);
    form->addRow(tr("Data folder:"), withBrowse(m_dataDir, this, tr("Data folder")));

    m_dryRun = new QCheckBox(tr("Simulate Nerfstudio (dry run, no GPU needed)"), this);
    m_dryRun->setChecked(settings.dryRun);
    form->addRow(QString(), m_dryRun);

    m_vis = new QComboBox(this);
    m_vis->addItems({QStringLiteral("viewer"), QStringLiteral("tensorboard"),
                     QStringLiteral("viewer+tensorboard"), QStringLiteral("wandb")});
    m_vis->setCurrentText(settings.vis);
    form->addRow(tr("Training visualiser:"), m_vis);

    m_viewerPort = new QSpinBox(this);
    m_viewerPort->setRange(1024, 65535);
    m_viewerPort->setValue(settings.viewerPort);
    form->addRow(tr("Live viewer port:"), m_viewerPort);

    m_matching = new QComboBox(this);
    m_matching->addItems({QStringLiteral("vocab_tree"), QStringLiteral("exhaustive"),
                          QStringLiteral("sequential")});
    m_matching->setCurrentText(settings.matchingMethod);
    m_matching->setToolTip(tr("sequential is fastest for video / walk-around captures; "
                              "exhaustive is most robust for small unordered sets."));
    form->addRow(tr("COLMAP matching:"), m_matching);

    m_videoFrames = new QSpinBox(this);
    m_videoFrames->setRange(20, 2000);
    m_videoFrames->setValue(settings.videoFramesTarget);
    form->addRow(tr("Frames from video:"), m_videoFrames);

    m_serverPort = new QSpinBox(this);
    m_serverPort->setRange(1024, 65535);
    m_serverPort->setValue(settings.serverPort);
    form->addRow(tr("Phone upload port:"), m_serverPort);

    auto *note = new QLabel(tr("A changed data folder takes effect after restarting the app."), this);
    note->setWordWrap(true);
    note->setEnabled(false);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(note);
    layout->addWidget(buttons);
    resize(620, sizeHint().height());
}

u3d::Settings PreferencesDialog::settings() const
{
    u3d::Settings s = m_settings;
    s.nsBinDir = m_nsBinDir->text().trimmed();
    s.dataDir = m_dataDir->text().trimmed();
    s.dryRun = m_dryRun->isChecked();
    s.vis = m_vis->currentText();
    s.viewerPort = m_viewerPort->value();
    s.matchingMethod = m_matching->currentText();
    s.videoFramesTarget = m_videoFrames->value();
    s.serverPort = m_serverPort->value();
    return s;
}
