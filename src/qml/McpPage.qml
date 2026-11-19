import BedrockMap 1.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "controls"

ScrollView {
    id: page
    clip: true
    contentWidth: availableWidth

    ColumnLayout {
        width: page.availableWidth
        spacing: Theme.gapMedium

        Text {
            Layout.fillWidth: true
            Layout.margins: Theme.gapLarge
            text: qsTr("Connect an MCP client on this device to BedrockMap using Streamable HTTP.")
            wrapMode: Text.Wrap
            color: Theme.text
            font.pixelSize: Theme.fontBody
        }
        AppSwitch {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.gapLarge
            Layout.rightMargin: Theme.gapLarge
            text: qsTr("Enable MCP server")
            enabled: App.mcp.supported
            checked: App.mcp.running
            onToggled: {
                if (checked) {
                    if (!App.mcp.start())
                        checked = App.mcp.running
                } else {
                    App.mcp.stop()
                }
            }
        }
        AppSwitch {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.gapLarge
            Layout.rightMargin: Theme.gapLarge
            text: qsTr("Allow MCP writes")
            enabled: App.mcp.running
            checked: App.mcp.writeEnabled
            onToggled: App.mcp.writeEnabled = checked
        }
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.gapLarge
            Layout.rightMargin: Theme.gapLarge
            text: qsTr("Write access lets the connected client edit, delete, export and save directly.")
            wrapMode: Text.Wrap
            color: Theme.muted
            font.pixelSize: Theme.fontCaption
        }
        Text {
            Layout.fillWidth: true
            Layout.margins: Theme.gapLarge
            visible: App.mcp.error.length > 0
            text: App.mcp.error
            wrapMode: Text.Wrap
            color: Theme.danger
            font.pixelSize: Theme.fontBody
        }
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.gapLarge
            Layout.rightMargin: Theme.gapLarge
            visible: App.mcp.running
            text: App.mcp.endpoint
            wrapMode: Text.WrapAnywhere
            color: Theme.text
            font.pixelSize: Theme.fontBody
        }
        AppButton {
            Layout.leftMargin: Theme.gapLarge
            visible: App.mcp.running
            text: qsTr("Copy endpoint")
            onClicked: App.logs.copyText(App.mcp.endpoint)
        }
        AppButton {
            Layout.leftMargin: Theme.gapLarge
            Layout.bottomMargin: Theme.gapLarge
            visible: App.mcp.running
            text: qsTr("Copy Authorization header")
            onClicked: App.logs.copyText("Authorization: Bearer " + App.mcp.token)
        }
    }
}
