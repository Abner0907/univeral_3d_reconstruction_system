#pragma once

#include "Settings.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;

class PreferencesDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PreferencesDialog(const u3d::Settings &settings, QWidget *parent = nullptr);
    u3d::Settings settings() const;

private:
    u3d::Settings m_settings;
    QLineEdit *m_nsBinDir;
    QLineEdit *m_dataDir;
    QCheckBox *m_dryRun;
    QComboBox *m_vis;
    QSpinBox *m_viewerPort;
    QComboBox *m_matching;
    QSpinBox *m_videoFrames;
    QSpinBox *m_serverPort;
};
