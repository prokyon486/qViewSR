// SPDX-License-Identifier: GPL-3.0-or-later
#include "model_view.h"
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickImageProvider>
#include <QQuickWindow>
#include <QQuickItem>
#include <QtQuick3D/qquick3dobject.h>
#include <QSet>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QImageWriter>
#include <QSaveFile>
#include <QFileInfo>
#include <QColorSpace>
#include <QTimer>
#include <QtMath>
#include <cmath>

namespace Model3D {
namespace {
// An original, procedural studio environment. No downloaded textures or extra assets.
class StudioProvider final : public QQuickImageProvider {
public:
    StudioProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &, QSize *size, const QSize &) override {
        QImage image(512, 256, QImage::Format_RGBA8888);
        for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
            const double sky = qMax(0.0, std::cos(y * M_PI / image.height()));
            const bool panel = y > 35 && y < 105 && ((x > 50 && x < 115) || (x > 305 && x < 345));
            const int value = panel ? 245 : qRound(70 + 90 * sky);
            image.setPixelColor(x, y, QColor(value, value, value));
        }
        if (size) *size = image.size();
        return image;
    }
};
bool finite(const QVector3D &value) {
    return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z());
}
}

View::View(QWidget *parent) : QQuickWidget(parent)
{
    setObjectName("modelView");
    setResizeMode(QQuickWidget::SizeRootObjectToView);
    setFocusPolicy(Qt::StrongFocus);
    setAcceptDrops(true);
    setClearColor(QColor("#292c32"));
    engine()->addImageProvider("qviewsr-studio", new StudioProvider);
    rootContext()->setContextProperty("modelView", this);
    connect(this, &QQuickWidget::sceneGraphError, this, [this](QQuickWindow::SceneGraphError, const QString &error) {
        rendererError_ = QStringLiteral("3D描画を開始できません: ") + error;
        fail(rendererError_);
    });
    setSource(QUrl("qrc:/model3d/Scene.qml"));
    if (status() == QQuickWidget::Error) {
        QStringList errors;
        for (const auto &error : this->errors()) errors << error.toString();
        rendererError_ = QStringLiteral("3D表示ライブラリを読み込めません: ") + errors.join('\n');
        fail(rendererError_);
    }
}
View::~View()
{
    // Tear down bindings while their context object still has its derived type.
    setSource(QUrl());
}
void View::clear()
{
    ++generation_;
    dragging_ = false; unsetCursor();
    ready_ = false; fitted_ = false; source_ = QUrl(); document_ = {};
    message_.clear();
    emit sourceChanged();
    emit stateChanged();
}
void View::fail(const QString &error)
{
    ready_ = false; message_ = error; unsetCursor(); emit stateChanged();
}
void View::loadModel(const QString &path)
{
    clear();
    if (!rendererError_.isEmpty()) { fail(rendererError_); return; }
    document_ = inspectGlb(path);
    if (!document_.error.isEmpty()) { fail(document_.error); return; }
    resetView();
    message_ = QStringLiteral("GLBを読み込み中…"); setCursor(Qt::WaitCursor); emit stateChanged();
    const auto generation = generation_;
    // Let the view switch and loading message paint before the synchronous Qt importer runs.
    QTimer::singleShot(30, this, [this, path, generation] {
        if (generation != generation_) return;
        source_ = QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath());
        emit sourceChanged();
        // Qt 6.4 emits Success before it builds the scene. Stop the timelines
        // after the source binding/import has returned, before the next frame.
        freezeAnimations();
        QTimer::singleShot(10000, this, [this, generation] {
            if (generation == generation_ && !fitted_ && message_ == QStringLiteral("GLBを読み込み中…"))
                fail(QStringLiteral("モデルの表示範囲を取得できませんでした。メッシュまたは描画環境を確認してください。"));
        });
    });
}
void View::rendererStatus(int status, const QString &error, QObject *asset)
{
    asset_ = asset;
    if (status == 2) fail(QStringLiteral("GLBの読み込みに失敗しました: ") + error);
}
void View::freezeAnimations()
{
    if (asset_ && document_.hasAnimations) {
        // Qt 6.4's runtime importer can start its first timeline automatically.
        // Use the exposed properties, without depending on private timeline headers.
        // Qt Quick 3D's item parents can differ from QObject ownership parents.
        QSet<QObject *> objects;
        QList<QObject *> pending{asset_};
        while (!pending.isEmpty()) {
            auto *object = pending.takeLast();
            if (objects.contains(object)) continue;
            objects.insert(object);
            pending.append(object->children());
            if (auto *node = qobject_cast<QQuick3DObject *>(object))
                for (auto *child : node->childItems()) pending.append(child);
        }
        for (auto *object : objects) {
            if (object->inherits("QQuickTimelineAnimation")) object->setProperty("running", false);
        }
        for (auto *object : objects) {
            if (object->inherits("QQuickTimeline")) object->setProperty("enabled", false);
        }
    }
}
void View::acceptBounds(const QVector3D &minimum, const QVector3D &maximum)
{
    if (fitted_ || source_.isEmpty()) return;
    freezeAnimations();
    const auto extent = maximum - minimum;
    const float radius = extent.length() / 2.0f;
    if (!finite(minimum) || !finite(maximum) || !std::isfinite(radius) || radius <= 0
            || extent.x() < 0 || extent.y() < 0 || extent.z() < 0) return;
    center_ = minimum + extent / 2.0f;
    radius_ = qMax(radius, 1e-6f);
    fitted_ = true; ready_ = true;
    resetView();
    message_ = QStringLiteral("GLB · %1メッシュ · %2°").arg(document_.meshes).arg(fov_, 0, 'f', 0);
    if (!document_.warnings.isEmpty()) message_ += QStringLiteral(" · 注意事項あり（マウスを重ねて確認）");
    unsetCursor(); emit stateChanged();
}
void View::fitDistance()
{
    const double vertical = qDegreesToRadians(double(fov_)) / 2;
    const double aspect = double(qMax(1, width())) / qMax(1, height());
    const double angle = qMin(vertical, std::atan(std::tan(vertical) * aspect));
    distance_ = float(radius_ * 1.12 / std::sin(angle));
}
void View::resetView()
{
    rotation_ = {}; pan_ = {}; fov_ = 45; automaticFit_ = true; fitDistance();
    emit cameraChanged(); emit stateChanged();
}
float View::clipNear() const { return qMax(radius_ * 0.0001f, distance_ - radius_ * 1.8f); }
float View::clipFar() const { return qMax(distance_ + radius_ * 4, clipNear() + radius_ * 8); }
void View::dolly(double steps)
{
    if (!ready_ || !std::isfinite(steps)) return;
    automaticFit_ = false;
    distance_ = float(qBound(double(radius_) * 0.02, distance_ * std::exp(-qBound(-100.0, steps, 100.0) * 0.12), double(radius_) * 10000));
    emit cameraChanged(); emit stateChanged();
}
void View::changeFieldOfView(double steps)
{
    if (!ready_ || !std::isfinite(steps)) return;
    automaticFit_ = false; fov_ = float(qBound(5.0, fov_ - steps * 2.0, 120.0));
    emit cameraChanged(); emit stateChanged();
}
void View::rotateModel(const QPointF &delta)
{
    if (!ready_) return;
    rotation_ = (QQuaternion::fromAxisAndAngle(0, 1, 0, delta.x() * 0.4)
                 * QQuaternion::fromAxisAndAngle(1, 0, 0, delta.y() * 0.4) * rotation_).normalized();
    emit cameraChanged();
}
void View::panCamera(const QPointF &delta)
{
    if (!ready_) return;
    automaticFit_ = false;
    const double perPixel = 2 * distance_ * std::tan(qDegreesToRadians(double(fov_)) / 2) / qMax(1, height());
    pan_ += QPointF(-delta.x(), delta.y()) * perPixel;
    emit cameraChanged();
}
QSize View::exportSize() const { return size() * devicePixelRatioF(); }
QString View::detailText() const
{
    return message_ + QStringLiteral("\nCtrl＋ドラッグ: 回転 / ドラッグ: 平行移動\nホイール: 距離 / Shift＋ホイール: 画角\n中央クリック: 全体表示 / ダブルクリック: 全画面\nPNG: 表示領域の実ピクセル数、sRGB（3D表示は画面ICC変換なし）")
            + (document_.warnings.isEmpty() ? QString() : "\n" + document_.warnings.join('\n'));
}
QImage View::capture()
{
    if (!ready_ || !isVisible()) return {};
    // Renders outstanding camera/resize changes too; never includes the Qt Widgets toolbar.
    auto image = grabFramebuffer();
    image.setDevicePixelRatio(1.0);
    image.setColorSpace(QColorSpace::SRgb);
    return image;
}
bool View::savePng(const QString &path, QString *error)
{
    const auto failed = [&](const QString &text) { if (error) *error = text; return false; };
    if (QFileInfo(path).suffix().compare("png", Qt::CaseInsensitive) != 0
            || QFileInfo(path).canonicalFilePath() == QFileInfo(source_.toLocalFile()).canonicalFilePath())
        return failed(QStringLiteral("GLBは変更できません。別の名前のPNGファイルを指定してください。"));
    const auto image = capture();
    if (image.isNull()) return failed(QStringLiteral("保存できる3D表示がありません。"));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return failed(file.errorString());
    QImageWriter writer(&file, "png");
    if (!writer.write(image)) return failed(writer.errorString());
    if (!file.commit()) return failed(file.errorString());
    return true;
}
void View::mousePressEvent(QMouseEvent *event)
{
    setFocus();
    if (event->button() == Qt::LeftButton) {
        dragging_ = true; lastPointer_ = event->position(); setCursor(Qt::ClosedHandCursor);
    } else if (event->button() == Qt::MiddleButton) resetView();
    else if (event->button() == Qt::BackButton) emit previousRequested();
    else if (event->button() == Qt::ForwardButton) emit nextRequested();
    else { event->ignore(); return; }
    event->accept();
}
void View::mouseMoveEvent(QMouseEvent *event)
{
    if (dragging_ && event->buttons().testFlag(Qt::LeftButton)) {
        const auto delta = event->position() - lastPointer_; lastPointer_ = event->position();
        if (event->modifiers().testFlag(Qt::ControlModifier)) rotateModel(delta);
        else panCamera(delta);
        event->accept();
    } else event->ignore();
}
void View::mouseReleaseEvent(QMouseEvent *event)
{
    dragging_ = false; unsetCursor(); event->accept();
}
void View::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) { dragging_ = false; unsetCursor(); emit fullscreenRequested(); event->accept(); }
    else event->ignore();
}
void View::wheelEvent(QWheelEvent *event)
{
    const double steps = event->pixelDelta().isNull() ? event->angleDelta().y() / 120.0 : event->pixelDelta().y() / 40.0;
    if (event->modifiers().testFlag(Qt::ShiftModifier)) changeFieldOfView(steps);
    else dolly(steps);
    event->accept();
}
void View::resizeEvent(QResizeEvent *event)
{
    QQuickWidget::resizeEvent(event);
    if (automaticFit_) { fitDistance(); emit cameraChanged(); }
    emit stateChanged();
}
void View::dragEnterEvent(QDragEnterEvent *event) { if (event->mimeData()->hasUrls()) event->acceptProposedAction(); }
void View::dragMoveEvent(QDragMoveEvent *event) { if (event->mimeData()->hasUrls()) event->acceptProposedAction(); }
void View::dropEvent(QDropEvent *event)
{
    if (event->mimeData()->hasUrls()) { emit filesDropped(event->mimeData()->urls()); event->acceptProposedAction(); }
}
}
