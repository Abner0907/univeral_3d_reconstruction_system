import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts
import U3D

Page {
    id: page

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            Label {
                text: "U3D"
                font.pixelSize: 20
                font.bold: true
                Layout.fillWidth: true
            }
            ToolButton {
                text: qsTr("Open .ply")
                onClicked: plyDialog.open()
            }
        }
    }

    FileDialog {
        id: plyDialog
        title: qsTr("Open a Gaussian splat or point cloud")
        nameFilters: [qsTr("PLY files (*.ply)")]
        onAccepted: window.openViewer(selectedFile, selectedFile.toString().split("/").pop())
    }

    FileDialog {
        id: importDialog
        title: qsTr("Choose 30-200 overlapping photos")
        fileMode: FileDialog.OpenFiles
        nameFilters: [qsTr("Photos (*.jpg *.jpeg *.png *.JPG *.JPEG *.PNG)")]
        onAccepted: {
            const path = window.projects.createProject("", "import")
            const n = window.projects.importImages(path, selectedFiles)
            window.openProject(path)
            if (n === 0)
                importError.open()
        }
    }

    Dialog {
        id: importError
        anchors.centerIn: parent
        title: qsTr("No photos imported")
        standardButtons: Dialog.Ok
        Label { text: qsTr("The selected files could not be read.") }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        // Capture actions (phone).
        RowLayout {
            visible: AppInfo.canCapture
            Layout.fillWidth: true
            spacing: 12
            Button {
                text: qsTr("New scan")
                highlighted: true
                Layout.fillWidth: true
                Layout.preferredHeight: 64
                onClicked: {
                    const path = window.projects.createProject("", "camera")
                    window.openCapture(path)
                }
            }
            Button {
                text: qsTr("Import photos")
                Layout.fillWidth: true
                Layout.preferredHeight: 64
                onClicked: importDialog.open()
            }
        }

        Label {
            visible: !AppInfo.canCapture
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.8
            text: qsTr("Desktop viewer: open a .ply exported from the U3D phone app (or any 3D Gaussian Splatting / point cloud file). Scanning and reconstruction run on the phone.")
        }

        Label {
            text: qsTr("Scans")
            font.pixelSize: 16
            font.bold: true
            visible: window.projects.count > 0
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 8
            model: window.projects
            delegate: ItemDelegate {
                width: list.width
                height: 84
                onClicked: window.openProject(model.path)
                contentItem: RowLayout {
                    spacing: 12
                    Rectangle {
                        Layout.preferredWidth: 72
                        Layout.preferredHeight: 72
                        color: "#2a3542"
                        radius: 6
                        clip: true
                        Image {
                            anchors.fill: parent
                            source: model.thumbnail
                            fillMode: Image.PreserveAspectCrop
                            sourceSize: Qt.size(144, 144)
                            asynchronous: true
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label { text: model.name; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                        Label {
                            text: qsTr("%1 photos · %2").arg(model.imageCount).arg(model.created)
                            opacity: 0.7
                            font.pixelSize: 12
                        }
                        Label {
                            visible: model.hasResult
                            text: qsTr("3D model ready")
                            color: Material.accent
                            font.pixelSize: 12
                        }
                    }
                }
            }
            Label {
                anchors.centerIn: parent
                width: parent.width * 0.85
                visible: list.count === 0 && AppInfo.canCapture
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                opacity: 0.6
                text: qsTr("No scans yet.\n\nTap \"New scan\" and walk slowly around an object, or import photos from the gallery.")
            }
        }
    }

    StackView.onActivated: window.projects.refresh()
}
