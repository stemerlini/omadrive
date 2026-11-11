import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Quick look at the highlighted item (Space). It does not take the keyboard,
// so the arrow keys keep moving through the pane and the preview follows.
Popup {
    id: popup

    anchors.centerIn: Overlay.overlay
    width: Math.round(parent.width * 0.72)
    height: Math.round(parent.height * 0.82)
    modal: false
    focus: false
    closePolicy: Popup.NoAutoClose
    padding: 14

    background: Rectangle {
        color: theme.surface
        radius: theme.radius
        border.width: 1
        border.color: theme.accent
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label {
                    Layout.fillWidth: true
                    text: preview.name
                    font.bold: true
                    font.pixelSize: 16
                    elide: Text.ElideMiddle
                    color: theme.accent
                }
                Label {
                    Layout.fillWidth: true
                    text: preview.details
                    elide: Text.ElideRight
                    color: theme.muted
                }
            }
            ToolButton {
                text: "✕"
                focusPolicy: Qt.NoFocus
                onClicked: popup.close()
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            BusyIndicator {
                anchors.centerIn: parent
                running: preview.kind === "loading"
                visible: running
            }

            Image {
                anchors.fill: parent
                visible: preview.kind === "image"
                source: visible ? preview.source : ""
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                cache: false
                mipmap: true
                // Never upscale small pictures past their own size.
                sourceSize.width: width * Screen.devicePixelRatio
                sourceSize.height: height * Screen.devicePixelRatio
            }

            ScrollView {
                anchors.fill: parent
                visible: preview.kind === "text"
                TextArea {
                    readOnly: true
                    text: preview.kind === "text" ? preview.text : ""
                    font.family: theme.fontFamily
                    wrapMode: TextEdit.NoWrap
                    selectByMouse: true
                    focusPolicy: Qt.NoFocus
                    background: null
                }
            }

            Loader {
                id: pdf
                anchors.fill: parent
                active: preview.kind === "pdf"
                source: "qrc:/PdfView.qml"
                onLoaded: item.source = Qt.binding(function () { return preview.source })
            }

            ListView {
                anchors.fill: parent
                visible: preview.kind === "folder"
                model: visible ? preview.entries : []
                clip: true
                interactive: true
                ScrollBar.vertical: ScrollBar {}
                delegate: Label {
                    required property string modelData
                    width: ListView.view.width
                    padding: 3
                    text: modelData
                    elide: Text.ElideRight
                    color: modelData.endsWith("/") ? theme.accent : theme.foreground
                }
            }

            Label {
                anchors.centerIn: parent
                width: parent.width * 0.8
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                visible: preview.kind === "info" || preview.kind === "error"
                         || (preview.kind === "pdf" && pdf.status === Loader.Error)
                         || (preview.kind === "folder" && preview.entries.length === 0)
                color: preview.kind === "error" ? theme.error : theme.muted
                text: preview.kind === "error" ? preview.details
                    : preview.kind === "folder" ? qsTr("This folder is empty")
                    : preview.kind === "pdf" ? qsTr("Install qt6-webengine to preview PDF files")
                    : qsTr("No preview for this kind of file.\nPress Enter to open it.")
            }
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("↑ ↓ next file   Enter open   Space or Esc close")
            color: theme.muted
            horizontalAlignment: Text.AlignHCenter
        }
    }
}
