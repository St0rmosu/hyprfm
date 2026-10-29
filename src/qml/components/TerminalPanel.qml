import QtQuick
import HyprFM

// The built-in terminal at the bottom of the window. Only in the module when
// HyprFM is built with HYPRFM_TERMINAL; Main.qml loads it by URL.
Item {
    id: panel

    property alias terminal: view
    // Shortcuts that still reach the window while the shell has focus.
    property var windowShortcuts: []
    // Tallest the handle can drag it; the window sets this.
    property real maximumHeight: 600

    implicitHeight: 260

    signal finished()
    signal unsafePaste(string text)
    signal linkActivated(string url)
    signal folderActivated(string path)

    // Drag handle on the top edge
    Rectangle {
        id: handle
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: 1
        color: Qt.rgba(Theme.text.r, Theme.text.g, Theme.text.b, 0.12)

        MouseArea {
            anchors { fill: parent; topMargin: -3; bottomMargin: -3 }
            cursorShape: Qt.SizeVerCursor
            property real startY
            property real startHeight
            onPressed: (mouse) => {
                startY = mapToItem(null, 0, mouse.y).y
                startHeight = panel.height
            }
            onPositionChanged: (mouse) => {
                const delta = startY - mapToItem(null, 0, mouse.y).y
                panel.implicitHeight = Math.max(80, Math.min(panel.maximumHeight, startHeight + delta))
            }
        }
    }

    TerminalView {
        id: view
        anchors { fill: parent; topMargin: 1 }
        foreground: Theme.text
        // Same translucency as the file view, so Hyprland blurs behind it.
        background: Theme.containerColor(Theme.base, 0.65)
        ansiPalette: Theme.terminalPalette
        fontFamily: config.terminalFontFamily
        fontSize: config.terminalFontSize
        ligatures: config.terminalLigatures
        cursorStyle: config.terminalCursorStyle
        cursorBlink: config.terminalCursorBlink
        windowShortcuts: panel.windowShortcuts
        onFinished: panel.finished()
        onUnsafePaste: (text) => panel.unsafePaste(text)
        onLinkActivated: (url) => panel.linkActivated(url)
        onFolderActivated: (path) => panel.folderActivated(path)
    }

    // Where the link under the mouse really goes, as a browser shows it:
    // an OSC 8 link's text can say anything.
    Rectangle {
        objectName: "terminalLinkStatus"
        visible: view.hoveredLink !== ""
        anchors { left: parent.left; bottom: parent.bottom; margins: 4 }
        width: Math.min(linkText.implicitWidth + 12, parent.width - 8)
        height: linkText.implicitHeight + 6
        radius: Theme.radiusSmall
        color: Theme.mantle
        border.color: Theme.overlay

        Text {
            id: linkText
            anchors { fill: parent; leftMargin: 6; rightMargin: 6 }
            verticalAlignment: Text.AlignVCenter
            text: view.hoveredLink + "  ·  Ctrl+click to open"
            textFormat: Text.PlainText
            elide: Text.ElideMiddle
            color: Theme.subtext
            font.pointSize: Theme.baseFontSize - 1
        }
    }
}
