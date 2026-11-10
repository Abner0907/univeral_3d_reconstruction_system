import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import U3D

Page {
    id: page
    property string projectPath
    property var info: window.projects.info(projectPath)
    readonly property var job: window.reconstruction
    readonly property bool mine: job.projectPath === projectPath

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            ToolButton { text: "‹"; font.pixelSize: 24; onClicked: page.StackView.view.pop() }
            Label { text: qsTr("Reconstruct"); font.pixelSize: 18; Layout.fillWidth: true }
        }
    }

    Connections {
        target: page.job
        function onFinished(success) {
            if (success && page.mine)
                window.projects.refresh()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("%1 photos. Everything runs on this phone: %2, then Gaussian Splatting training.")
                  .arg(page.info.imageCount || 0)
                  .arg(page.info.hasArcorePoses && !forceSfm.checked ? qsTr("ARCore camera poses")
                                                                     : qsTr("feature matching + structure-from-motion"))
        }

        ColumnLayout {
            visible: !page.job.running
            Layout.fillWidth: true
            Label { text: qsTr("Quality") }
            ComboBox {
                id: quality
                Layout.fillWidth: true
                currentIndex: 1
                model: [qsTr("Fast - 2000 iterations, 320 px"),
                        qsTr("Balanced - 4000 iterations, 480 px"),
                        qsTr("High - 7000 iterations, 640 px (slow)")]
            }
            CheckBox {
                id: forceSfm
                visible: page.info.hasArcorePoses === true
                text: qsTr("Ignore ARCore poses (estimate from photos)")
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                opacity: 0.6
                font.pixelSize: 12
                text: qsTr("Training uses all %1 CPU cores and takes several minutes; the screen stays on meanwhile. Keep the phone plugged in for High quality.").arg(AppInfo.cpuThreads)
            }
            Button {
                text: qsTr("Start")
                highlighted: true
                Layout.fillWidth: true
                Layout.preferredHeight: 56
                enabled: AppInfo.canReconstruct && (page.info.imageCount || 0) >= 3
                onClicked: page.job.start(page.projectPath, quality.currentIndex, forceSfm.checked)
            }
        }

        ColumnLayout {
            visible: page.job.running || page.job.log.length > 0
            Layout.fillWidth: true
            ProgressBar {
                Layout.fillWidth: true
                value: page.job.progress
                indeterminate: page.job.running && page.job.progress === 0
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: page.job.running ? page.job.message
                                       : (page.job.error.length > 0 ? qsTr("Failed: %1").arg(page.job.error) : qsTr("Done."))
                color: !page.job.running && page.job.error.length > 0 ? "#ef5350" : Material.foreground
            }
            RowLayout {
                Layout.fillWidth: true
                Button {
                    visible: page.job.running
                    text: qsTr("Cancel")
                    onClicked: page.job.cancel()
                }
                Button {
                    visible: !page.job.running && page.job.resultUrl.toString().length > 0 && page.mine
                    text: qsTr("View 3D model")
                    highlighted: true
                    Layout.fillWidth: true
                    onClicked: page.StackView.view.replace(viewerPage, { source: page.job.resultUrl, title: page.info.name, projectPath: page.projectPath })
                }
            }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: page.job.log.length > 0
            TextArea {
                readOnly: true
                text: page.job.log
                font.family: "monospace"
                font.pixelSize: 11
                wrapMode: TextEdit.WrapAnywhere
                onTextChanged: cursorPosition = length
            }
        }
        Item { Layout.fillHeight: true; visible: page.job.log.length === 0 }
    }
}
