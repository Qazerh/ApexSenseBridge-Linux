// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
Image {
    id: artwork
    property string deviceModel: ""
    property string deviceKind: "pad"
    objectName: "deviceArtwork"
    readonly property string imageFile: {
        const model = deviceModel.toLowerCase();
        if (deviceKind === "dock") return "device-dock.svg";
        if (model === "k4" || model.indexOf("apex 4") >= 0) return "controller-apex4.png";
        if (model === "k5" || model.indexOf("apex 5") >= 0) return "controller-apex5.png";
        if (model === "k6p" || model.indexOf("apex 6 pro") >= 0) return "controller-apex6-pro.png";
        return "device-controller.svg";
    }
    source: (typeof apexAssetBase !== "undefined" ? apexAssetBase : Qt.resolvedUrl("../../../../../gui/assets/")) + imageFile
    fillMode: Image.PreserveAspectFit
    sourceSize.width: 320
    sourceSize.height: 240
    asynchronous: false
    smooth: true
    cache: true
    Accessible.ignored: true
}
