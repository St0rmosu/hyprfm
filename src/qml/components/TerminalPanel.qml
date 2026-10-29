import QtQuick
import HyprFM

// The built-in terminal at the bottom of the window. Only in the module when
// HyprFM is built with HYPRFM_TERMINAL; Main.qml loads it by URL.
Item {
    id: panel

    // The shell of the tab in front.
    readonly property var terminal: currentTab ? currentTab.view : null
    property int currentIndex: 0
    property Item currentTab: null
    // Shortcuts that still reach the window while the shell has focus.
    property var windowShortcuts: []
    // Tallest the handle can drag it; the window sets this.
    property real maximumHeight: 600

    implicitHeight: 260

    signal finished()
    signal unsafePaste(string text)
    signal linkActivated(string url)
    signal folderActivated(string path)

    // Tabs: Ctrl+Shift+T opens one in the shell's folder, Ctrl+Shift+W or
    // the shell exiting closes it, Ctrl+PageUp/PageDown switch. The bar
    // only shows with two or more; the last one closing closes the panel.
    ListModel { id: tabs }
    property int nextTabId: 0
    Component.onCompleted: tabs.append({ tabId: nextTabId++ })

    function showTab(index) {
        currentIndex = Math.max(0, Math.min(tabs.count - 1, index))
        currentTab = tabRepeater.itemAt(currentIndex)
        if (currentTab)
            currentTab.view.forceActiveFocus()
    }

    function newTab() {
        const from = terminal
        tabs.append({ tabId: nextTabId++ })
        showTab(tabs.count - 1)
        const view = currentTab.view
        view.workingDirectory = from ? from.currentDirectory() : ""
        view.start()
    }

    function closeTab(tab) {
        const index = tabRepeater.indexOfTab(tab)
        if (index < 0)
            return
        if (tabs.count === 1) {
            panel.finished()
            return
        }
        // Not from inside the closing shell's own signal.
        Qt.callLater(() => {
            const at = tabRepeater.indexOfTab(tab)
            if (at < 0)
                return
            tabs.remove(at)
            showTab(at <= currentIndex ? currentIndex - 1 : currentIndex)
        })
    }

    function switchTab(step) {
        if (tabs.count > 1)
            showTab((currentIndex + step + tabs.count) % tabs.count)
    }

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


    // The bar sits on the terminal's own background.
    Rectangle {
        anchors { left: parent.left; right: parent.right; top: handle.bottom; bottom: tabBar.bottom; bottomMargin: -2 }
        visible: tabBar.visible
        color: Theme.containerColor(Theme.base, 0.65)
    }

    Row {
        id: tabBar
        objectName: "terminalTabBar"
        visible: tabs.count > 1
        anchors { left: parent.left; right: parent.right; top: handle.bottom; leftMargin: 4; topMargin: 4 }
        height: visible ? 26 : 0
        spacing: 2

        Repeater {
            model: tabs
            delegate: Rectangle {
                id: tabButton
                required property int index
                readonly property Item tab: { tabRepeater.count; return tabRepeater.itemAt(index) }
                readonly property bool current: index === panel.currentIndex
                width: Math.min(180, (tabBar.width - addButton.width) / tabs.count - tabBar.spacing)
                height: tabBar.height
                radius: Theme.radiusSmall
                color: current ? Qt.rgba(Theme.text.r, Theme.text.g, Theme.text.b, 0.1)
                    : tabMouse.containsMouse ? Qt.rgba(Theme.text.r, Theme.text.g, Theme.text.b, 0.05)
                    : "transparent"
                Accessible.role: Accessible.PageTab
                Accessible.name: tabTitle.text

                MouseArea {
                    id: tabMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                    onClicked: (mouse) => {
                        if (mouse.button === Qt.MiddleButton)
                            panel.closeTab(tabButton.tab)
                        else
                            panel.showTab(tabButton.index)
                    }
                }

                Text {
                    id: tabTitle
                    anchors { left: parent.left; right: tabClose.left; leftMargin: 8; rightMargin: 4; verticalCenter: parent.verticalCenter }
                    // The program's title (OSC 2), else the shell's folder.
                    text: {
                        const view = tabButton.tab ? tabButton.tab.view : null
                        if (!view)
                            return ""
                        if (view.title)
                            return view.title
                        const dir = view.shellDirectory || view.workingDirectory
                        return dir ? (dir.split("/").filter(Boolean).pop() || "/") : "Shell"
                    }
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: tabButton.current ? Theme.text : Theme.subtext
                    font.pointSize: Theme.baseFontSize - 1
                }

                Rectangle {
                    id: tabClose
                    anchors { right: parent.right; rightMargin: 4; verticalCenter: parent.verticalCenter }
                    width: 18; height: 18
                    radius: Theme.radiusSmall
                    visible: tabButton.current || tabMouse.containsMouse || tabCloseMouse.containsMouse
                    color: tabCloseMouse.containsMouse ? Qt.rgba(Theme.text.r, Theme.text.g, Theme.text.b, 0.12) : "transparent"
                    Accessible.role: Accessible.Button
                    Accessible.name: "Close tab (Ctrl+Shift+W)"
                    IconX { anchors.centerIn: parent; size: 12; color: Theme.subtext }
                    MouseArea {
                        id: tabCloseMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: panel.closeTab(tabButton.tab)
                    }
                }
            }
        }

        Rectangle {
            id: addButton
            width: 26; height: tabBar.height
            radius: Theme.radiusSmall
            color: addMouse.containsMouse ? Qt.rgba(Theme.text.r, Theme.text.g, Theme.text.b, 0.08) : "transparent"
            Accessible.role: Accessible.Button
            Accessible.name: "New tab (Ctrl+Shift+T)"
            IconPlus { anchors.centerIn: parent; size: 14; color: Theme.subtext }
            MouseArea {
                id: addMouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: panel.newTab()
            }
        }
    }

    Repeater {
        id: tabRepeater
        model: tabs
        // One shell with its own scrollbar, find bar and link label.
        delegate: Item {
            id: tab
            required property int index
            property alias view: view
            anchors { left: panel.left; right: panel.right; top: tabBar.visible ? tabBar.bottom : handle.bottom; bottom: panel.bottom; topMargin: tabBar.visible ? 2 : 0 }
            visible: index === panel.currentIndex

            TerminalView {
                id: view
                anchors.fill: parent
                foreground: Theme.text
                // Same translucency as the file view, so Hyprland blurs behind it.
                background: Theme.containerColor(Theme.base, 0.65)
                ansiPalette: Theme.terminalPalette
                fontFamily: config.terminalFontFamily
                fontSize: config.terminalFontSize
                ligatures: config.terminalLigatures
                cursorStyle: config.terminalCursorStyle
                cursorBlink: config.terminalCursorBlink
                scrollbackLines: config.terminalScrollbackLines
                windowShortcuts: panel.windowShortcuts
                onFinished: panel.closeTab(tab)
                onUnsafePaste: (text) => panel.unsafePaste(text)
                onLinkActivated: (url) => panel.linkActivated(url)
                onFolderActivated: (path) => panel.folderActivated(path)
                matchColor: Theme.warning
                onSearchRequested: tab.openFind()
                onNewTabRequested: panel.newTab()
                onCloseTabRequested: panel.closeTab(tab)
                onSwitchTabRequested: (step) => panel.switchTab(step)
            }

            // Overlay scrollbar on the right edge. It shows while the view is up
            // in the scrollback (a reminder that new output is below), while the
            // mouse is near the edge, and while dragging; at the prompt it hides.
            Item {
                id: scrollbar
                objectName: "terminalScrollbar"
                readonly property bool scrollable: view.scrollTotal > view.scrollLength
                readonly property bool inHistory: view.scrollOffset + view.scrollLength < view.scrollTotal
                anchors { right: parent.right; top: parent.top; bottom: parent.bottom; topMargin: 3; bottomMargin: 3 }
                width: 12
                visible: scrollable
                opacity: inHistory || scrollMouse.containsMouse || scrollMouse.pressed ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: Theme.animDurationFast } }

                Rectangle {
                    id: thumb
                    readonly property real ratio: view.scrollTotal > 0 ? view.scrollLength / view.scrollTotal : 1
                    anchors.right: parent.right
                    anchors.rightMargin: 2
                    width: scrollMouse.containsMouse || scrollMouse.pressed ? 8 : 4
                    height: Math.max(24, scrollbar.height * ratio)
                    y: view.scrollTotal > view.scrollLength
                       ? (scrollbar.height - height) * view.scrollOffset / (view.scrollTotal - view.scrollLength) : 0
                    radius: width / 2
                    color: Qt.rgba(Theme.text.r, Theme.text.g, Theme.text.b, scrollMouse.pressed ? 0.55 : 0.35)
                    Behavior on width { NumberAnimation { duration: Theme.animDurationFast } }
                }

                MouseArea {
                    id: scrollMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: scrollbar.scrollable
                    property real grabOffset: 0
                    // Grabbing the thumb keeps it under the pointer; pressing the
                    // track centres the thumb there.
                    onPressed: (mouse) => {
                        grabOffset = mouse.y >= thumb.y && mouse.y <= thumb.y + thumb.height
                            ? mouse.y - thumb.y : thumb.height / 2
                        scrollTo(mouse.y)
                    }
                    onPositionChanged: (mouse) => { if (pressed) scrollTo(mouse.y) }
                    function scrollTo(y) {
                        const travel = scrollbar.height - thumb.height
                        const fraction = travel > 0 ? Math.max(0, Math.min(1, (y - grabOffset) / travel)) : 0
                        view.scrollToRow(Math.round(fraction * (view.scrollTotal - view.scrollLength)))
                    }
                }
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
                            tab.closeFind()
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
                                        tab.closeFind()
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
        onItemAdded: (index, item) => { if (index === panel.currentIndex) panel.currentTab = item }
        function indexOfTab(tab) {
            for (let i = 0; i < count; ++i) {
                if (itemAt(i) === tab)
                    return i
            }
            return -1
        }
    }
}
