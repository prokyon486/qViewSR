#include "qvgraphicsview.h"
#include "qvapplication.h"
#include "qvinfodialog.h"
#include "qvcocoafunctions.h"
#include "settingsmanager.h"
#include <QWheelEvent>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QSettings>
#include <QMessageBox>
#include <QMovie>
#include <QtMath>
#include <QGestureEvent>
#include <QScrollBar>
#include <QDrag>
#include <QPointer>
#include <QSaveFile>
#include <QImageWriter>
#include <QStandardPaths>
#include <QDateTime>
#include <QToolTip>
#include <QUuid>

QVGraphicsView::QVGraphicsView(QWidget *parent) : QGraphicsView(parent)
{
    // GraphicsView setup
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setFrameShape(QFrame::NoFrame);
    setTransformationAnchor(QGraphicsView::NoAnchor);
    viewport()->setAutoFillBackground(false);

    // part of a pathetic attempt at gesture support
    grabGesture(Qt::PinchGesture);

    // Scene setup
    auto *scene = new QGraphicsScene(-1000000.0, -1000000.0, 2000000.0, 2000000.0, this);
    setScene(scene);

    // Initialize other variables
    currentScale = 1.0;
    scaledSize = QSize();
    isOriginalSize = false;
    lastZoomEventPos = QPoint(-1, -1);
    lastZoomRoundingError = QPointF();
    lastScrollRoundingError = QPointF();
    mousePressButton = Qt::MouseButton::NoButton;
    mousePressModifiers = Qt::KeyboardModifier::NoModifier;
    mousePressPosition = QPoint();

    zoomBasisScaleFactor = 1.0;

    connect(&imageCore, &QVImageCore::animatedFrameChanged, this,
            &QVGraphicsView::animatedFrameChanged);
    connect(&imageCore, &QVImageCore::fileChanged, this, &QVGraphicsView::postLoad);
    connect(&imageCore, &QVImageCore::updateLoadedPixmapItem, this,
            &QVGraphicsView::updateLoadedPixmapItem);
    connect(&imageCore, &QVImageCore::sourceChanging, this, [this] {
        fileDragPending = fileDragGesture = false;
        viewport()->setCursor(Qt::ArrowCursor);
    });

    // Should replace the other timer eventually
    expensiveScaleTimerNew = new QTimer(this);
    expensiveScaleTimerNew->setSingleShot(true);
    expensiveScaleTimerNew->setInterval(50);
    connect(expensiveScaleTimerNew, &QTimer::timeout, this, [this] { scaleExpensively(); });

    loadedPixmapItem = new QGraphicsPixmapItem();
    scene->addItem(loadedPixmapItem);

    // Connect to settings signal
    connect(&qvApp->getSettingsManager(), &SettingsManager::settingsUpdated, this,
            &QVGraphicsView::settingsUpdated);
    settingsUpdated();
}

// Events

void QVGraphicsView::resizeEvent(QResizeEvent *event)
{
    QGraphicsView::resizeEvent(event);
    if (!isOriginalSize)
        resetScale();
    else
        centerOn(loadedPixmapItem);
}

void QVGraphicsView::dropEvent(QDropEvent *event)
{
    if (event->source() == this) { event->ignore(); return; }
    QGraphicsView::dropEvent(event);
    loadMimeData(event->mimeData());
}

void QVGraphicsView::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->source() == this) { event->ignore(); return; }
    QGraphicsView::dragEnterEvent(event);
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void QVGraphicsView::dragMoveEvent(QDragMoveEvent *event)
{
    if (event->source() == this) { event->ignore(); return; }
    QGraphicsView::dragMoveEvent(event);
    event->acceptProposedAction();
}

void QVGraphicsView::dragLeaveEvent(QDragLeaveEvent *event)
{
    QGraphicsView::dragLeaveEvent(event);
    event->accept();
}

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
void QVGraphicsView::enterEvent(QEvent *event)
#else
void QVGraphicsView::enterEvent(QEnterEvent *event)
#endif
{
    QGraphicsView::enterEvent(event);
    viewport()->setCursor(Qt::ArrowCursor);
}

