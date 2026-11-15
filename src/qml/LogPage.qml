import BedrockMap 1.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import "controls"

Control {
    id: page
    font.pixelSize: Theme.fontBody
    palette.window: Theme.surface
    palette.base: Theme.surface
    palette.text: Theme.text
    palette.windowText: Theme.text
    palette.button: Theme.surfaceAlt
    palette.buttonText: Theme.text
    palette.highlight: Theme.accent
    palette.highlightedText: Theme.onAccent
    palette.placeholderText: Theme.muted

    function openLogExport(historyName) {
        exportDialog.historyName = historyName || ""
        const logPath = exportDialog.historyName.length > 0 ? exportDialog.historyName : App.logs.logFile
        const separator = Math.max(logPath.lastIndexOf("/"), logPath.lastIndexOf("\\"))
        const existingName = logPath.length > 0 ? logPath.substring(separator + 1) : ""
        const timestamp = Qt.formatDateTime(new Date(), "yyyyMMdd-HHmmss")
        const fileName = existingName.length > 0 ? existingName : "BedrockMap-logs-" + timestamp + ".log"
        exportDialog.selectedFile = exportDialog.currentFolder.toString().length > 0
                ? exportDialog.currentFolder + "/" + encodeURIComponent(fileName) : fileName
        exportDialog.open()
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.background
    }
    FileUrlDialog {
        id: exportDialog
        property string historyName: ""
        title: qsTr("Export logs")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "log"
        nameFilters: [qsTr("Log files (*.log *.txt)")]
        onAccepted: {
            const saved = historyName.length > 0 ? App.logs.saveHistorical(historyName, selectedUrl) : App.logs.save(selectedUrl)
            if (!saved)
                App.notifications.error(qsTr("Export failed"), "", App.logs.error)
            else
                App.notifications.info(qsTr("Export successful"))
        }
    }
    AppDialog {
        id: historyDialog
        property var files: []
        title: qsTr("Historical logs")
        preferredWidth: Theme.ui(560)
        preferredHeight: Theme.ui(440)
        onClosed: files = []
        footer: AppButton {
            text: qsTr("Close")
            onClicked: historyDialog.close()
        }
        contentItem: Item {
            ListView {
                id: historyList
                anchors.fill: parent
                model: historyDialog.files
                clip: true
                spacing: Theme.gapSmall
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                delegate: AppListRow {
                    required property var modelData
                    width: historyList.width
                    iconName: "log"
                    label: modelData.name
                    detail: modelData.modified
                    onClicked: {
                        const name = modelData.name
                        historyDialog.close()
                        page.openLogExport(name)
                    }
                }
            }
            Text {
                anchors.centerIn: parent
                width: parent.width
                visible: historyDialog.files.length === 0
                text: qsTr("No historical logs")
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                color: Theme.muted
                font.pixelSize: Theme.fontBody
            }
        }
    }
    AppMenu {
        id: copyMenu
        property string selection: ""
        AppMenuItem {
            text: qsTr("Copy")
            enabled: copyMenu.selection.length > 0
            onTriggered: App.logs.copyText(copyMenu.selection)
        }
        AppMenuItem {
            text: qsTr("Copy displayed logs")
            onTriggered: App.logs.copyAll()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.gapLarge
            spacing: Theme.gapMedium
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gapMedium
                AppTextField {
                    id: search
                    Layout.fillWidth: true
                    Layout.minimumWidth: 60
                    placeholderText: qsTr("Search logs")
                    text: App.logs.filter
                    onTextEdited: searchDelay.restart()
                    Timer {
                        id: searchDelay
                        interval: 200
                        onTriggered: App.logs.filter = search.text
                    }
                }
                AppComboBox {
                    Layout.preferredWidth: 130
                    model: [qsTr("All levels"), qsTr("Info and above"), qsTr("Warnings and above"), qsTr("Errors")]
                    currentIndex: App.logs.minimumLevel
                    onActivated: App.logs.minimumLevel = currentIndex
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gapMedium
                AppCheckBox {
                    text: qsTr("Show prefix")
                    checked: App.logs.showPrefix
                    onToggled: App.logs.showPrefix = checked
                }
                Item {
                    Layout.fillWidth: true
                }
                AppButton {
                    text: qsTr("Copy")
                    enabled: App.logs.count > 0
                    onClicked: App.logs.copyAll()
                    tooltip: qsTr("Copy displayed logs")
                }
                AppButton {
                    text: qsTr("Export")
                    enabled: App.logs.logFile.length > 0 || App.logs.storedCount > 0
                    onClicked: page.openLogExport()
                    tooltip: qsTr("Export the complete log file")
                }
                AppButton {
                    text: qsTr("Clear")
                    enabled: App.logs.storedCount > 0
                    onClicked: App.logs.clear()
                    tooltip: qsTr("Clear the view; keep the log file")
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.border
        }
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Flickable {
                id: logView
                property bool followTail: true
                anchors.fill: parent
                clip: true
                contentWidth: width
                contentHeight: message.height + Theme.gapMedium * 2
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                onMovementStarted: followTail = false
                Component.onCompleted: scrollToEnd()

                function scrollToEnd() {
                    Qt.callLater(function () {
                        contentY = Math.max(0, contentHeight - height)
                    })
                }

                TextEdit {
                    id: message
                    x: Theme.gapLarge
                    y: Theme.gapMedium
                    width: logView.width - Theme.gapLarge * 2
                    height: implicitHeight
                    text: App.logs.displayHtml
                    textFormat: TextEdit.RichText
                    readOnly: true
                    selectByMouse: !Theme.touch
                    wrapMode: TextEdit.WrapAnywhere
                    font.pixelSize: 11
                    color: Theme.text
                    selectedTextColor: Theme.onAccent
                    selectionColor: Theme.accent
                    TapHandler {
                        enabled: !Theme.touch
                        acceptedButtons: Qt.RightButton
                        onTapped: {
                            copyMenu.selection = message.selectedText
                            copyMenu.popup()
                        }
                    }
                }
            }
            Text {
                anchors.centerIn: parent
                visible: App.logs.count === 0
                text: App.logs.storedCount > 0 ? qsTr("No matching logs") : qsTr("No logs")
                color: Theme.muted
                font.pixelSize: Theme.fontBody
            }
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.border
        }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.gapMedium
            spacing: Theme.gapSmall
            Label {
                Layout.fillWidth: true
                text: qsTr("%1 displayed / %2 retained | %3 older entries omitted").arg(App.logs.count).arg(
                          App.logs.storedCount).arg(App.logs.omittedCount)
                color: Theme.muted
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gapSmall
                Label {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    text: App.logs.logFile
                    color: Theme.muted
                    font.pixelSize: Theme.fontCaption
                    elide: Text.ElideMiddle
                    AppToolTip {
                        text: App.logs.logFile
                        shown: pathHover.hovered
                    }
                    HoverHandler {
                        id: pathHover
                    }
                }
                AppButton {
                    text: qsTr("History")
                    iconName: "log"
                    onClicked: {
                        historyDialog.files = App.logs.historicalFiles()
                        historyDialog.open()
                    }
                }
            }
        }
    }
    Connections {
        target: App.logs
        function onCountChanged() {
            if (logView.followTail)
                logView.scrollToEnd()
        }
        function onShowPrefixChanged() {
            if (logView.followTail)
                logView.scrollToEnd()
        }
    }
}
