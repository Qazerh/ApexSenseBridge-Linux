// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import Apex5
Controls.Pane {
    id: root
    padding: 0
    palette.window: "#0B1225"
    palette.base: "#111A31"
    palette.alternateBase: "#17213A"
    palette.button: "#1B2540"
    palette.buttonText: "#F4F6FB"
    palette.text: "#F4F6FB"
    palette.windowText: "#F4F6FB"
    palette.highlight: "#1E6FE8"
    palette.highlightedText: "white"
    Kirigami.Theme.inherit: false
    Kirigami.Theme.colorSet: Kirigami.Theme.Window
    Kirigami.Theme.backgroundColor: "#0B1225"
    Kirigami.Theme.textColor: "#F4F6FB"
    Kirigami.Theme.highlightColor: "#1E6FE8"
    Kirigami.Theme.alternateBackgroundColor: "#111A31"
    function openPage(file) { loader.source = Qt.resolvedUrl("pages/" + file) }
    background: Rectangle { color: "#0B1225" }
    ColumnLayout {
        anchors.fill: parent
        spacing: 8
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            Layout.maximumWidth: Math.max(0, Math.min(880, root.width - 32))
            Layout.alignment: Qt.AlignHCenter
            type: Kirigami.MessageType.Error
            text: App.device.error
            visible: text !== ""
            showCloseButton: true
        }
        Loader {
            id: loader
            objectName: "configurationPage"
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
    }
}
