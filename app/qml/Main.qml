import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import U3D

ApplicationWindow {
    id: window
    width: AppInfo.isMobile ? 420 : 1200
    height: AppInfo.isMobile ? 860 : 800
    visible: true
    title: qsTr("U3D - Universal 3D Reconstruction")

    Material.theme: Material.Dark
    Material.accent: Material.Cyan
    Material.primary: "#1e2a38"

    // Shared state for all pages.
    property ProjectModel projects: ProjectModel {}
    property ReconstructionController reconstruction: ReconstructionController {}

    Component { id: projectPage; ProjectPage {} }
    Component { id: capturePage; CapturePage {} }
    Component { id: reconstructPage; ReconstructPage {} }
    Component { id: viewerPage; ViewerPage {} }

    function openProject(path) { stack.push(projectPage, { projectPath: path }) }
    function openCapture(path) { stack.push(capturePage, { projectPath: path }) }
    function openReconstruct(path) { stack.push(reconstructPage, { projectPath: path }) }
    function openViewer(url, title) { stack.push(viewerPage, { source: url, title: title }) }

    StackView {
        id: stack
        anchors.fill: parent
        initialItem: HomePage {}
    }

    // Android back button: go back one page before leaving the app.
    onClosing: (close) => {
        if (AppInfo.isMobile && stack.depth > 1) {
            close.accepted = false
            stack.pop()
        }
    }

    Component.onCompleted: {
        if (startupFile.toString() !== "")
            openViewer(startupFile, startupFile.toString().split("/").pop())
    }
}
