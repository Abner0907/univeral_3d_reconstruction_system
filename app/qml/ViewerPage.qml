import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts
import U3D

Page {
    id: page
    property url source
    property string projectPath // set when the model belongs to a scan (crop saves in place)
    property int cropRevision: 0 // bumped after each save so bindings re-check the backup
    property bool moveMode: false // one-finger drag moves instead of rotating

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            ToolButton {
                text: "‹"
                font.pixelSize: 24
                visible: page.StackView.view && page.StackView.view.depth > 1
                onClicked: page.StackView.view.pop()
            }
            Label { text: page.title; font.pixelSize: 18; elide: Text.ElideRight; Layout.fillWidth: true; leftPadding: 8 }
            ToolButton {
                text: qsTr("Crop")
                checkable: true
                checked: view.cropEnabled
                enabled: view.splatCount > 0
                onClicked: view.cropEnabled = checked
            }
            ToolButton { text: qsTr("Reset"); onClicked: view.resetView() }
        }
    }

    background: Rectangle {
        gradient: Gradient {
            GradientStop { position: 0; color: "#2b3442" }
            GradientStop { position: 1; color: "#10141a" }
        }
    }

    SplatView {
        id: view
        anchors.fill: parent
        source: page.source

        // One finger / left mouse: rotate around the orbit centre (or move, in Move mode).
        // Two fingers: move + pinch zoom.  Right / middle mouse: move.  Wheel: zoom.
        // Double tap / double click: make the tapped spot the orbit centre.
        DragHandler {
            id: oneFinger
            target: null
            acceptedButtons: Qt.LeftButton
            minimumPointCount: 1
            maximumPointCount: 1
            property point last
            onActiveChanged: last = Qt.point(0, 0)
            onActiveTranslationChanged: {
                const dx = activeTranslation.x - last.x, dy = activeTranslation.y - last.y
                if (page.moveMode)
                    view.pan(dx, dy)
                else
                    view.orbit(dx, dy)
                last = Qt.point(activeTranslation.x, activeTranslation.y)
            }
        }
        DragHandler {
            target: null
            acceptedButtons: Qt.RightButton | Qt.MiddleButton
            property point last
            onActiveChanged: last = Qt.point(0, 0)
            onActiveTranslationChanged: {
                view.pan(activeTranslation.x - last.x, activeTranslation.y - last.y)
                last = Qt.point(activeTranslation.x, activeTranslation.y)
            }
        }
        PinchHandler {
            target: null
            minimumPointCount: 2
            maximumPointCount: 2
            property real lastScale: 1
            property point lastCentroid
            onActiveChanged: {
                lastScale = 1
                lastCentroid = centroid.position
            }
            onActiveScaleChanged: {
                if (!active)
                    return
                view.zoom(activeScale / lastScale)
                lastScale = activeScale
            }
            onCentroidChanged: {
                if (!active)
                    return
                view.pan(centroid.position.x - lastCentroid.x, centroid.position.y - lastCentroid.y)
                lastCentroid = centroid.position
            }
        }
        TapHandler {
            acceptedButtons: Qt.LeftButton
            onDoubleTapped: (eventPoint) => {
                hint.text = view.focusAt(eventPoint.position.x, eventPoint.position.y)
                            ? qsTr("Rotation centre set") : qsTr("Nothing there - double-tap on the model")
                hintTimer.restart()
            }
        }
        WheelHandler {
            target: null
            onWheel: (event) => view.zoom(Math.pow(1.0015, event.angleDelta.y))
        }
    }

    BusyIndicator {
        anchors.centerIn: parent
        running: view.loading
    }

    Label {
        anchors.centerIn: parent
        width: parent.width * 0.8
        visible: view.error.length > 0
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
        color: "#ef5350"
        text: view.error
    }

    Label {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 12
        visible: view.splatCount > 0
        opacity: 0.7
        font.pixelSize: 12
        text: (view.isSplat ? qsTr("%1 Gaussians") : qsTr("%1 points")).arg(view.splatCount.toLocaleString(Qt.locale(), 'f', 0))
              + (AppInfo.isMobile ? qsTr("  ·  double-tap: set rotation centre")
                                   : qsTr("  ·  drag: rotate  ·  right-drag: move  ·  wheel: zoom  ·  double-click: set rotation centre"))
    }

    // ------------------------------------------------------------------ view controls
    ColumnLayout {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 8
        spacing: 4
        visible: view.splatCount > 0

        Button {
            text: page.moveMode ? qsTr("Move") : qsTr("Rotate")
            checkable: true
            checked: page.moveMode
            onClicked: page.moveMode = checked
            Layout.fillWidth: true
            ToolTip.visible: hovered
            ToolTip.text: qsTr("What dragging with one finger does")
        }
        Button { text: qsTr("Center"); flat: true; Layout.fillWidth: true; onClicked: view.centerOnObject() }
        Button { text: qsTr("Front"); flat: true; Layout.fillWidth: true; onClicked: view.viewPreset(0) }
        Button { text: qsTr("Side"); flat: true; Layout.fillWidth: true; onClicked: view.viewPreset(1) }
        Button { text: qsTr("Top"); flat: true; Layout.fillWidth: true; onClicked: view.viewPreset(2) }
    }

    Label {
        id: hint
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 12
        padding: 8
        visible: hintTimer.running
        background: Rectangle { color: "#b0000000"; radius: 6 }
    }
    Timer { id: hintTimer; interval: 1800 }

    // ------------------------------------------------------------------ cropping
    FileDialog {
        id: saveDialog
        fileMode: FileDialog.SaveFile
        defaultSuffix: "ply"
        nameFilters: [qsTr("Gaussian splat (*.ply)")]
        onAccepted: view.saveCropped(selectedFile)
    }

    Connections {
        target: view
        function onCropSaved(ok, count, error) {
            cropMessage.text = ok ? qsTr("Saved %1 Gaussians.").arg(count) : qsTr("Crop failed: %1").arg(error)
            page.cropRevision += 1
            if (ok && page.projectPath.length > 0) {
                view.cropEnabled = false
                view.reload()
                window.projects.refresh()
            }
        }
    }

    function applyCrop() {
        if (page.projectPath.length > 0) {
            window.projects.backupResult(page.projectPath) // the first crop keeps the original
            view.saveCropped(page.source)
        } else {
            saveDialog.open()
        }
    }

    Pane {
        id: cropPanel
        visible: view.cropEnabled
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        opacity: 0.94

        ColumnLayout {
            anchors.fill: parent
            spacing: 2

            Label {
                text: qsTr("Keep %1 of %2 Gaussians").arg(view.keptCount).arg(view.splatCount)
                font.bold: true
            }

            Repeater {
                model: [{ axis: "x", name: "X" }, { axis: "y", name: "Y (up)" }, { axis: "z", name: "Z" }]
                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    Label { text: modelData.name; Layout.preferredWidth: 48 }
                    RangeSlider {
                        Layout.fillWidth: true
                        from: view.boundsMin[modelData.axis]
                        to: view.boundsMax[modelData.axis]
                        first.value: view.cropMin[modelData.axis]
                        second.value: view.cropMax[modelData.axis]
                        first.onMoved: {
                            const v = Qt.vector3d(view.cropMin.x, view.cropMin.y, view.cropMin.z)
                            v[modelData.axis] = first.value
                            view.cropMin = v
                        }
                        second.onMoved: {
                            const v = Qt.vector3d(view.cropMax.x, view.cropMax.y, view.cropMax.z)
                            v[modelData.axis] = second.value
                            view.cropMax = v
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label { text: qsTr("Remove faint"); Layout.preferredWidth: 96 }
                Slider {
                    Layout.fillWidth: true
                    from: 0
                    to: 0.5
                    value: view.minOpacity
                    onMoved: view.minOpacity = value
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Button { text: qsTr("Auto"); onClicked: view.suggestCrop() }
                Button {
                    text: qsTr("Apply")
                    highlighted: true
                    enabled: !view.saving && view.keptCount > 0
                    onClicked: page.applyCrop()
                }
                Button { text: qsTr("Cancel"); flat: true; onClicked: view.cropEnabled = false }
                Item { Layout.fillWidth: true }
                BusyIndicator { running: view.saving; visible: running; Layout.preferredHeight: 32 }
            }

            Button {
                visible: page.cropRevision >= 0 && page.projectPath.length > 0
                         && window.projects.hasFullBackup(page.projectPath)
                text: qsTr("Restore original (uncropped)")
                flat: true
                onClicked: {
                    if (window.projects.restoreFullResult(page.projectPath)) {
                        cropMessage.text = qsTr("Original restored.")
                        view.reload()
                    }
                }
            }

            Label {
                id: cropMessage
                visible: text.length > 0
                color: Material.accent
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }
    }
}
