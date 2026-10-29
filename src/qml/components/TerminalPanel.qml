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
        matchColor: Theme.warning
        onSearchRequested: panel.openFind()
    }

    function openFind() {
        findBar.visible = true
        findInput.selectAll()
        findInput.forceActiveFocus()
    }

    function closeFind() {
        findBar.visible = false
        view.searchText = ""
        view.forceActiveFocus()
    }

    // Ctrl+Shift+F in the terminal. Enter goes up to older matches,
    // Shift+Enter back down, Escape closes and clears the highlights.
    Rectangle {
        id: findBar
        objectName: "terminalFindBar"
        visible: false
        anchors { right: parent.right; top: parent.top; margins: 6; topMargin: 7 }
        width: Math.min(340, parent.width - 12)
        height: 32
        radius: Theme.radiusSmall
        color: Theme.mantle
        border.color: findInput.activeFocus ? Theme.accent : Theme.overlay

        IconSearch {
            id: findIcon
            anchors { left: parent.left; leftMargin: 9; verticalCenter: parent.verticalCenter }
            size: 14
            color: Theme.muted
        }

        TextInput {
            id: findInput
            objectName: "terminalFindInput"
            anchors {
                left: findIcon.right; leftMargin: 7
                right: findCount.left; rightMargin: 8
                verticalCenter: parent.verticalCenter
            }
            clip: true
            color: Theme.text
            selectionColor: Theme.accent
            selectedTextColor: Theme.base
            font.pointSize: Theme.baseFontSize
            onTextEdited: view.searchText = text
            Keys.onPressed: (event) => {
                if (event.key === Qt.Key_Escape) {
                    panel.closeFind()
                    event.accepted = true
                } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                    if (event.modifiers & Qt.ShiftModifier)
                        view.findPrevious()
                    else
                        view.findNext()
                    event.accepted = true
                }
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: !findInput.text
                text: "Find in terminal"
                color: Theme.muted
                font: findInput.font
            }
        }

        Text {
            id: findCount
            anchors { right: findButtons.left; rightMargin: 4; verticalCenter: parent.verticalCenter }
            text: !findInput.text ? ""
                : view.searchMatchCount === 0 ? "No matches"
                : view.searchMatchIndex + " of " + view.searchMatchCount
            color: findInput.text && view.searchMatchCount === 0 ? Theme.error : Theme.subtext
            font.pointSize: Theme.baseFontSize - 1
        }

        Row {
            id: findButtons
            anchors { right: parent.right; rightMargin: 4; verticalCenter: parent.verticalCenter }
            spacing: 0

            Repeater {
                model: [
                    { icon: "up", tip: "Older match (Enter)" },
                    { icon: "down", tip: "Newer match (Shift+Enter)" },
                    { icon: "close", tip: "Close (Escape)" }
                ]
                delegate: Rectangle {
                    required property var modelData
                    width: 24; height: 24
                    radius: Theme.radiusSmall
                    color: findButtonMouse.containsMouse
                        ? Qt.rgba(Theme.text.r, Theme.text.g, Theme.text.b, 0.1) : "transparent"
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.tip

                    IconChevronUp {
                        visible: modelData.icon === "up"
                        anchors.centerIn: parent; size: 14; color: Theme.text
                    }
                    IconChevronDown {
                        visible: modelData.icon === "down"
                        anchors.centerIn: parent; size: 14; color: Theme.text
                    }
                    IconX {
                        visible: modelData.icon === "close"
                        anchors.centerIn: parent; size: 14; color: Theme.text
                    }

                    MouseArea {
                        id: findButtonMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            if (modelData.icon === "up")
                                view.findNext()
                            else if (modelData.icon === "down")
                                view.findPrevious()
                            else
                                panel.closeFind()
                        }
                    }
                }
            }
        }
    }

    // Where the link under the mouse really goes, as a browser shows it:
    // an OSC 8 link's text can say anything. Bottom right, off the prompt.
    Rectangle {
        objectName: "terminalLinkStatus"
        visible: view.hoveredLink !== ""
        anchors { right: parent.right; bottom: parent.bottom; margins: 4 }
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