void QVGraphicsView::mousePressEvent(QMouseEvent *event)
{
    fileDragPending = fileDragGesture = false;
    // Begin on the image rectangle (including transparent pixels), not on empty
    // viewport space. Plain dragging continues to pan the image.
    if (event->button() == Qt::LeftButton && event->modifiers() == Qt::ControlModifier
            && getCurrentFileDetails().isPixmapLoaded && !getCurrentFileDetails().isModelDocument
            && loadedPixmapItem->boundingRect().contains(loadedPixmapItem->mapFromScene(mapToScene(event->pos())))) {
        mousePressButton = Qt::NoButton;
        mousePressModifiers = Qt::NoModifier;
        fileDragPending = fileDragGesture = true;
        fileDragStart = event->pos();
        event->accept();
        return;
    }
    const auto startWindowMove = [this, event]() {
#ifdef COCOA_LOADED
        return QVCocoaFunctions::startSystemMove(window());
#else
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        return window()->windowHandle()->startSystemMove();
#else
        Q_UNUSED(event)
        return false;
#endif
#endif
    };

    const auto startFallbackWindowMove = [this, event]() {
        mousePressButton = event->button();
        mousePressModifiers = event->modifiers();
        mousePressPosition = event->pos();
    };

    // Ctrl/Cmd + Shift keeps window movement separate from file dragging.
    if (event->button() == Qt::LeftButton &&
        event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier) &&
        qvApp->getSettingsManager().getBool(SettingsManager::Setting::CtrlDragWindow)) {
        const auto windowState = window()->windowState();
        if (!windowState.testFlag(Qt::WindowFullScreen)
            && !windowState.testFlag(Qt::WindowMaximized)) {
            if (!startWindowMove()) {
                startFallbackWindowMove();
            }
            return;
        }
    }

    // Check for titlebar region drag
    if (event->button() == Qt::LeftButton) {
        const auto windowState = window()->windowState();
        if (!windowState.testFlag(Qt::WindowFullScreen)
            && !windowState.testFlag(Qt::WindowMaximized)) {
#ifdef COCOA_LOADED
            // Check if click is in titlebar region
            int titlebarHeight = QVCocoaFunctions::getTitlebarHeight(window()->windowHandle());
            if (event->pos().y() <= titlebarHeight) {
                if (!startWindowMove()) {
                    startFallbackWindowMove();
                }
                return;
            }
#endif
        }
    }

    QGraphicsView::mousePressEvent(event);
}

