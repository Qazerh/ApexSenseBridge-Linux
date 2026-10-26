// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
Controls.Slider {
    id: control
    implicitWidth: 200
    implicitHeight: 32
    padding: 0
    background: Rectangle {
        x: control.leftPadding
        y: control.topPadding + (control.availableHeight-height)/2
        width: control.availableWidth
        height: 4
        radius: 2
        color: "#2A3552"
        Rectangle {
            x: control.mirrored ? parent.width-width : 0
            width: control.visualPosition * parent.width
            height: parent.height
            radius: 2
            color: "#1E6FE8"
        }
    }
    handle: Rectangle {
        width: 18; height: 18; radius: 9
        x: control.leftPadding + control.visualPosition * (control.availableWidth-width)
        y: control.topPadding + (control.availableHeight-height)/2
        color: control.enabled ? "#F4F6FB" : "#6E7995"
        border.width: control.visualFocus ? 2 : 1
        border.color: control.visualFocus ? "#5B8CFF" : "#AAB4CC"
    }
}
