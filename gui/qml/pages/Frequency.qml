import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

Page {
    id: page
    padding: 20

    ColumnLayout {
        anchors.fill: parent
        spacing: 20

        Label {
            text: "Manual Frequency Override"
            font.pixelSize: 32
            font.bold: true
            color: "#e94560"
            Layout.alignment: Qt.AlignHCenter
        }

        Label {
            text: "Lock CPU/GPU frequency to a fixed value.\nSet to 0 to release back to auto-scaling."
            font.pixelSize: 14
            color: "#a0a0b0"
            Layout.alignment: Qt.AlignHCenter
            wrapMode: Text.WordWrap
        }

        // Enable toggle
        CheckBox {
            id: enableToggle
            text: "Override Enabled"
            checked: false
            font.pixelSize: 18
            font.bold: true
            Layout.alignment: Qt.AlignHCenter
        }

        // CPU Prime cluster
        GroupBox {
            title: "CPU Prime (cpu0)"
            Layout.fillWidth: true

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12

                Label {
                    text: "Current: " + (root.modeProxy.frequencies.length > 0 ?
                        Math.round(root.modeProxy.frequencies[0] / 1000 * 100) / 100 + " MHz" : "-- MHz")
                    font.pixelSize: 16
                    color: "#4ecca3"
                }

                Slider {
                    id: primeSlider
                    from: 600000; to: 3200000
                    value: 2400000
                    stepSize: 50000
                    Layout.fillWidth: true
                }

                RowLayout {
                    spacing: 16
                    Label {
                        text: Math.round(primeSlider.value / 1000000 * 100) / 100 + " GHz"
                        font.pixelSize: 16
                        font.bold: true
                        color: "#e94560"
                    }
                    Label {
                        text: "(0 = auto)"
                        font.pixelSize: 12
                        color: "#a0a0b0"
                    }
                }
            }
        }

        // CPU Performance cluster
        GroupBox {
            title: "CPU Performance (cpu1-2)"
            Layout.fillWidth: true

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12

                Label {
                    text: "Current: " + (root.modeProxy.frequencies.length > 1 ?
                        Math.round(root.modeProxy.frequencies[1] / 1000 * 100) / 100 + " MHz" : "-- MHz")
                    font.pixelSize: 16
                    color: "#4ecca3"
                }

                Slider {
                    id: perfSlider
                    from: 500000; to: 2800000
                    value: 2200000
                    stepSize: 50000
                    Layout.fillWidth: true
                }

                Label {
                    text: Math.round(perfSlider.value / 1000000 * 100) / 100 + " GHz"
                    font.pixelSize: 16
                    font.bold: true
                    color: "#e94560"
                    Layout.alignment: Qt.AlignHCenter
                }
            }
        }

        // CPU Efficiency cluster
        GroupBox {
            title: "CPU Efficiency (cpu3-7)"
            Layout.fillWidth: true

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12

                Label {
                    text: "Current: " + (root.modeProxy.frequencies.length > 2 ?
                        Math.round(root.modeProxy.frequencies[2] / 1000 * 100) / 100 + " MHz" : "-- MHz")
                    font.pixelSize: 16
                    color: "#4ecca3"
                }

                Slider {
                    id: effSlider
                    from: 300000; to: 2000000
                    value: 1600000
                    stepSize: 50000
                    Layout.fillWidth: true
                }

                Label {
                    text: Math.round(effSlider.value / 1000000 * 100) / 100 + " GHz"
                    font.pixelSize: 16
                    font.bold: true
                    color: "#e94560"
                    Layout.alignment: Qt.AlignHCenter
                }
            }
        }

        // GPU
        GroupBox {
            title: "GPU"
            Layout.fillWidth: true

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12

                Slider {
                    id: gpuSlider
                    from: 300000000; to: 1000000000
                    value: 600000000
                    stepSize: 10000000
                    Layout.fillWidth: true
                }

                Label {
                    text: Math.round(gpuSlider.value / 1000000 * 100) / 100 + " MHz"
                    font.pixelSize: 16
                    font.bold: true
                    color: "#e94560"
                    Layout.alignment: Qt.AlignHCenter
                }
            }
        }

        // Apply / Release buttons
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 10

            Button {
                text: "Apply"
                font.pixelSize: 18
                font.bold: true
                highlighted: true
                contentItem: Label {
                    text: parent.text
                    font: parent.font
                    color: "#1a1a2e"
                    horizontalAlignment: Qt.AlignHCenter
                    verticalAlignment: Qt.AlignVCenter
                }
                background: Rectangle {
                    radius: 12
                    color: parent.hovered ? "#e94560" : "#4ecca3"
                }
                onClicked: {
                    if (!enableToggle.checked) {
                        statusMsg.text = "Please enable override first"
                        statusMsg.color = "#ffa500"
                        return
                    }
                    root.modeProxy.applyFreqOverride(
                        primeSlider.value,
                        perfSlider.value,
                        effSlider.value,
                        gpuSlider.value
                    )
                    statusMsg.text = "Frequency overrides applied!"
                    statusMsg.color = "#4ecca3"
                }
                Layout.preferredWidth: 160
            }

            Button {
                text: "Release All"
                font.pixelSize: 18
                font.bold: true
                contentItem: Label {
                    text: parent.text
                    font: parent.font
                    color: "#1a1a2e"
                    horizontalAlignment: Qt.AlignHCenter
                    verticalAlignment: Qt.AlignVCenter
                }
                background: Rectangle {
                    radius: 12
                    color: parent.hovered ? "#ff6b6b" : "#666680"
                }
                onClicked: {
                    root.modeProxy.releaseFreqOverride()
                    enableToggle.checked = false
                    statusMsg.text = "All overrides released — resumed auto-scaling"
                    statusMsg.color = "#ffa500"
                }
                Layout.preferredWidth: 160
            }
        }

        Label {
            id: statusMsg
            Layout.alignment: Qt.AlignHCenter
            font.pixelSize: 14
            color: "#a0a0b0"
        }
    }
}
