// SPDX-FileCopyrightText: 2026 Mikalai Kaliaha
// SPDX-License-Identifier: GPL-3.0-or-later
// Explicit geometry: Basic Switch has zero implicit width when contentItem is null.
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigamiaddons.formcard as FormCard

FormCard.AbstractFormDelegate {
    id: root
    property string description: ""
    property bool value: false
    signal moved(bool wanted)
    checkable: true
    focusPolicy: Qt.StrongFocus
    horizontalPadding: 16
    verticalPadding: 14
    implicitHeight: Math.max(56, contentItem.implicitHeight + topPadding + bottomPadding)
    onToggled: root.moved(checked)
    Accessible.role: Accessible.CheckBox
    Accessible.description: root.description
    Binding { target: root; property: "checked"; value: root.value; restoreMode: Binding.RestoreNone }

    contentItem: RowLayout {
        spacing: 20
        ColumnLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: 5
            Controls.Label {
                text: root.text
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: root.enabled ? "#F4F6FB" : "#6E7995"
                Accessible.ignored: true
            }
            Controls.Label {
                visible: root.description !== ""
                text: root.description
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: "#AAB4CC"
                Accessible.ignored: true
            }
        }
        Controls.Switch {
            id: toggle
            objectName: "checkControl"
            Layout.minimumWidth: 46
            Layout.preferredWidth: 46
            Layout.maximumWidth: 46
            implicitWidth: 46
            implicitHeight: 28
            padding: 0
            spacing: 0
            focusPolicy: Qt.NoFocus
            enabled: root.enabled
            onToggled: root.moved(checked)
            Accessible.ignored: true
            Binding { target: toggle; property: "checked"; value: root.checked; restoreMode: Binding.RestoreNone }
            contentItem: Item { implicitWidth: 0; implicitHeight: 0 }
            indicator: Rectangle {
                implicitWidth: 46
                implicitHeight: 26
                x: (toggle.width-width)/2
                y: (toggle.height-height)/2
                radius: 13
                color: toggle.checked ? "#1E6FE8" : "#25314A"
                border.width: root.activeFocus ? 2 : 1
                border.color: root.activeFocus ? "#8DB7FF" : toggle.checked ? "#5B8CFF" : "#3A4768"
                opacity: root.enabled ? 1 : 0.45
                Rectangle {
                    width: 20; height: 20; radius: 10
                    x: 3 + toggle.visualPosition * (parent.width-width-6)
                    y: 3
                    color: "#F4F6FB"
                    Behavior on x { NumberAnimation { duration: 100 } }
                }
            }
        }
    }
}
