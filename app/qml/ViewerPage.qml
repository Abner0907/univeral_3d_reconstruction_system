import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import U3D

Page {
    id: page
    property url source

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

        // One finger / left mouse: orbit.  Two fingers / right mouse: pan.  Pinch / wheel: zoom.
        DragHandler {
            id: orbitDrag
            target: null
            acceptedButtons: Qt.LeftButton
            maximumPointCount: 1
            property point last
            onActiveChanged: last = Qt.point(0, 0)
            onActiveTranslationChanged: {
                view.orbit(activeTranslation.x - last.x, activeTranslation.y - last.y)
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
            property real lastScale: 1
            property point lastCentroid
            onActiveChanged: { lastScale = 1; lastCentroid = centroid.position }
            onActiveScaleChanged: {
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
              + (AppInfo.isMobile ? "" : qsTr("  ·  drag: orbit  ·  right-drag: pan  ·  wheel: zoom"))
    }
}
