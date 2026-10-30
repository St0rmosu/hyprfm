import QtQuick
import QtQuick.Window
import HyprFM
import Quill as Q

// Hover tooltip naming a control and its keyboard shortcut, if it has one,
// so the shortcuts can be learned from the buttons (#63). The tooltip is only
// built while shown, so the many idle buttons cost nothing.
Loader {
    id: root

    property string text: ""
    // A shortcut action from config.shortcutMap ("back") follows the user's
    // remapping; keys is for fixed keys ("Esc").
    property string action: ""
    property string keys: ""
    property bool shown: false

    readonly property string sequence: keys !== "" ? keys
        : (action !== "" && config.shortcutMap[action] ? config.shortcutMap[action] : "")

    // Set while the popup is on screen, so it stays loaded until it has
    // finished fading out. Reading item.visible here instead loops.
    property bool _onScreen: false

    active: text !== "" && (shown || _onScreen)
    sourceComponent: Q.Tooltip {
        parent: root.parent
        visible: root.shown
        onAboutToShow: root._onScreen = true
        onClosed: root._onScreen = false
        margins: 4
        // Same opaque surface as the menus and the sidebar device tooltip.
        background: Rectangle {
            color: Qt.rgba(Theme.crust.r, Theme.crust.g, Theme.crust.b, 1.0)
            radius: Theme.radiusSmall
            border.width: 1
            border.color: Qt.rgba(Theme.text.r, Theme.text.g, Theme.text.b, 0.14)
        }
        text: root.sequence !== "" ? root.text + "  (" + root.sequence + ")" : root.text
        // Below the control, or above it when the window ends first.
        y: {
            const below = parent.height + 6
            const bottom = parent.mapToItem(null, 0, below + implicitHeight).y
            return bottom > root.Window.height - 4 ? -implicitHeight - 6 : below
        }
    }
}
