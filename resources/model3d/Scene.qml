// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick3D
import QtQuick3D.AssetUtils

Item {
    View3D {
        anchors.fill: parent
        environment: SceneEnvironment {
            // The hosting widget supplies the on-screen background. Keeping
            // this texture transparent also preserves coverage when exporting.
            backgroundMode: SceneEnvironment.Transparent
            antialiasingMode: SceneEnvironment.MSAA
            antialiasingQuality: SceneEnvironment.High
            tonemapMode: SceneEnvironment.TonemapModeLinear
            lightProbe: Texture { source: "image://qviewsr-studio/environment" }
        }
        camera: camera
        PerspectiveCamera {
            id: camera
            objectName: "modelCamera"
            position: modelView.cameraPosition
            rotation: modelView.cameraRotation
            fieldOfView: modelView.fieldOfView
            clipNear: modelView.clipNear
            clipFar: modelView.clipFar
        }
        // These lights and the environment remain in world space, outside the
        // model's transform and the camera's roll.
        DirectionalLight { objectName: "worldKeyLight"; eulerRotation: Qt.vector3d(-35, -35, 0); brightness: 0.7 }
        DirectionalLight { objectName: "worldFillLight"; eulerRotation: Qt.vector3d(25, 145, 0); brightness: 0.3 }
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