void QVGraphicsView::mouseMoveEvent(QMouseEvent *event)
{
    if (fileDragGesture) {
        event->accept();
        if (!fileDragPending) return;
        if (!event->buttons().testFlag(Qt::LeftButton) || event->modifiers() != Qt::ControlModifier) {
            fileDragPending = false;
            return;
        }
        if ((event->pos() - fileDragStart).manhattanLength() < QApplication::startDragDistance()) return;
        fileDragPending = false;
        emit cancelSlideshow();
        QString error;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        auto *mime = getFileDragMimeData(&error);
        QApplication::restoreOverrideCursor();
        if (!mime->hasUrls()) {
            delete mime;
            if (!error.isEmpty()) QToolTip::showText(event->globalPosition().toPoint(), error, viewport());
            return;
        }
        auto *drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->setPixmap(getLoadedPixmap().scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        drag->setHotSpot(QPoint(-12, -12));
        // exec processes events: the user can close this window during a drag.
        const QPointer<QVGraphicsView> guard(this);
        executeFileDrag(drag);
        if (guard) {
            fileDragPending = fileDragGesture = false;
            viewport()->setCursor(Qt::ArrowCursor);
        }
        return;
    }
    if (mousePressButton == Qt::LeftButton) {
        if (mousePressModifiers.testFlag(Qt::ControlModifier)
            && !event->modifiers().testFlag(Qt::ControlModifier)) {
            mousePressButton = Qt::NoButton;
            mousePressModifiers = Qt::NoModifier;
            QGraphicsView::mouseMoveEvent(event);
            return;
        }

        const QPoint delta = event->pos() - mousePressPosition;
        window()->move(window()->pos() + delta);
        return;
    }

    QGraphicsView::mouseMoveEvent(event);
}

void QVGraphicsView::mouseReleaseEvent(QMouseEvent *event)
{
    if (fileDragGesture) {
        fileDragPending = fileDragGesture = false;
        viewport()->setCursor(Qt::ArrowCursor);
        event->accept();
        return;
    }
    mousePressButton = Qt::NoButton;
    mousePressModifiers = Qt::NoModifier;
    QGraphicsView::mouseReleaseEvent(event);
    viewport()->setCursor(Qt::ArrowCursor);
}

bool QVGraphicsView::event(QEvent *event)
{
    // this is for touchpad pinch gestures
    if (event->type() == QEvent::Gesture) {
        auto *gestureEvent = static_cast<QGestureEvent *>(event);
        if (QGesture *pinch = gestureEvent->gesture(Qt::PinchGesture)) {
            auto *pinchGesture = static_cast<QPinchGesture *>(pinch);
            QPinchGesture::ChangeFlags changeFlags = pinchGesture->changeFlags();

            if (changeFlags & QPinchGesture::ScaleFactorChanged) {
                const QPoint hotPoint = mapFromGlobal(pinchGesture->hotSpot().toPoint());
                zoom(pinchGesture->scaleFactor(), hotPoint);
            }

            // Fun rotation stuff maybe later
            //            if (changeFlags & QPinchGesture::RotationAngleChanged) {
            //                qreal rotationDelta = pinchGesture->rotationAngle() -
            //                pinchGesture->lastRotationAngle(); rotate(rotationDelta);
            //                centerOn(loadedPixmapItem);
            //            }
            return true;
        }
    } else if (event->type() == QEvent::NativeGesture) {
        auto *nativeEvent = static_cast<QNativeGestureEvent *>(event);
        if (nativeEvent->gestureType() == Qt::ZoomNativeGesture) {
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
            const QPoint eventPos = nativeEvent->position().toPoint();
#else
            const QPoint eventPos = nativeEvent->pos();
#endif
            zoom(nativeEvent->value() + 1, eventPos);
            return true;
        }
    }
    return QGraphicsView::event(event);
}

void QVGraphicsView::wheelEvent(QWheelEvent *event)
{
#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
    const QPoint eventPos = event->position().toPoint();
#else
    const QPoint eventPos = event->pos();
#endif

    const bool modifierPressed = event->modifiers().testFlag(Qt::ControlModifier);
    bool dontZoom = qvGetSettingInt(ScrollZoom) == 2;
    if (modifierPressed) {
        dontZoom = !dontZoom;
    }

    bool touchDeviceDetected = false;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Auto-detect touchpad
    touchDeviceDetected = event->device()->type() == QInputDevice::DeviceType::TouchPad
            || event->device()->type() == QInputDevice::DeviceType::TouchScreen;
    // Real touchpads are likely to exhibit these characteristics in empirical testing
    touchDeviceDetected = touchDeviceDetected && event->phase() != Qt::NoScrollPhase;
    if (touchDeviceDetected && qvGetSettingInt(ScrollZoom) == 1) {
        // If this is a touch device, override setting
        dontZoom = !modifierPressed;
    }
#endif

    if (dontZoom) {
        const qreal scrollDivisor = 2.0; // To make scrolling less sensitive
        qreal scrollX = event->angleDelta().x() * (isRightToLeft() ? 1 : -1) / scrollDivisor;
        qreal scrollY = event->angleDelta().y() * -1 / scrollDivisor;

        if (event->modifiers() & Qt::ShiftModifier)
            std::swap(scrollX, scrollY);

        QPointF targetScrollDelta = QPointF(scrollX, scrollY) - lastScrollRoundingError;
        QPoint roundedScrollDelta = targetScrollDelta.toPoint();

        horizontalScrollBar()->setValue(horizontalScrollBar()->value() + roundedScrollDelta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() + roundedScrollDelta.y());

        lastScrollRoundingError = roundedScrollDelta - targetScrollDelta;

        return;
    }

    const int yDelta = event->angleDelta().y();
    const qreal yScale = 120.0;

    if (yDelta == 0)
        return;

    const qreal zoomAmountPerWheelClick = qvGetSettingInt(ScaleFactor)/100.0;
    qreal zoomFactor = zoomAmountPerWheelClick;
    if (qvGetSettingBool(FractionalZoom) || touchDeviceDetected) {
        const qreal fractionalWheelClicks = qFabs(yDelta) / yScale;
        zoomFactor *= fractionalWheelClicks;
    }
    zoomFactor += 1.0;

    if (yDelta < 0)
        zoomFactor = qPow(zoomFactor, -1);

    zoom(zoomFactor, eventPos);
}

// Functions

void QVGraphicsView::executeFileDrag(QDrag *drag)
{
    // Never offer a move operation: dropping into a file manager must not remove
    // the original or the PNG that an asynchronous browser upload may still read.
    drag->exec(Qt::CopyAction, Qt::CopyAction);
}

QMimeData *QVGraphicsView::getFileDragMimeData(QString *error)
{
    auto *mime = new QMimeData;
    if (error) error->clear();
    if (!getCurrentFileDetails().isPixmapLoaded || getCurrentFileDetails().isModelDocument) return mime;
    const auto replacement = dragImageProvider ? dragImageProvider() : std::nullopt;
    QString path = getCurrentFileDetails().fileInfo.absoluteFilePath();
    if (replacement) {
        const auto &image = *replacement;
        if (image.isNull()) {
            if (error) *error = QStringLiteral("受け渡し用のSR画像を用意できません。メモリーの空きを確認してください。");
            return mime;
        }
        // Keep immutable exports beyond the drag/window lifetime: browsers read
        // File objects asynchronously after the native drop has completed.
        bool reuse = image.cacheKey() == exportedImageKey && QFileInfo(exportedImagePath).isReadable();
        if (reuse) {
            QFile cached(exportedImagePath);
            reuse = cached.open(QIODevice::ReadWrite) && cached.setFileTime(QDateTime::currentDateTimeUtc(), QFileDevice::FileModificationTime);
        }
        if (!reuse) {
            const auto cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
            const QDir directory(cache + "/drag-exports");
            if (cache.isEmpty() || !QDir().mkpath(directory.absolutePath())) {
                if (error) *error = QStringLiteral("受け渡し用PNGの保存先を作成できません。");
                return mime;
            }
            const auto cutoff = QDateTime::currentDateTimeUtc().addDays(-1);
            for (const auto &file : directory.entryInfoList({"qviewsr-sr-*.png"}, QDir::Files | QDir::NoSymLinks))
                if (file.lastModified() < cutoff) QFile::remove(file.absoluteFilePath());
            const auto target = directory.filePath("qviewsr-sr-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".png");
            QSaveFile file(target);
            QImageWriter writer(&file, "png");
            writer.setCompression(1); // Favor drag startup time; PNG stays lossless.
            if (!file.open(QIODevice::WriteOnly) || !writer.write(image) || !file.commit()) {
                if (error) *error = QStringLiteral("受け渡し用PNGを保存できません: ") + (file.error() != QFileDevice::NoError ? file.errorString() : writer.errorString());
                return mime;
            }
            exportedImagePath = target;
            exportedImageKey = image.cacheKey();
        }
        path = exportedImagePath;
    }
    if (!QFileInfo(path).isFile() || !QFileInfo(path).isReadable()) {
        if (error) *error = QStringLiteral("受け渡す画像ファイルを読み込めません: ") + path;
        return mime;
    }
    // A real local-file URL becomes a browser File, preserving bytes/metadata.
    // Do not add imageData: some browsers would synthesize a second bitmap file.
    mime->setUrls({QUrl::fromLocalFile(path)});
    return mime;
}

QMimeData *QVGraphicsView::getMimeData() const
{
    auto *mimeData = new QMimeData();
    if (!getCurrentFileDetails().isPixmapLoaded)
        return mimeData;

    mimeData->setUrls(
            { QUrl::fromLocalFile(imageCore.getCurrentFileDetails().fileInfo.absoluteFilePath()) });
    mimeData->setImageData(imageCore.getLoadedPixmap().toImage());
    return mimeData;
}

void QVGraphicsView::loadMimeData(const QMimeData *mimeData)
{
    if (mimeData == nullptr)
        return;

    if (!mimeData->hasUrls())
        return;

    const QList<QUrl> urlList = mimeData->urls();

    bool first = true;
    for (const auto &url : urlList) {
        if (first) {
            loadFile(url.toString());
            emit cancelSlideshow();
            first = false;
            continue;
        }
        QVApplication::openFile(url.toString());
    }
}

void QVGraphicsView::loadFile(const QString &fileName)
{
    imageCore.loadFile(fileName);
}

void QVGraphicsView::reloadFile()
{
    if (!getCurrentFileDetails().isPixmapLoaded && !getCurrentFileDetails().isModelDocument)
        return;

    imageCore.loadFile(getCurrentFileDetails().fileInfo.absoluteFilePath(), true);
}

void QVGraphicsView::postLoad()
{
    updateLoadedPixmapItem();
    qvApp->getActionManager().addFileToRecentsList(getCurrentFileDetails().fileInfo);

    emit fileChanged();
}

void QVGraphicsView::zoomIn(const QPoint &pos)
{
    zoom(qvGetSettingInt(ScaleFactor)/100.0 + 1, pos);
}

void QVGraphicsView::zoomOut(const QPoint &pos)
{
    zoom(qPow(qvGetSettingInt(ScaleFactor)/100.0 + 1, -1), pos);
}

void QVGraphicsView::zoom(qreal scaleFactor, const QPoint &pos)
{
    // don't zoom too far out, dude
    currentScale *= scaleFactor;
    if (currentScale >= 500 || currentScale <= 0.01) {
        currentScale *= qPow(scaleFactor, -1);
        return;
    }

    updateFilteringMode();

    if (pos != lastZoomEventPos) {
        lastZoomEventPos = pos;
        lastZoomRoundingError = QPointF();
    }
    const QPointF scenePos = mapToScene(pos) - lastZoomRoundingError;

    zoomBasisScaleFactor *= scaleFactor;
    setTransform(QTransform(zoomBasis).scale(zoomBasisScaleFactor, zoomBasisScaleFactor));
    absoluteTransform.scale(scaleFactor, scaleFactor);

    // If we are zooming in, we have a point to zoom towards, the mouse is on top of the viewport,
    // and cursor zooming is enabled
    if (currentScale > 1.00001 && pos != QPoint(-1, -1) && underMouse()
        && qvGetSettingBool(CursorZoom)) {
        const QPointF p1mouse = mapFromScene(scenePos);
        const QPointF move = p1mouse - pos;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value()
                                        + (move.x() * (isRightToLeft() ? -1 : 1)));
        verticalScrollBar()->setValue(verticalScrollBar()->value() + move.y());
        lastZoomRoundingError = mapToScene(pos) - scenePos;
    } else {
        centerOn(loadedPixmapItem);
    }

    if (qvGetSettingBool(ScalingEnabled) && !isOriginalSize) {
        expensiveScaleTimerNew->start();
    }
}

void QVGraphicsView::scaleExpensively()
{
    // Determine if mirrored or flipped
    bool mirrored = false;
    if (transform().m11() < 0)
        mirrored = true;

    bool flipped = false;
    if (transform().m22() < 0)
        flipped = true;

    // If we are above maximum scaling size
    if ((currentScale >= MAX_EXPENSIVE_SCALING_SIZE)
        || (!qvGetSettingBool(ScalingTwoEnabled) && currentScale > 1.00001)) {
        // Return to original size
        makeUnscaled();
        return;
    }

    // Map size of the original pixmap to the scale acquired in fitting with modification from
    // zooming percentage
    const QRectF mappedRect =
            absoluteTransform.mapRect(QRectF({}, getCurrentFileDetails().loadedPixmapSize));
    const QSizeF mappedPixmapSize = mappedRect.size() * devicePixelRatioF();

    // Undo mirror/flip before new transform
    if (mirrored)
        scale(-1, 1);

    if (flipped)
        scale(1, -1);

    // Set image to scaled version
    loadedPixmapItem->setPixmap(imageCore.scaleExpensively(mappedPixmapSize));

    // Reset transformation
    setTransform(
            QTransform::fromScale(qPow(devicePixelRatioF(), -1), qPow(devicePixelRatioF(), -1)));

    // Redo mirror/flip after new transform
    if (mirrored)
        scale(-1, 1);

    if (flipped)
        scale(1, -1);

    // Set zoombasis
    zoomBasis = transform();
    zoomBasisScaleFactor = 1.0;
}

void QVGraphicsView::makeUnscaled()
{
    // Determine if mirrored or flipped
    bool mirrored = false;
    if (transform().m11() < 0)
        mirrored = true;

    bool flipped = false;
    if (transform().m22() < 0)
        flipped = true;

    // Return to original size
    if (getCurrentFileDetails().isMovieLoaded)
        loadedPixmapItem->setPixmap(getLoadedMovie().currentPixmap());
    else
        loadedPixmapItem->setPixmap(getLoadedPixmap());

    setTransform(absoluteTransform);

    // Redo mirror/flip after new transform
    if (mirrored)
        scale(-1, 1);

    if (flipped)
        scale(1, -1);

    // Reset transformation
    zoomBasis = transform();
    zoomBasisScaleFactor = 1.0;
}

void QVGraphicsView::updateFilteringMode()
{
    const bool exceededSmoothScaleLimit = currentScale >= MAX_FILTERING_SIZE;
    loadedPixmapItem->setTransformationMode(!exceededSmoothScaleLimit
                                                            && qvGetSettingBool(FilteringEnabled)
                                                    ? Qt::SmoothTransformation
                                                    : Qt::FastTransformation);
}

void QVGraphicsView::animatedFrameChanged(QRect rect)
{
    Q_UNUSED(rect)

    if (qvGetSettingBool(ScalingEnabled)) {
        scaleExpensively();
    } else {
        loadedPixmapItem->setPixmap(getLoadedMovie().currentPixmap());
    }
}

void QVGraphicsView::updateLoadedPixmapItem()
{
    // set pixmap and offset
    loadedPixmapItem->setPixmap(getLoadedPixmap());
    scaledSize = loadedPixmapItem->boundingRect().size().toSize();

    resetScale();

    emit updatedLoadedPixmapItem();
}

void QVGraphicsView::setDisplayImagePreservingView(const QImage &image)
{
    if (image.isNull() || getLoadedPixmap().isNull()) return;
    expensiveScaleTimerNew->stop();
    const QPoint scrollPosition(horizontalScrollBar()->value(), verticalScrollBar()->value());
    const QSize oldSize = getLoadedPixmap().size();
    imageCore.setDisplayImage(image);
    const QSize newSize = getLoadedPixmap().size();
    if (newSize == oldSize) {
        // A frame update changes pixels, not the view. Recentring every frame
        // accumulates integer scrollbar rounding (one pixel per update).
        // Keep a resampled display at exactly its existing size as well.
        const auto pixels = loadedPixmapItem->pixmap().size();
        auto pixmap = getLoadedPixmap();
        if (pixels != pixmap.size())
            pixmap = pixmap.scaled(pixels, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        loadedPixmapItem->setPixmap(pixmap);
        viewport()->update();
        return;
    }
    // Resolution changes keep the same displayed width and image origin. Reuse
    // the integer scroll position directly rather than round-tripping through
    // floating-point centreOn(), including for repeated original/SR comparisons.
    makeUnscaled();
    QTransform next = transform();
    loadedPixmapItem->setPixmap(getLoadedPixmap());
    const qreal ratio = qreal(oldSize.width()) / newSize.width();
    next.scale(ratio, ratio);
    absoluteTransform.scale(ratio, ratio);
    setTransform(next);
    zoomBasis = next;
    zoomBasisScaleFactor = 1.0;
    scaledSize = newSize;
    horizontalScrollBar()->setValue(scrollPosition.x());
    verticalScrollBar()->setValue(scrollPosition.y());
    viewport()->update();
}

void QVGraphicsView::resetScale()
{
    if (!getCurrentFileDetails().isPixmapLoaded)
        return;

    fitInViewMarginless(loadedPixmapItem);

    if (qvGetSettingBool(ScalingEnabled))
        expensiveScaleTimerNew->start();
}

void QVGraphicsView::originalSize()
{
    if (isOriginalSize) {
        // If we are at the actual original size
        if (transform() == QTransform()) {
            resetScale(); // back to normal mode
            return;
        }
    }
    makeUnscaled();

    resetTransform();
    centerOn(loadedPixmapItem);

    zoomBasis = transform();
    zoomBasisScaleFactor = 1.0;
    absoluteTransform = transform();

    isOriginalSize = true;
}

void QVGraphicsView::goToFile(const GoToFileMode &mode, int index)
{
    bool shouldRetryFolderInfoUpdate = false;

    // Update folder info only after a little idle time as an optimization for when
    // the user is rapidly navigating through files.
    if (!getCurrentFileDetails().timeSinceLoaded.isValid()
        || getCurrentFileDetails().timeSinceLoaded.hasExpired(3000)) {
        // Make sure the file still exists because if it disappears from the file listing we'll lose
        // track of our index within the folder. Use the static 'exists' method to avoid caching.
        // If we skip updating now, flag it for retry later once we locate a new file.
        if (QFile::exists(getCurrentFileDetails().fileInfo.absoluteFilePath()))
            imageCore.updateFolderInfo();
        else
            shouldRetryFolderInfoUpdate = true;
    }

    const auto &fileList = getCurrentFileDetails().folderFileInfoList;
    if (fileList.isEmpty())
        return;

    int newIndex = getCurrentFileDetails().loadedIndexInFolder;
    int searchDirection = 0;

    switch (mode) {
    case GoToFileMode::constant: {
        newIndex = index;
        break;
    }
    case GoToFileMode::first: {
        newIndex = 0;
        searchDirection = 1;
        break;
    }
    case GoToFileMode::previous: {
        if (newIndex == 0) {
            if (qvGetSettingBool(LoopFoldersEnabled))
                newIndex = fileList.size() - 1;
            else
                emit cancelSlideshow();
        } else
            newIndex--;
        searchDirection = -1;
        break;
    }
    case GoToFileMode::next: {
        if (fileList.size() - 1 == newIndex) {
            if (qvGetSettingBool(LoopFoldersEnabled))
                newIndex = 0;
            else
                emit cancelSlideshow();
        } else
            newIndex++;
        searchDirection = 1;
        break;
    }
    case GoToFileMode::last: {
        newIndex = fileList.size() - 1;
        searchDirection = -1;
        break;
    }
    }

    if (searchDirection != 0) {
        while (searchDirection == 1 && newIndex < fileList.size() - 1
               && !QFile::exists(fileList.value(newIndex).absoluteFilePath))
            newIndex++;
        while (searchDirection == -1 && newIndex > 0
               && !QFile::exists(fileList.value(newIndex).absoluteFilePath))
            newIndex--;
    }

    const QString nextImageFilePath = fileList.value(newIndex).absoluteFilePath;

    if (!QFile::exists(nextImageFilePath)
        || nextImageFilePath == getCurrentFileDetails().fileInfo.absoluteFilePath())
        return;

    if (shouldRetryFolderInfoUpdate) {
        // If the user just deleted a file through qView, closeImage will have been called which
        // empties currentFileDetails.fileInfo. In this case updateFolderInfo can't infer the
        // directory from fileInfo like it normally does, so we'll explicity pass in the folder
        // here.
        imageCore.updateFolderInfo(QFileInfo(nextImageFilePath).path());
    }

    loadFile(nextImageFilePath);
}

void QVGraphicsView::fitInViewMarginless(const QRectF &rect)
{
#ifdef COCOA_LOADED
    int obscuredHeight = QVCocoaFunctions::getObscuredHeight(window()->windowHandle());
#else
    int obscuredHeight = 0;
#endif

    // Set adjusted image size / bounding rect based on
    QSize adjustedImageSize = getCurrentFileDetails().loadedPixmapSize;
    QRectF adjustedBoundingRect = rect;

    switch (qvGetSettingInt(CropMode)) { // should be enum tbh
    case 1: // only take into account height
    {
        adjustedImageSize.setWidth(1);
        adjustedBoundingRect.setWidth(1);
        break;
    }
    case 2: // only take into account width
    {
        adjustedImageSize.setHeight(1);
        adjustedBoundingRect.setHeight(1);
        break;
    }
    }
    adjustedBoundingRect.moveCenter(rect.center());

    if (!scene() || adjustedBoundingRect.isNull())
        return;

    // Reset the view scale to 1:1.
    QRectF unity = transform().mapRect(QRectF(0, 0, 1, 1));
    if (unity.isEmpty())
        return;
    scale(1 / unity.width(), 1 / unity.height());

    // Determine what we are resizing to
    const int adjWidth = width() - MARGIN;
    const int adjHeight = height() - MARGIN - obscuredHeight;

    QRectF viewRect;
    // Resize to window size unless you are meant to stop at the actual size, basically
    if (qvGetSettingBool(PastActualSizeEnabled)
        || (adjustedImageSize.width() >= adjWidth || adjustedImageSize.height() >= adjHeight)) {
        viewRect = viewport()->rect().adjusted(MARGIN, MARGIN, -MARGIN, -MARGIN);
        viewRect.setHeight(viewRect.height() - obscuredHeight);
    } else {
        // stop at actual size
        viewRect = QRect(QPoint(), getCurrentFileDetails().loadedPixmapSize);
        QPoint center = this->rect().center();
        center.setY(center.y() - obscuredHeight);
        viewRect.moveCenter(center);
    }

    if (viewRect.isEmpty())
        return;

    // Find the ideal x / y scaling ratio to fit \a rect in the view.
    QRectF sceneRect = transform().mapRect(adjustedBoundingRect);
    if (sceneRect.isEmpty())
        return;

    qreal xratio = viewRect.width() / sceneRect.width();
    qreal yratio = viewRect.height() / sceneRect.height();

    xratio = yratio = qMin(xratio, yratio);

    // Find and set the transform required to fit the original image
    // Compact version of above code
    QRectF sceneRect2 = transform().mapRect(QRectF({}, adjustedImageSize));
    qreal absoluteRatio =
            qMin(viewRect.width() / sceneRect2.width(), viewRect.height() / sceneRect2.height());

    absoluteTransform = QTransform::fromScale(absoluteRatio, absoluteRatio);

    // Scale and center on the center of \a rect.
    scale(xratio, yratio);
    centerOn(adjustedBoundingRect.center());

    // variables
    zoomBasis = transform();

    isOriginalSize = false;
    currentScale = 1.0;
    updateFilteringMode();
    zoomBasisScaleFactor = 1.0;
}

void QVGraphicsView::fitInViewMarginless(const QGraphicsItem *item)
{
    return fitInViewMarginless(item->sceneBoundingRect());
}

void QVGraphicsView::centerOn(const QPointF &pos)
{
#ifdef COCOA_LOADED
    int obscuredHeight = QVCocoaFunctions::getObscuredHeight(window()->windowHandle());
#else
    int obscuredHeight = 0;
#endif

    qreal width = viewport()->width();
    qreal height = viewport()->height() - obscuredHeight;
    QPointF viewPoint = transform().map(pos);

    if (isRightToLeft()) {
        qint64 horizontal = 0;
        horizontal += horizontalScrollBar()->minimum();
        horizontal += horizontalScrollBar()->maximum();
        horizontal -= int(viewPoint.x() - width / 2.0);
        horizontalScrollBar()->setValue(horizontal);
    } else {
        horizontalScrollBar()->setValue(int(viewPoint.x() - width / 2.0));
    }

    verticalScrollBar()->setValue(int(viewPoint.y() - obscuredHeight - (height / 2.0)));
}

void QVGraphicsView::centerOn(qreal x, qreal y)
{
    centerOn(QPointF(x, y));
}

void QVGraphicsView::centerOn(const QGraphicsItem *item)
{
    centerOn(item->sceneBoundingRect().center());
}

void QVGraphicsView::settingsUpdated()
{
    if (getCurrentFileDetails().isPixmapLoaded)
        resetScale();
}

void QVGraphicsView::closeImage()
{
    imageCore.closeImage();
}

void QVGraphicsView::jumpToNextFrame()
{
    imageCore.jumpToNextFrame();
}

void QVGraphicsView::setPaused(const bool &desiredState)
{
    imageCore.setPaused(desiredState);
}

void QVGraphicsView::setSpeed(const int &desiredSpeed)
{
    imageCore.setSpeed(desiredSpeed);
}

void QVGraphicsView::rotateImage(int rotation)
{
    imageCore.rotateImage(rotation);
}
