import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 520
    height: 720
    minimumWidth: 380
    minimumHeight: 420
    visible: true
    title: "Haze"
    color: theme.background

    Material.theme: theme.light ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.background: theme.background
    Material.foreground: theme.foreground

    readonly property bool rightToLeft: Qt.application.layoutDirection === Qt.RightToLeft
    readonly property color muted: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.6)

    // Reads the language so the digits follow a switch.
    function percent(value) {
        translator.language
        return qsTr("%1%").arg(Math.round(value * 100).toLocaleString(Qt.locale(), 'f', 0))
    }

    function tintName(key) {
        switch (key) {
        case "black": return qsTr("Black")
        case "white": return qsTr("White")
        case "accent": return qsTr("Accent")
        default: return qsTr("Theme")
        }
    }

    function tintColor(key) {
        switch (key) {
        case "black": return "#000000"
        case "white": return "#ffffff"
        case "accent": return theme.accent
        default: return theme.background
        }
    }

    Shortcut { sequence: "?"; onActivated: help.opened ? help.close() : help.open() }
    Shortcut { sequence: "Q"; onActivated: Qt.quit() }
    Shortcut { sequence: "Escape"; enabled: !help.opened; onActivated: win.close() }
    Shortcut { sequence: "Space"; onActivated: backend.toggle() }
    Shortcut { sequence: "B"; onActivated: backend.blur = !backend.blur }
    Shortcut { sequence: "C"; onActivated: backend.customBlur = !backend.customBlur }
    Shortcut { sequence: "T"; onActivated: backend.nextTint() }
    Shortcut { sequence: "["; onActivated: backend.dim = backend.dim - 0.05 }
    Shortcut { sequence: "]"; onActivated: backend.dim = backend.dim + 0.05 }
    Shortcut { sequence: "-"; onActivated: backend.strength = backend.strength - 1 }
    Shortcut { sequences: ["=", "+"]; onActivated: backend.strength = backend.strength + 1 }

    component SectionTitle: Label {
        Layout.fillWidth: true
        Layout.topMargin: 12
        color: win.muted
        font.pixelSize: 12
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 1
        elide: Text.ElideRight
    }

    // A named slider with its value at the far end.
    component Setting: ColumnLayout {
        id: setting
        property alias name: settingName.text
        property alias valueText: settingValue.text
        property alias slider: settingSlider
        Layout.fillWidth: true
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Label { id: settingName; Layout.fillWidth: true; elide: Text.ElideRight }
            Label { id: settingValue; color: win.muted }
        }
        Slider {
            id: settingSlider
            Layout.fillWidth: true
            focusPolicy: Qt.TabFocus
        }
    }

    // A switch with its caption before it, so the captions line up.
    component Toggle: RowLayout {
        property alias name: toggleName.text
        property alias control: toggleSwitch
        Layout.fillWidth: true

        Label { id: toggleName; Layout.fillWidth: true; wrapMode: Text.Wrap }
        Switch { id: toggleSwitch; focusPolicy: Qt.NoFocus }
    }

    component Note: Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: win.muted
        font.pixelSize: 13
    }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Flickable {
        anchors.fill: parent
        contentHeight: content.implicitHeight + 56
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        LayoutMirroring.enabled: win.rightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            id: content
            y: 28
            width: Math.min(parent.width - 56, 520)
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                spacing: 16

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label {
                        objectName: "state"
                        Layout.fillWidth: true
                        text: backend.enabled ? qsTr("Haze is on") : qsTr("Haze is off")
                        font.pixelSize: 28
                        font.bold: true
                        elide: Text.ElideRight
                    }
                    Note { text: qsTr("Blurs and dims everything but the window you are working in.") }
                }
                Switch {
                    objectName: "enabled"
                    focusPolicy: Qt.NoFocus
                    checked: backend.enabled
                    onToggled: backend.enabled = checked
                }
            }

            Note {
                objectName: "noHyprland"
                visible: !hyprland.available
                color: theme.colors.red
                text: qsTr("Haze needs Hyprland, and can't find it running.")
            }

            SectionTitle { text: qsTr("Dim") }

            Setting {
                name: qsTr("Darkness")
                valueText: win.percent(backend.dim)
                slider.objectName: "dim"
                slider.from: 0
                slider.to: 0.9
                slider.stepSize: 0.01
                slider.value: backend.dim
                slider.onMoved: backend.dim = slider.value
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Repeater {
                    model: backend.tints
                    delegate: ColumnLayout {
                        id: swatch
                        required property string modelData
                        readonly property bool current: backend.tint === modelData
                        Layout.fillWidth: true
                        spacing: 6

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 40
                            radius: Math.min(4, theme.radius)
                            color: win.tintColor(swatch.modelData)
                            border.width: swatch.current ? 2 : 1
                            border.color: swatch.current ? theme.accent : theme.colors.selection

                            // The accent swatch can't mark itself in its own color.
                            Rectangle {
                                visible: swatch.current
                                anchors.centerIn: parent
                                width: 10
                                height: 10
                                radius: 5
                                color: swatch.modelData === "accent" ? theme.accentForeground
                                     : swatch.modelData === "theme" ? theme.foreground
                                     : swatch.modelData === "white" ? "#000000" : "#ffffff"
                            }
                            TapHandler { onTapped: backend.tint = swatch.modelData }
                        }
                        Label {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            text: win.tintName(swatch.modelData)
                            color: swatch.current ? theme.foreground : win.muted
                            font.pixelSize: 13
                            elide: Text.ElideRight
                        }
                    }
                }
            }

            SectionTitle { text: qsTr("Blur") }

            Toggle {
                name: qsTr("Blur what is behind")
                control.objectName: "blur"
                control.checked: backend.blur
                control.onToggled: backend.blur = control.checked
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 10
                enabled: backend.blur
                opacity: enabled ? 1 : 0.4

                Toggle {
                    name: qsTr("Haze's own blur")
                    control.objectName: "customBlur"
                    control.checked: backend.customBlur
                    control.onToggled: backend.customBlur = control.checked
                }

                Note {
                    text: qsTr("Hyprland has one blur for the whole desktop, so while Haze is on, see-through windows and panels are blurred the same way.")
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    enabled: backend.customBlur
                    opacity: enabled ? 1 : 0.4

                    Setting {
                        name: qsTr("Strength")
                        valueText: backend.strength.toLocaleString(Qt.locale(), 'f', 0) + (translator.language, "")
                        slider.objectName: "strength"
                        slider.from: 1
                        slider.to: 10
                        slider.stepSize: 1
                        slider.snapMode: Slider.SnapAlways
                        slider.value: backend.strength
                        slider.onMoved: backend.strength = slider.value
                    }
                    Setting {
                        name: qsTr("Vibrancy")
                        valueText: win.percent(backend.vibrancy)
                        slider.objectName: "vibrancy"
                        slider.from: 0
                        slider.to: 1
                        slider.stepSize: 0.01
                        slider.value: backend.vibrancy
                        slider.onMoved: backend.vibrancy = slider.value
                    }
                    Setting {
                        name: qsTr("Grain")
                        valueText: win.percent(backend.grain)
                        slider.objectName: "grain"
                        slider.from: 0
                        slider.to: 1
                        slider.stepSize: 0.01
                        slider.value: backend.grain
                        slider.onMoved: backend.grain = slider.value
                    }
                }
            }

            Note {
                Layout.topMargin: 16
                text: qsTr("Haze stays in the status bar when this window is closed. Press Q to quit it, or ? for all shortcuts.")
            }
        }
    }

    Popup {
        id: help
        objectName: "help"
        anchors.centerIn: Overlay.overlay
        modal: true
        focus: true
        padding: 24
        background: Rectangle {
            color: theme.background
            border.color: theme.colors.selection
            radius: theme.radius
        }

        ColumnLayout {
            spacing: 8
            // A popup is not an Item and doesn't inherit the window's
            // mirroring, so its content opts in.
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true

            Repeater {
                model: [
                    { keys: "Space", action: qsTr("Turn the haze on or off") },
                    { keys: "[  ]", action: qsTr("Dim less or more") },
                    { keys: "T", action: qsTr("Next tint") },
                    { keys: "B", action: qsTr("Blur or only dim") },
                    { keys: "C", action: qsTr("Haze's own blur or the desktop's") },
                    { keys: "-  =", action: qsTr("Blur less or more") },
                    { keys: "?", action: qsTr("Show or hide the shortcuts") },
                    { keys: "Esc", action: qsTr("Close the window and keep the haze") },
                    { keys: "Q", action: qsTr("Quit") }
                ]
                delegate: RowLayout {
                    required property var modelData
                    spacing: 24

                    Label {
                        Layout.preferredWidth: 64
                        text: modelData.keys
                        color: theme.accent
                        font.bold: true
                    }
                    Label { text: modelData.action }
                }
            }
        }
    }
}
