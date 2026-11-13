import BedrockMap 1.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "controls"

Item {
    id: page
    property var controller
    property bool canLocate: true
    signal locateRequested(int dimension, int x, int z)

    function ensureLoaded() {
        if (visible && controller && !controller.loaded && !controller.loading)
            controller.refresh()
    }
    onVisibleChanged: ensureLoaded()
    onControllerChanged: ensureLoaded()
    Component.onCompleted: ensureLoaded()

    Connections {
        target: page.controller
        function onModelAboutToBeReset() { entitiesDialog.close() }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.gapLarge
        spacing: Theme.gapMedium

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gapSmall
            Text {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: qsTr("World entities")
                color: Theme.text
                font.pixelSize: Theme.fontHeading
                elide: Text.ElideRight
            }
            AppButton {
                compact: true
                text: page.controller && page.controller.loading ? qsTr("Cancel") : qsTr("Refresh")
                iconName: page.controller && page.controller.loading ? "close" : "refresh"
                enabled: !!page.controller
                onClicked: {
                    if (page.controller.loading)
                        page.controller.cancel()
                    else
                        page.controller.refresh()
                }
            }
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("Each region is 16 × 16 blocks. Click a region to locate it on the map.")
            color: Theme.muted
            font.pixelSize: Theme.fontBody
            wrapMode: Text.Wrap
        }
        Text {
            Layout.fillWidth: true
            visible: !!page.controller
            text: !page.controller ? "" : page.controller.loading
                  ? page.controller.progressText
                  : qsTr("%1 entities in %2 regions").arg(String(page.controller.entityCount)).arg(String(page.controller.regionCount))
            color: Theme.text
            font.pixelSize: Theme.fontBody
            wrapMode: Text.Wrap
        }
        Text {
            Layout.fillWidth: true
            visible: !!page.controller && page.controller.stale
            text: qsTr("The world or addon resources changed. Refresh to update the list.")
            color: Theme.warning
            font.pixelSize: Theme.fontBody
            wrapMode: Text.Wrap
        }
        Text {
            Layout.fillWidth: true
            visible: !!page.controller && page.controller.skippedRecords > 0
            text: qsTr("Skipped %1 unreadable or missing entity records.").arg(String(page.controller ? page.controller.skippedRecords : 0))
            color: Theme.warning
            font.pixelSize: Theme.fontBody
            wrapMode: Text.Wrap
        }
        Text {
            Layout.fillWidth: true
            visible: !!page.controller && page.controller.error.length > 0
            text: page.controller ? page.controller.error : ""
            color: Theme.danger
            font.pixelSize: Theme.fontBody
            wrapMode: Text.Wrap
        }
        ListView {
            id: regions
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: page.controller
            clip: true
            spacing: Theme.gapSmall
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                id: regionRow
                required property int index
                required property int regionDimension
                required property string dimensionName
                required property int startX
                required property int startZ
                required property int regionEntityCount
                required property string entitySummary
                width: ListView.view.width
                height: regionContent.implicitHeight + Theme.gapLarge * 2
                color: regionMouse.containsMouse ? Theme.surfaceAlt : Theme.surface
                radius: Theme.radiusControl

                MouseArea {
                    id: regionMouse
                    anchors.fill: parent
                    enabled: page.canLocate
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: page.locateRequested(regionRow.regionDimension, regionRow.startX + 8, regionRow.startZ + 8)
                }
                ColumnLayout {
                    id: regionContent
                    x: Theme.gapLarge
                    y: Theme.gapLarge
                    width: parent.width - Theme.gapLarge * 2
                    spacing: Theme.gapSmall
                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            text: regionRow.dimensionName + " · " + qsTr("%1 entities").arg(String(regionRow.regionEntityCount))
                            color: Theme.text
                            font.pixelSize: Theme.fontSubtitle
                            wrapMode: Text.Wrap
                        }
                        AppButton {
                            text: qsTr("Entities")
                            compact: true
                            onClicked: {
                                entitiesDialog.regionDimension = regionRow.regionDimension
                                entitiesDialog.title = regionRow.dimensionName + " · X " + regionRow.startX + " · Z " + regionRow.startZ
                                entitiesDialog.entities = page.controller.entitiesForRegion(regionRow.index)
                                entitiesDialog.open()
                            }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: "X " + regionRow.startX + "…" + (regionRow.startX + 15)
                              + " · Z " + regionRow.startZ + "…" + (regionRow.startZ + 15)
                        color: Theme.accent
                        font.pixelSize: Theme.fontBody
                        wrapMode: Text.Wrap
                    }
                    Text {
                        Layout.fillWidth: true
                        text: regionRow.entitySummary
                        color: Theme.muted
                        font.pixelSize: Theme.fontBody
                        wrapMode: Text.Wrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                    }
                }
            }
            Text {
                anchors.centerIn: parent
                width: Math.max(0, parent.width - Theme.gapLarge * 2)
                visible: !!page.controller && page.controller.loaded && !page.controller.loading && regions.count === 0
                text: qsTr("No entities found.")
                color: Theme.muted
                font.pixelSize: Theme.fontBody
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
            }
        }
    }

    AppDialog {
        id: entitiesDialog
        property int regionDimension: 0
        property var entities: []
        preferredWidth: Theme.ui(560)
        preferredHeight: Theme.ui(560)
        onClosed: entities = []
        footer: AppButton {
            text: qsTr("Close")
            onClicked: entitiesDialog.close()
        }
        ListView {
            anchors.fill: parent
            model: entitiesDialog.entities
            clip: true
            spacing: Theme.gapSmall
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            delegate: Item {
                id: entityRow
                required property var modelData
                width: ListView.view.width
                height: Math.max(Theme.controlHeight, entityText.implicitHeight) + Theme.gapLarge * 2
                MouseArea {
                    anchors.fill: parent
                    enabled: page.canLocate
                    onClicked: {
                        page.locateRequested(entitiesDialog.regionDimension, Math.floor(entityRow.modelData.x), Math.floor(entityRow.modelData.z))
                        entitiesDialog.close()
                    }
                }
                Image {
                    id: entityIcon
                    x: Theme.gapSmall
                    anchors.verticalCenter: parent.verticalCenter
                    width: Theme.ui(32)
                    height: width
                    source: entityRow.modelData.icon
                    fillMode: Image.PreserveAspectFit
                    smooth: false
                }
                AppIcon {
                    anchors.fill: entityIcon
                    visible: entityIcon.status !== Image.Ready
                    name: "villager"
                }
                Column {
                    id: entityText
                    anchors.left: entityIcon.right
                    anchors.leftMargin: Theme.gapMedium
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.gapSmall
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.gapSmall
                    Text {
                        width: parent.width
                        text: entityRow.modelData.label
                        color: Theme.text
                        font.pixelSize: Theme.fontSubtitle
                        wrapMode: Text.WrapAnywhere
                    }
                    Text {
                        width: parent.width
                        text: "X " + Number(entityRow.modelData.x).toFixed(1)
                              + " · Y " + Number(entityRow.modelData.y).toFixed(1)
                              + " · Z " + Number(entityRow.modelData.z).toFixed(1)
                        color: Theme.muted
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
    }
}
