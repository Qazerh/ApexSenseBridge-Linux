// SPDX-FileCopyrightText: 2026 Mikalai Kaliaha
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard as FormCard
import Apex5
import "../components"

Kirigami.ScrollablePage {
    id: page
    objectName: "devicesPage"
    title: "Устройства"
    ColumnLayout {
        spacing: 12
        ConsoleHeader {
            title: "Подключённые устройства"
            trailing: ConsoleButton {
                text: "Обновить"
                onClicked: App.devices.refresh()
            }
        }
        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            visible: App.devices.count === 0
            text: "Устройства не найдены"
            explanation: "Включите контроллер и подключите USB-кабель или приёмник. Док-станция должна быть подключена к компьютеру."
        }
        Repeater {
            model: App.devices
            delegate: ConsoleCard {
                id: card
                required property int index
                required property string label
                required property string kind
                required property string path
                required property bool supported
                required property int battery
                required property bool charging
                required property string firmware
                required property string model
                required property string uid
                required property string error
                FormCard.AbstractFormDelegate {
                    background: null
                    onClicked: { if (card.supported) App.devices.select(card.index); }
                    contentItem: RowLayout {
                        spacing: 20
                        DeviceArtwork {
                            deviceModel: card.model
                            deviceKind: card.kind
                            Layout.preferredWidth: 112
                            Layout.preferredHeight: 84
                            Layout.minimumWidth: 80
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            spacing: 6
                            Controls.Label {
                                text: card.label
                                font.bold: true
                                font.pixelSize: 16
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                            }
                            Controls.Label {
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                color: "#AAB4CC"
                                text: card.model + (card.firmware !== "" ? " · прошивка " + card.firmware : "")
                                    + (card.kind === "pad" ? (card.charging ? " · заряжается" : " · заряд " + card.battery + "/" + App.device.batterySteps) : "")
                            }
                            Controls.Label {
                                Layout.fillWidth: true
                                text: card.path + (card.uid !== "" ? " · UID " + card.uid : "")
                                color: "#6E7995"
                                font.pixelSize: 11
                                wrapMode: Text.WrapAnywhere
                            }
                            Controls.Label {
                                visible: card.error !== "" || !card.supported
                                Layout.fillWidth: true
                                text: card.error !== "" ? card.error : "Настройка этого устройства не поддерживается."
                                color: "#FF9999"
                                wrapMode: Text.Wrap
                            }
                        }
                        ConsoleButton {
                            text: card.index === App.devices.currentIndex ? "Выбрано" : "Выбрать"
                            enabled: card.supported && card.index !== App.devices.currentIndex
                            onClicked: App.devices.select(card.index)
                        }
                    }
                }
            }
        }
    }
}
