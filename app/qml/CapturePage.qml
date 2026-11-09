import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtMultimedia
import U3D

// Two capture modes:
//  - ARCore (preferred): tracked camera poses + feature points are recorded with every photo,
//    so reconstruction skips structure-from-motion. Auto mode takes a photo every ~7 cm / 7°.
//  - Plain camera (Qt Multimedia): photos only; poses are estimated later from the images.
Page {
    id: page
    property string projectPath
    property int plainCount: 0
    readonly property bool arMode: ar.available && arSwitch.checked
    readonly property bool permissionGranted: cameraPermission.status === Qt.PermissionStatus.Granted

    ArCapture {
        id: ar
        // Any ARCore problem: carry on with the normal camera and say why.
        onFailed: (message) => {
            errorLabel.text = message
            arSwitch.checked = false
        }
        onFrameCountChanged: flash.restart()
    }

    CameraPermission { id: cameraPermission }
    onPermissionGrantedChanged: updateSession()

    Component.onCompleted: {
        if (cameraPermission.status !== Qt.PermissionStatus.Granted)
            cameraPermission.request()
        plainCount = window.projects.info(projectPath).imageCount || 0
    }
    onArModeChanged: updateSession()
    function updateSession() {
        if (arMode && permissionGranted && !ar.running)
            ar.start(projectPath)
        else if (!arMode && ar.running)
            ar.stop()
    }
    Connections {
        target: ar
        function onAvailableChanged() { page.updateSession() }
    }

    function finish() {
        ar.stop()
        window.projects.refresh()
        page.StackView.view.pop()
    }

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            ToolButton { text: "‹"; font.pixelSize: 24; onClicked: page.finish() }
            Label {
                text: qsTr("%1 photos").arg(page.arMode ? ar.frameCount : page.plainCount)
                font.pixelSize: 18
                Layout.fillWidth: true
            }
            Switch {
                id: arSwitch
                visible: ar.available
                checked: true
                text: qsTr("ARCore")
            }
        }
    }

    // ------------------------------------------------------------------ preview
    Rectangle {
        anchors.fill: parent
        color: "black"

        // ARCore preview (frames from the ARCore session).
        Image {
            anchors.fill: parent
            visible: page.arMode
            fillMode: Image.PreserveAspectCrop
            cache: false
            source: page.arMode ? "image://arcore/" + ar.previewRevision : ""
        }

        // Plain camera.
        CaptureSession {
            camera: Camera {
                id: camera
                active: ar.ready && !page.arMode && cameraPermission.status === Qt.PermissionStatus.Granted
            }
            imageCapture: ImageCapture {
                id: imageCapture
                onImageSaved: { page.plainCount += 1; flash.restart() }
                onErrorOccurred: (id, error, message) => errorLabel.text = message
            }
            videoOutput: videoOutput
        }
        VideoOutput {
            id: videoOutput
            anchors.fill: parent
            visible: !page.arMode
            fillMode: VideoOutput.PreserveAspectCrop
        }
        // What the camera is waiting for (instead of a silent black screen).
        Label {
            anchors.centerIn: parent
            width: parent.width * 0.8
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            color: "white"
            visible: page.arMode ? ar.previewRevision === 0 : !camera.active || !imageCapture.readyForCapture
            text: !page.permissionGranted ? qsTr("Waiting for camera permission...")
                  : !ar.ready ? qsTr("Checking ARCore...")
                  : page.arMode ? qsTr("Starting ARCore camera...")
                  : qsTr("Starting camera...")
        }

        // White flash whenever a photo is saved.
        Rectangle {
            id: flashRect
            anchors.fill: parent
            color: "white"
            opacity: 0
            SequentialAnimation {
                id: flash
                NumberAnimation { target: flashRect; property: "opacity"; to: 0.6; duration: 40 }
                NumberAnimation { target: flashRect; property: "opacity"; to: 0; duration: 160 }
            }
        }

        Timer {
            id: plainAuto
            interval: 1000
            repeat: true
            running: !page.arMode && autoButton.checked && camera.active
            onTriggered: if (imageCapture.readyForCapture) imageCapture.captureToFile(window.projects.nextImagePath(page.projectPath))
        }

        // Status banner.
        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: 12
            height: statusColumn.implicitHeight + 16
            radius: 8
            color: "#b0000000"
            ColumnLayout {
                id: statusColumn
                anchors.fill: parent
                anchors.margins: 8
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: page.arMode && !ar.tracking ? "#ffb74d" : "white"
                    text: page.arMode ? (ar.status || qsTr("Starting ARCore..."))
                                      : qsTr("Walk slowly around the object; keep it centred and overlap each photo by ~70%.")
                }
                Label {
                    visible: ar.crashedLastTime
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: "#ffb74d"
                    text: ar.status
                }
                Button {
                    visible: ar.crashedLastTime
                    text: qsTr("Try ARCore again")
                    onClicked: ar.retryArCore()
                }
                Label {
                    visible: page.arMode
                    opacity: 0.7
                    font.pixelSize: 12
                    text: qsTr("%1 tracked points").arg(ar.pointCount)
                }
                Label {
                    id: errorLabel
                    visible: text.length > 0
                    color: "#ef5350"
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }
        }
    }

    // ------------------------------------------------------------------ controls
    footer: Pane {
        RowLayout {
            anchors.fill: parent
            Button {
                id: autoButton
                checkable: true
                checked: true
                text: qsTr("Auto")
                onCheckedChanged: ar.autoCapture = checked
                Component.onCompleted: ar.autoCapture = checked
            }
            Item { Layout.fillWidth: true }
            RoundButton {
                Layout.preferredWidth: 76
                Layout.preferredHeight: 76
                highlighted: true
                text: "●"
                font.pixelSize: 28
                onClicked: {
                    if (page.arMode)
                        ar.captureNow()
                    else if (imageCapture.readyForCapture)
                        imageCapture.captureToFile(window.projects.nextImagePath(page.projectPath))
                }
            }
            Item { Layout.fillWidth: true }
            Button {
                text: qsTr("Done")
                highlighted: true
                onClicked: page.finish()
            }
        }
    }
}
