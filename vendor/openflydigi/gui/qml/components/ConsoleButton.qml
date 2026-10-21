// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
Controls.Button {
    id: control
    property bool primary: objectName === "saveButton" || objectName === "screenUploadButton"
    implicitHeight: 38
    implicitWidth: Math.max(100, contentItem.implicitWidth + 30)
    leftPadding: 15
    rightPadding: 15
    topPadding: 8
    bottomPadding: 8
    background: Rectangle {
        radius: 6
        color: !control.enabled ? "#141C30" : control.down ? "#124A9E" : control.primary ? "#1E6FE8" : control.hovered ? "#23365C" : "#1B2540"
        border.width: control.visualFocus ? 2 : 1
        border.color: control.visualFocus ? "#8DB7FF" : control.primary ? "#1E6FE8" : "#3A4768"
    }
    contentItem: Text {
        text: control.text
        font: control.font
        color: control.enabled ? "#F4F6FB" : "#6E7995"
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}
