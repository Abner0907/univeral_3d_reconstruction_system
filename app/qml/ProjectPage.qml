import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts
import U3D

Page {
    id: page
    property string projectPath
    property var info: ({})

    function reload() { info = window.projects.info(projectPath) }
    Component.onCompleted: reload()
    StackView.onActivated: reload()

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            ToolButton { text: "‹"; font.pixelSize: 24; onClicked: page.StackView.view.pop() }
            Label { text: page.info.name || ""; font.pixelSize: 18; elide: Text.ElideRight; Layout.fillWidth: true }
            ToolButton { text: qsTr("Rename"); onClicked: renameDialog.open() }
        }
    }

    FileDialog {
        id: importDialog
        fileMode: FileDialog.OpenFiles
        nameFilters: [qsTr("Photos (*.jpg *.jpeg *.png *.JPG *.JPEG *.PNG)")]
        onAccepted: {
            window.projects.importImages(page.projectPath, selectedFiles)
            page.reload()
        }
    }

    FileDialog {
        id: exportDialog
        fileMode: FileDialog.SaveFile
        defaultSuffix: "ply"
        currentFile: "file:///" + (page.info.name || "scan").replace(/[^A-Za-z0-9_-]+/g, "_") + ".ply"
        nameFilters: [qsTr("Gaussian splat (*.ply)")]
        onAccepted: exportResult.text = window.projects.exportResult(page.projectPath, selectedFile)
                    ? qsTr("Exported.") : qsTr("Export failed.")
    }

    Dialog {
        id: renameDialog
        anchors.centerIn: parent
        title: qsTr("Rename scan")
        standardButtons: Dialog.Ok | Dialog.Cancel
        TextField { id: nameField; text: page.info.name || ""; width: 260 }
        onAccepted: { window.projects.renameProject(page.projectPath, nameField.text); page.reload() }
    }

    Dialog {
        id: deleteDialog
        anchors.centerIn: parent
        title: qsTr("Delete this scan?")
        standardButtons: Dialog.Yes | Dialog.No
        Label { text: qsTr("All photos and the 3D model will be removed.") }
        onAccepted: {
            window.projects.deleteProject(page.projectPath)
            page.StackView.view.pop()
        }
    }

    Flickable {
        anchors.fill: parent
        contentHeight: column.implicitHeight + 32
        clip: true

        ColumnLayout {
            id: column
            x: 16
            y: 16
            width: parent.width - 32
            spacing: 12

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: width * 0.6
                color: "#2a3542"
                radius: 8
                clip: true
                Image {
                    anchors.fill: parent
                    source: page.info.thumbnail || ""
                    fillMode: Image.PreserveAspectCrop
                    sourceSize.width: 800
                    asynchronous: true
                }
                Label {
                    anchors.centerIn: parent
                    visible: !page.info.imageCount
                    text: qsTr("No photos yet")
                    opacity: 0.6
                }
            }

            Label {
                text: qsTr("%1 photos · %2").arg(page.info.imageCount || 0).arg(
                          page.info.hasArcorePoses ? qsTr("ARCore camera poses recorded") : qsTr("poses will be estimated from the photos"))
                opacity: 0.8
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            Button {
                text: page.info.hasResult ? qsTr("View 3D model") : qsTr("Reconstruct 3D model")
                highlighted: true
                Layout.fillWidth: true
                Layout.preferredHeight: 56
                enabled: page.info.hasResult || (AppInfo.canReconstruct && page.info.imageCount >= 3)
                onClicked: page.info.hasResult ? window.openViewer(page.info.resultUrl, page.info.name)
                                               : window.openReconstruct(page.projectPath)
            }
            Button {
                visible: page.info.hasResult && AppInfo.canReconstruct
                text: qsTr("Reconstruct again")
                Layout.fillWidth: true
                onClicked: window.openReconstruct(page.projectPath)
            }
            RowLayout {
                visible: AppInfo.canCapture
                Layout.fillWidth: true
                Button {
                    text: qsTr("Take more photos")
                    Layout.fillWidth: true
                    onClicked: window.openCapture(page.projectPath)
                }
                Button {
                    text: qsTr("Import photos")
                    Layout.fillWidth: true
                    onClicked: importDialog.open()
                }
            }
            Button {
                visible: page.info.hasResult
                text: qsTr("Export .ply")
                Layout.fillWidth: true
                onClicked: exportDialog.open()
            }
            Label { id: exportResult; visible: text.length > 0; color: Material.accent }

            Button {
                text: qsTr("Delete scan")
                flat: true
                Material.foreground: Material.Red
                Layout.alignment: Qt.AlignHCenter
                onClicked: deleteDialog.open()
            }

            Label {
                visible: (page.info.log || "").length > 0
                text: qsTr("Last reconstruction log")
                font.bold: true
            }
            Label {
                visible: (page.info.log || "").length > 0
                text: page.info.log || ""
                font.family: "monospace"
                font.pixelSize: 11
                wrapMode: Text.WrapAnywhere
                opacity: 0.7
                Layout.fillWidth: true
            }
        }
    }
}
