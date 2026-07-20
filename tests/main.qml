/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.pipewire as PipeWire

ApplicationWindow {
    id: root
    title: "PipeWire Portal Preview"
    width: 800
    height: 600
    visible: true
    property QtObject app

    function addStream(nodeid, displayText, fd, allowDmaBuf) {
        streams.append({nodeId: nodeid, display: displayText, fd: fd, allowDmaBuf: allowDmaBuf})
    }

    function removeStream(nodeid) {
        for (var i = 0; i < streams.count; ++i) {
            if (streams.get(i).nodeId === nodeid) {
                streams.remove(i)
                break;
            }
        }
    }

    ListModel {
        id: streams
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 12

        Label {
            Layout.fillWidth: true
            text: "Portal Preview"
            font.pixelSize: 20
        }

        ListView {
            id: rep
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 12
            model: streams

            delegate: Frame {
                id: streamDelegate
                required property var nodeId
                required property string display
                required property var fd
                required property bool allowDmaBuf
                width: ListView.view.width

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true

                        Label {
                            Layout.fillWidth: true
                            text: streamDelegate.display
                            elide: Text.ElideRight
                        }

                        Button {
                            text: sourceItem.visible ? "Pause Preview" : "Resume Preview"
                            onClicked: sourceItem.visible = !sourceItem.visible
                        }
                    }

                    PipeWire.PipeWireSourceItem {
                        id: sourceItem
                        Layout.fillWidth: true
                        Layout.preferredHeight: 320
                        nodeId: streamDelegate.nodeId
                        fd: streamDelegate.fd
                        allowDmaBuf: streamDelegate.allowDmaBuf
                    }
                }
            }
        }
    }
}
