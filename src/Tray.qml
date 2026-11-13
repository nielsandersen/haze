import QtQuick
import Qt.labs.platform

// Haze's icon in the status bar, there for as long as Haze runs: a click
// turns the haze on or off, and its menu has the rest.
SystemTrayIcon {
    id: tray
    visible: true
    // Drawn by Haze in the theme's foreground, like the bar's own icons, and
    // again whenever the theme changes.
    icon.source: "image://haze/" + (backend.enabled ? "on" : "off") + "/" + theme.foreground
    tooltip: backend.enabled ? qsTr("Haze is on") : qsTr("Haze is off")

    onActivated: function(reason) {
        if (reason === SystemTrayIcon.Trigger)
            backend.toggle()
        else if (reason === SystemTrayIcon.MiddleClick || reason === SystemTrayIcon.DoubleClick)
            backend.openSettings()
    }

    menu: Menu {
        MenuItem {
            objectName: "trayToggle"
            text: backend.enabled ? qsTr("Turn the haze off") : qsTr("Turn the haze on")
            onTriggered: backend.toggle()
        }
        MenuItem {
            objectName: "trayBlur"
            text: qsTr("Blur what is behind")
            checkable: true
            checked: backend.blur
            onTriggered: backend.blur = !backend.blur
        }
        MenuItem { separator: true }
        MenuItem {
            objectName: "traySettings"
            text: qsTr("Open Haze")
            onTriggered: backend.openSettings()
        }
        MenuItem {
            objectName: "trayQuit"
            text: qsTr("Quit")
            onTriggered: Qt.quit()
        }
    }
}
