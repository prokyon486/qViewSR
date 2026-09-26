// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick3D
import QtQuick3D.AssetUtils

Item {
    View3D {
        anchors.fill: parent
        environment: SceneEnvironment {
            backgroundMode: SceneEnvironment.Color
            clearColor: "#292c32"
            antialiasingMode: SceneEnvironment.MSAA
            antialiasingQuality: SceneEnvironment.High
            tonemapMode: SceneEnvironment.TonemapModeLinear
            lightProbe: Texture { source: "image://qviewsr-studio/environment" }
        }
        camera: camera
        PerspectiveCamera {
            id: camera
            position: modelView.cameraPosition
            fieldOfView: modelView.fieldOfView
            clipNear: modelView.clipNear
            clipFar: modelView.clipFar
        }
        DirectionalLight { eulerRotation: Qt.vector3d(-35, -35, 0); brightness: 0.7 }
        DirectionalLight { eulerRotation: Qt.vector3d(25, 145, 0); brightness: 0.3 }
        Node {
            position: modelView.modelCenter
            pivot: modelView.modelCenter
            rotation: modelView.modelRotation
            RuntimeLoader {
                id: asset
                source: modelView.modelSource
                onStatusChanged: modelView.rendererStatus(status, errorString, asset)
                onBoundsChanged: {
                    if (status === RuntimeLoader.Success)
                        modelView.acceptBounds(bounds.minimum, bounds.maximum)
                }
            }
        }
    }
    Text {
        anchors.centerIn: parent
        width: Math.max(1, parent.width - 48)
        visible: !modelView.ready
        text: modelView.message
        color: "#eeeeee"
        font.pixelSize: 16
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
    }
}
