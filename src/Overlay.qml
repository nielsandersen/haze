import QtQuick
import QtQuick.Shapes

// The haze on one screen: a tint over everything, with a hole where the
// focused window is. Hyprland blurs what is behind the tint and leaves the
// hole alone. The same file is the haze under the bar: there it tints only
// the edges the bar reserves, below the bar's own clock and icons.
Item {
    id: root

    required property string screenName
    // Under the windows and the bar, rather than over the windows.
    property bool under: false
    readonly property var edges: hyprland.reserved[screenName] || { left: 0, top: 0, right: 0, bottom: 0 }
    readonly property color fill: Qt.rgba(tint.r, tint.g, tint.b, strength)

    // The focused window's rectangle on this screen, or undefined when it is
    // on another one.
    readonly property var hole: hyprland.holes[screenName]

    // Hyprland draws the border just outside the window, so the hole takes it in.
    readonly property real grow: hyprland.borderSize
    property real holeX: 0
    property real holeY: 0
    property real holeWidth: 0
    property real holeHeight: 0

    // The hole glides from one window to the next, but appears in place when
    // it comes from nowhere.
    property bool glide: false
    property bool wasHoled: false

    readonly property color tint: backend.tint === "black" ? "#000000"
                                : backend.tint === "white" ? "#ffffff"
                                : backend.tint === "accent" ? theme.accent
                                : theme.background
    // A fully see-through haze is not blurred behind, so one that blurs keeps
    // a trace of tint even at no dim. Every haze surface fades by the one
    // `haze.reveal`, so they come and go as a single motion, and the blur
    // behind them all arrives on the same frame.
    readonly property real reveal: haze ? haze.reveal : 1
    readonly property real strength: backend.blur ? Math.max(backend.dim * reveal, reveal > 0 ? 0.012 : 0)
                                                  : backend.dim * reveal

    // Reads the hole itself, not `holed`: when the hole changes, this runs
    // before the bindings that depend on it have caught up.
    function place() {
        const next = hole
        const has = next !== undefined
        glide = has && wasHoled
        if (has) {
            holeX = next.x - grow
            holeY = next.y - grow
            holeWidth = next.width + 2 * grow
            holeHeight = next.height + 2 * grow
        } else {
            holeWidth = 0
            holeHeight = 0
        }
        wasHoled = has
    }

    onHoleChanged: place()
    onGrowChanged: place()
    Component.onCompleted: place()

    Behavior on holeX { enabled: root.glide; NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
    Behavior on holeY { enabled: root.glide; NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
    Behavior on holeWidth { enabled: root.glide; NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
    Behavior on holeHeight { enabled: root.glide; NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }

    // The strips behind the bars: across the top and bottom, and down the
    // sides between them.
    Item {
        anchors.fill: parent
        visible: root.under

        Rectangle { objectName: "topEdge"; width: parent.width; height: root.edges.top; color: root.fill }
        Rectangle { y: parent.height - height; width: parent.width; height: root.edges.bottom; color: root.fill }
        Rectangle {
            y: root.edges.top
            width: root.edges.left
            height: parent.height - root.edges.top - root.edges.bottom
            color: root.fill
        }
        Rectangle {
            x: parent.width - width
            y: root.edges.top
            width: root.edges.right
            height: parent.height - root.edges.top - root.edges.bottom
            color: root.fill
        }
    }

    Shape {
        anchors.fill: parent
        visible: !root.under
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: "transparent"
            fillColor: root.fill
            // Odd-even: the inner rectangle is cut out of the outer one.
            fillRule: ShapePath.OddEvenFill

            PathRectangle {
                width: root.width
                height: root.height
            }
            PathRectangle {
                x: root.holeX
                y: root.holeY
                width: root.holeWidth
                height: root.holeHeight
                radius: root.holeWidth > 0 ? theme.radius + (theme.radius > 0 ? root.grow : 0) : 0
            }
        }
    }
}
