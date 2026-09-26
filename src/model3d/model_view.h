// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QQuickWidget>
#include <QQuaternion>
#include <QPointer>
#include <QVector3D>
#include "glb_document.h"

namespace Model3D {
class View : public QQuickWidget
{
    Q_OBJECT
    Q_PROPERTY(QUrl modelSource READ modelSource NOTIFY sourceChanged)
    Q_PROPERTY(QVector3D modelCenter READ modelCenter NOTIFY cameraChanged)
    Q_PROPERTY(QQuaternion modelRotation READ modelRotation NOTIFY cameraChanged)
    Q_PROPERTY(QVector3D cameraPosition READ cameraPosition NOTIFY cameraChanged)
    Q_PROPERTY(QQuaternion cameraRotation READ cameraRotation NOTIFY cameraChanged)
    Q_PROPERTY(float fieldOfView READ fieldOfView NOTIFY cameraChanged)
    Q_PROPERTY(float clipNear READ clipNear NOTIFY cameraChanged)
    Q_PROPERTY(float clipFar READ clipFar NOTIFY cameraChanged)
    Q_PROPERTY(QString message READ message NOTIFY stateChanged)
    Q_PROPERTY(bool ready READ isReady NOTIFY stateChanged)
public:
    explicit View(QWidget *parent = nullptr);
    ~View() override;
    void loadModel(const QString &path);
    void clear();
    void resetView();
    void dolly(double steps);
    void changeFieldOfView(double steps);
    void rollCamera(int degrees);
    void rotateModel(const QPointF &delta);
    void panCamera(const QPointF &delta);
    QImage capture(bool transparentBackground = false);
    bool savePng(const QString &path, QString *error = nullptr);
    bool isReady() const { return ready_; }
    QString message() const { return message_; }
    QString detailText() const;
    QUrl modelSource() const { return source_; }
    QVector3D modelCenter() const { return center_; }
    QQuaternion modelRotation() const { return rotation_; }
    QVector3D cameraPosition() const { return center_ + QVector3D(pan_.x(), pan_.y(), distance_); }
    QQuaternion cameraRotation() const { return QQuaternion::fromAxisAndAngle(0, 0, 1, roll_); }
    int cameraRoll() const { return roll_; }
    float fieldOfView() const { return fov_; }
    float distance() const { return distance_; }
    float radius() const { return radius_; }
    float clipNear() const;
    float clipFar() const;
    QSize exportSize() const;
    Q_INVOKABLE void acceptBounds(const QVector3D &minimum, const QVector3D &maximum);
    Q_INVOKABLE void rendererStatus(int status, const QString &error, QObject *asset);
signals:
    void sourceChanged();
    void cameraChanged();
    void stateChanged();
    void previousRequested();
    void nextRequested();
    void fullscreenRequested();
    void filesDropped(const QList<QUrl> &urls);
protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
private:
    void fail(const QString &error);
    void freezeAnimations();
    void fitDistance();
    QUrl source_;
    QPointer<QObject> asset_;
    Document document_;
    QVector3D center_;
    QQuaternion rotation_;
    QPointF pan_, lastPointer_;
    float radius_ = 1, distance_ = 3, fov_ = 45;
    int roll_ = 0;
    bool ready_ = false, fitted_ = false, automaticFit_ = true, dragging_ = false;
    quint64 generation_ = 0;
    QString message_, rendererError_;
};
}
