// SPDX-License-Identifier: GPL-3.0-or-later
#include "qvapplication.h"
#include "mainwindow.h"
#include "sr/sr_controller.h"
#include "sr/color_pipeline.h"
#include "vector/vector_controller.h"
#include <QtTest>
#include <QCheckBox>
#include <QColorSpace>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDomDocument>
#include <QDoubleSpinBox>
#include <QFile>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QSvgRenderer>
#include <QTemporaryDir>
#include <QSettings>
#include <QScrollBar>
#include <QThreadPool>

class VectorTests : public QObject {
    Q_OBJECT
    QTemporaryDir files;
    QPointer<MainWindow> window;
    QVGraphicsView* view = nullptr;
    Vector::Controller* controller = nullptr;
    QDialog* panel = nullptr;
    QString sourcePath;
    QByteArray sourceDigest;
    QString failure;

    static QByteArray digest(const QString& path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(&file);
        return hash.result();
    }

    static QImage fixture() {
        QImage image(64, 48, QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        QPainter painter(&image); // Deliberately binary alpha, including the border.
        painter.fillRect(QRect(5, 5, 54, 38), QColor(242, 212, 173));
        painter.fillRect(QRect(10, 9, 21, 25), QColor(192, 89, 75));
        painter.fillRect(QRect(33, 13, 20, 25), QColor(73, 133, 201));
        painter.setPen(QPen(QColor(72, 17, 29), 2));
        painter.drawLine(13, 12, 26, 29);
        painter.drawEllipse(QRect(34, 18, 14, 13));
        painter.setPen(QPen(QColor(17, 67, 31), 2));
        painter.drawLine(9, 38, 50, 39);
        painter.end();
        image.setColorSpace(QColorSpace::AdobeRgb);
        return image;
    }

    bool waitIdle(int timeout = 15000) {
        QElapsedTimer elapsed; elapsed.start();
        do {
            QCoreApplication::processEvents();
            if (!controller->isBusy()) return true;
            QTest::qWait(20);
        } while (elapsed.elapsed() < timeout);
        return false;
    }

    bool generate(int timeout = 20000) {
        failure.clear();
        QSignalSpy ready(controller, &Vector::Controller::resultReady);
        controller->generate();
        QElapsedTimer elapsed; elapsed.start();
        while (elapsed.elapsed() < timeout) {
            if (!failure.isEmpty()) return false;
            if (ready.count() && !controller->isBusy()) return controller->hasResult();
            QTest::qWait(20);
        }
        failure = panel->findChild<QLabel*>("vectorStatus")->text();
        return false;
    }

    bool select(const char* name, const QString& value) {
        auto* combo = panel->findChild<QComboBox*>(name);
        const int index = combo ? combo->findData(value) : -1;
        if (index < 0) return false;
        combo->setCurrentIndex(index);
        return waitIdle();
    }

    QDomDocument savedSvg(const QString& name = "result.svg") {
        QDomDocument document;
        QString error;
        const auto path = files.filePath(name);
        if (!controller->saveSvg(path, &error)) { failure = error; return document; }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || !document.setContent(file.readAll()))
            failure = QStringLiteral("Cannot parse saved SVG");
        return document;
    }

    static QDomElement group(const QDomDocument& document, const QString& id) {
        const auto elements = document.elementsByTagName("g");
        for (int i = 0; i < elements.count(); ++i)
            if (elements.at(i).toElement().attribute("id") == id) return elements.at(i).toElement();
        return {};
    }

    static double imageError(const QImage& actual, const QImage& reference) {
        if (actual.size() != reference.size()) return 255;
        double error = 0;
        for (int y = 0; y < actual.height(); ++y) for (int x = 0; x < actual.width(); ++x) {
            const auto a = actual.pixelColor(x, y), b = reference.pixelColor(x, y);
            error += qAbs(a.red()-b.red()) + qAbs(a.green()-b.green())
                   + qAbs(a.blue()-b.blue()) + qAbs(a.alpha()-b.alpha());
        }
        return error / (actual.width() * actual.height() * 4.0);
    }

    static QByteArray curvedSvg() {
        return QByteArrayLiteral(
            "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='48' viewBox='0 0 64 48'>"
            "<path d='M 4 36 C 17 2 40 47 60 9' fill='none' stroke='#dd3927' stroke-width='1.15' stroke-linecap='round'/>"
            "<ellipse cx='32.3' cy='24.2' rx='9.1' ry='6.7' fill='#257dc9' fill-opacity='.65' stroke='#112c17' stroke-width='.8'/>"
            "</svg>");
    }

    static QImage rasterizedSvg(const QByteArray& svg, QSize size) {
        QImage image(size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        image.setColorSpace(QColorSpace::SRgb);
        QSvgRenderer renderer(svg);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        renderer.render(&painter, QRectF(QPointF(), QSizeF(size)));
        return image;
    }

    QImage renderedView(qreal dpr = 1) const {
        const auto size = view->viewport()->size();
        QImage image(QSize(qRound(size.width() * dpr), qRound(size.height() * dpr)), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent); image.setDevicePixelRatio(dpr);
        QPainter painter(&image);
        painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
        view->render(&painter, QRectF(QPointF(), QSizeF(size)), view->viewport()->rect());
        return image;
    }

    QImage renderedReference(const QByteArray& svg, const QTransform& imageTransform = {},
                             qreal dpr = 1, const QImage& raster = {}) const {
        const auto size = view->viewport()->size();
        QImage image(QSize(qRound(size.width() * dpr), qRound(size.height() * dpr)), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent); image.setDevicePixelRatio(dpr);
        QPainter painter(&image);
        painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
        painter.setWorldTransform(imageTransform * view->viewportTransform());
        if (raster.isNull()) {
            QSvgRenderer renderer(svg);
            renderer.render(&painter, QRectF(0, 0, 64, 48));
        } else painter.drawImage(QPointF(), raster);
        return image;
    }

private slots:
    void initTestCase() {
        QVERIFY(files.isValid());
        const QString root = QStringLiteral(SR_SOURCE_DIR);
        qputenv("QVIEWSR_VECTOR_ROOT", root.toUtf8());
        const auto python = qEnvironmentVariable("QVIEWSR_VECTOR_PYTHON", root+"/.local/vector-probe-venv/bin/python");
        const auto autotrace = qEnvironmentVariable("QVIEWSR_AUTOTRACE", root+"/.local/vector-probe-autotrace/bin/autotrace");
        if (!QFileInfo(python).isExecutable() || !QFileInfo(autotrace).isExecutable())
            QSKIP("Vector dependencies missing; run python3 tools/vector/setup_vector.py locally.");
        QProcess check;
        auto environment = QProcessEnvironment::systemEnvironment(); environment.remove("LD_LIBRARY_PATH");
        check.setProcessEnvironment(environment);
        check.start(python, {"-c", "import numpy, PIL, vtracer; assert hasattr(vtracer, 'convert_image_to_svg_py')"});
        if (!check.waitForFinished(10000) || check.exitStatus() != QProcess::NormalExit || check.exitCode() != 0)
            QSKIP("Python vector dependencies are unavailable or incompatible.");

        // These tests must never initialize physical NCS devices.
        auto sr = Sr::Configuration::load();
        sr.workerPath = files.filePath("missing-sr-worker");
        sr.runtimeRoot = files.filePath("missing-sr-runtime");
        sr.modelPath = files.filePath("missing-sr-model.xml");
        sr.displayProfile = "sRGB"; sr.save();
        QSettings settings;
        settings.setValue("options/preloadingmode", 0);
        settings.setValue("options/windowresizemode", 0);
        qvApp->getSettingsManager().loadSettings();
    }

    void init() {
        QSettings().remove("vector");
        sourcePath = files.filePath("source.png");
        QVERIFY(fixture().save(sourcePath));
        sourceDigest = digest(sourcePath); QVERIFY(!sourceDigest.isEmpty());
        window = new MainWindow;
        window->resize(1000, 700); window->show();
        view = window->findChild<QVGraphicsView*>(); QVERIFY(view);
        controller = window->findChild<Vector::Controller*>(); QVERIFY(controller);
        panel = window->findChild<QDialog*>("vectorPanel"); QVERIFY(panel);
        failure.clear();
        connect(controller, &Vector::Controller::failed, this, [this](const QString& error) { failure = error; });
        window->openFile(sourcePath);
        QTRY_COMPARE_WITH_TIMEOUT(view->getImageCore().getSourceImage().size(), QSize(64, 48), 5000);
        for (const char* name : {"vectorColor", "vectorBackground", "vectorStrength", "vectorDetail"})
            QVERIFY(panel->findChild<QComboBox*>(name));
        for (const char* name : {"vectorShowFill", "vectorGrayFill", "vectorShowResult"})
            QVERIFY(panel->findChild<QCheckBox*>(name));
    }

    void cleanup() {
        if (window) {
            controller->cancel();
            QThreadPool::globalInstance()->waitForDone();
            window->close();
            QTRY_VERIFY_WITH_TIMEOUT(window.isNull(), 5000);
        }
        view = nullptr; controller = nullptr; panel = nullptr;
    }

    void floatingPanelAndPersistence() {
        controller->showPanel();
        QTRY_VERIFY(panel->isVisible());
        QCOMPARE(panel->windowType(), Qt::Tool);
        QVERIFY(!panel->isModal());
        QCOMPARE(panel->windowModality(), Qt::NonModal);
        panel->move(45, 35); panel->resize(430, panel->height()+20);
        QVERIFY2(generate(), qPrintable(failure));
        QVERIFY(controller->showingResult());
        QVERIFY(view->hasVectorPreview());
        const auto before = controller->resultImage();
        const auto geometry = panel->geometry();
        panel->close();
        QVERIFY(!panel->isVisible());
        QVERIFY(controller->hasResult()); QVERIFY(controller->showingResult());
        controller->showPanel(); QTRY_VERIFY(panel->isVisible());
        QCOMPARE(panel->geometry(), geometry);
        QCOMPARE(controller->resultImage(), before);
        QVERIFY(!QSettings().value("vector/panelGeometry").toByteArray().isEmpty());
        controller->setFullscreen(true); QVERIFY(!panel->isVisible());
        controller->setFullscreen(false); QTRY_VERIFY(panel->isVisible());
        controller->setShowingResult(false); QVERIFY(!controller->showingResult());
        QVERIFY(!view->hasVectorPreview());
        QCOMPARE(view->getLoadedPixmap().size(), QSize(64, 48));
        controller->setShowingResult(true); QVERIFY(controller->showingResult());
        QVERIFY(view->hasVectorPreview());
        QCOMPARE(digest(sourcePath), sourceDigest);
    }

    void zoomRendersCurvesAtScreenResolution() {
        auto* sr = window->findChild<Sr::Controller*>(); QVERIFY(sr);
        const auto svg = curvedSvg();
        const auto lowResolution = rasterizedSvg(svg, QSize(64, 48));
        sr->setExternalVectorPreview(lowResolution, svg);
        QVERIFY(view->hasVectorPreview());
        view->originalSize();
        view->zoom(24);
        view->horizontalScrollBar()->setValue(view->horizontalScrollBar()->value() + 37);
        view->verticalScrollBar()->setValue(view->verticalScrollBar()->value() - 21);
        const auto transform = view->transform();
        const QPoint scroll(view->horizontalScrollBar()->value(), view->verticalScrollBar()->value());

        for (const qreal dpr : {1.0, 2.0}) {
            const auto actual = renderedView(dpr);
            const auto fresh = renderedReference(svg, {}, dpr);
            const auto enlargedBitmap = renderedReference(svg, {}, dpr, lowResolution);
            const auto vectorError = imageError(actual, fresh);
            const auto bitmapError = imageError(actual, enlargedBitmap);
            QVERIFY2(vectorError < .5, qPrintable(QString("Fresh SVG error at DPR %1: %2").arg(dpr).arg(vectorError)));
            QVERIFY2(bitmapError > vectorError + 1.0, qPrintable(QString("Vector %1, bitmap %2").arg(vectorError).arg(bitmapError)));
        }
        // Painting a new viewport must never rewrite the logical image or the
        // scene position, including after the old expensive-scaling timer fires.
        QTest::qWait(80);
        QCOMPARE(view->transform(), transform);
        QCOMPARE(QPoint(view->horizontalScrollBar()->value(), view->verticalScrollBar()->value()), scroll);
        QCOMPARE(view->getLoadedPixmap().size(), QSize(64, 48));
        QVERIFY(view->hasVectorPreview());
        sr->clearExternalPreview();
        QVERIFY(!view->hasVectorPreview());
    }

    void vectorViewportHonorsRotationMirrorsAndDisplayProfile() {
        auto* sr = window->findChild<Sr::Controller*>(); QVERIFY(sr);
        const auto previous = sr->configuration();
        auto config = previous;
        config.displayProfile = files.filePath("display-adobe-rgb.icc");
        const auto destination = QColorSpace(QColorSpace::AdobeRgb).iccProfile();
        { QFile file(config.displayProfile); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(destination), destination.size()); }
        sr->setConfiguration(config);
        const auto svg = curvedSvg();
        sr->setExternalVectorPreview(rasterizedSvg(svg, QSize(64, 48)), svg);
        QVERIFY(view->hasVectorPreview());
        window->rotateRight(); window->mirror(); window->flip();
        view->zoom(16);
        QVERIFY(view->transform().m11() < 0); QVERIFY(view->transform().m22() < 0);
        const auto rotation = QImage::trueMatrix(QTransform().rotate(90), 64, 48);
        const auto fresh = renderedReference(svg, rotation);
        QString error;
        const auto reference = Sr::convert(fresh, {Sr::srgbProfile(), "sRGB", {}, false}, destination, &error);
        QVERIFY2(!reference.isNull(), qPrintable(error));
        const auto actual = renderedView();
        QVERIFY2(imageError(actual, reference) < .5, qPrintable(QString("Rotated ICC SVG error: %1").arg(imageError(actual, reference))));
        QVERIFY(imageError(actual, fresh) > .5);
        QVERIFY(view->hasVectorPreview());
        sr->setConfiguration(previous);
        QVERIFY(view->hasVectorPreview());
        sr->setExternalPreview(rasterizedSvg(svg, QSize(64, 48)));
        QVERIFY(!view->hasVectorPreview());
    }

    void layerColorsAndTransparentCombinations() {
        QVERIFY2(generate(), qPrintable(failure));
        panel->findChild<QSlider*>("vectorOpacity")->setValue(100);
        QVERIFY(waitIdle());
        for (const auto& line : {QString("transparent"), QString("white"), QString("black")}) {
            QVERIFY(select("vectorColor", line));
            for (const auto& background : {QString("transparent"), QString("white"), QString("black")}) {
                QVERIFY(select("vectorBackground", background));
                const auto document = savedSvg();
                QVERIFY2(!document.isNull(), qPrintable(failure));
                QVERIFY(group(document, "fill").isNull());
                const auto lines = group(document, "lines");
                QCOMPARE(lines.isNull(), line == "transparent");
                if (!lines.isNull()) {
                    const auto paths = lines.elementsByTagName("path");
                    QVERIFY(paths.count() > 0);
                    for (int i = 0; i < paths.count(); ++i)
                        QCOMPARE(QColor(paths.at(i).toElement().attribute("stroke")), line == "white" ? QColor(Qt::white) : QColor(Qt::black));
                }
                const auto image = controller->resultImage(); QVERIFY(!image.isNull());
                const auto corner = image.pixelColor(0, 0);
                if (background == "transparent") QCOMPARE(corner.alpha(), 0);
                else QCOMPARE(corner, background == "white" ? QColor(Qt::white) : QColor(Qt::black));
                if (line == "transparent" && background == "transparent") {
                    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x)
                        QCOMPARE(image.pixelColor(x, y).alpha(), 0);
                }
            }
        }
        QCOMPARE(digest(sourcePath), sourceDigest);
    }

    void grayscaleOnlyAffectsFill() {
        QVERIFY2(generate(), qPrintable(failure));
        QVERIFY(select("vectorColor", "source"));
        QVERIFY(select("vectorBackground", "transparent"));
        panel->findChild<QCheckBox*>("vectorShowFill")->setChecked(true);
        QVERIFY(waitIdle());
        const auto colored = savedSvg();
        QVERIFY(!group(colored, "fill").isNull());
        const auto sourceLines = group(colored, "lines").elementsByTagName("path");
        QStringList lineColors;
        for (int i = 0; i < sourceLines.count(); ++i) lineColors << sourceLines.at(i).toElement().attribute("stroke");
        QVERIFY(!lineColors.isEmpty());
        panel->findChild<QCheckBox*>("vectorGrayFill")->setChecked(true);
        QVERIFY(waitIdle());
        const auto gray = savedSvg();
        const auto fills = group(gray, "fill").elementsByTagName("path");
        QVERIFY(fills.count() > 0);
        for (int i = 0; i < fills.count(); ++i) {
            const QColor color(fills.at(i).toElement().attribute("fill"));
            if (!color.isValid()) continue;
            QCOMPARE(color.red(), color.green()); QCOMPARE(color.green(), color.blue());
        }
        const auto lines = group(gray, "lines").elementsByTagName("path");
        QCOMPARE(lines.count(), lineColors.size());
        bool coloredLine = false;
        for (int i = 0; i < lines.count(); ++i) {
            const auto color = lines.at(i).toElement().attribute("stroke");
            QCOMPARE(color, lineColors[i]);
            const QColor rgb(color); coloredLine |= rgb.red() != rgb.green() || rgb.green() != rgb.blue();
        }
        QVERIFY(coloredLine);
    }

    void vectorPngExportAndViewTransforms() {
        QVERIFY(select("vectorBackground", "transparent"));
        panel->findChild<QCheckBox*>("vectorShowFill")->setChecked(true);
        QVERIFY2(generate(), qPrintable(failure));
        const auto svg = savedSvg();
        QVERIFY2(!svg.isNull(), qPrintable(failure));
        QCOMPARE(svg.elementsByTagName("image").count(), 0);
        QVERIFY(svg.elementsByTagName("path").count() > 0);
        QVERIFY(!svg.toByteArray().contains("data:image"));
        QString error;
        const auto normal = files.filePath("normal.png");
        QVERIFY2(controller->savePng(normal, 2, &error), qPrintable(error));
        const QImage before(normal);
        QCOMPARE(before.size(), QSize(128, 96));
        QCOMPARE(before.colorSpace(), QColorSpace(QColorSpace::SRgb));
        QCOMPARE(before.pixelColor(0, 0).alpha(), 0);
        QVERIFY(before.pixelColor(40, 40).alpha() > 0);
        QVERIFY(!controller->savePng(files.filePath("invalid.png"), 0, &error));
        QVERIFY(!controller->savePng(sourcePath, 2, &error));
        QVERIFY(!controller->saveSvg(sourcePath, &error));
        QCOMPARE(digest(sourcePath), sourceDigest);

        window->rotateRight(); window->mirror(); window->flip();
        const auto expected = before.transformed(QTransform().rotate(90)).mirrored(true, true);
        const auto transformed = files.filePath("transformed.png");
        QVERIFY2(controller->savePng(transformed, 2, &error), qPrintable(error));
        const QImage after(transformed);
        QCOMPARE(after.size(), QSize(96, 128));
        QVERIFY(imageError(after, expected) < 2.0);
        const auto oriented = savedSvg("transformed.svg");
        QVERIFY(!oriented.isNull());
        QSvgRenderer renderer(oriented.toByteArray()); QVERIFY(renderer.isValid());
        QCOMPARE(renderer.defaultSize(), QSize(48, 64));
        QImage fromSvg(after.size(), QImage::Format_ARGB32_Premultiplied); fromSvg.fill(Qt::transparent);
        { QPainter painter(&fromSvg); renderer.render(&painter, QRectF(QPointF(), QSizeF(after.size()))); }
        QVERIFY(imageError(after, fromSvg) < .1);
        std::unique_ptr<QMimeData> drag(view->getFileDragMimeData(&error));
        QVERIFY2(error.isEmpty(), qPrintable(error)); QCOMPARE(drag->urls().size(), 1);
        const QImage dragged(drag->urls().first().toLocalFile());
        QCOMPARE(dragged.width() * 4, dragged.height() * 3); // Rotated 48:64 aspect ratio.
        QCOMPARE(dragged.colorSpace(), QColorSpace(QColorSpace::SRgb));
        QCOMPARE(digest(sourcePath), sourceDigest);
    }

    void fileChangesAndExternalBusyCancelPendingResults() {
        QVERIFY2(generate(), qPrintable(failure));
        const auto retained = controller->resultImage();
        controller->generate(); QVERIFY(controller->isBusy());
        {
            // Simulate the external controller's busy interval without its real
            // idle state notification replacing this deliberately injected state.
            QSignalBlocker block(window->findChild<Sr::Controller*>());
            controller->setExternalBusy(true);
            QTRY_VERIFY_WITH_TIMEOUT(!controller->isBusy(), 5000);
            QVERIFY(!controller->showingResult());
            QCOMPARE(controller->resultImage(), retained);
            QVERIFY(!panel->findChild<QPushButton*>("vectorGenerate")->isEnabled());
            controller->setShowingResult(true); QVERIFY(!controller->showingResult());
            controller->setExternalBusy(false);
        }
        controller->setShowingResult(true); QVERIFY(controller->showingResult());

        const auto other = files.filePath("other.png");
        QImage different(31, 25, QImage::Format_RGB32); different.fill(Qt::green); QVERIFY(different.save(other));
        QSignalSpy ready(controller, &Vector::Controller::resultReady);
        controller->generate(); QVERIFY(controller->isBusy());
        window->openFile(other);
        QTRY_COMPARE_WITH_TIMEOUT(view->getImageCore().getSourceImage().size(), QSize(31, 25), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller->isBusy(), 5000);
        QVERIFY(!controller->hasResult()); QVERIFY(!controller->showingResult());
        QVERIFY(!view->hasVectorPreview());
        QCOMPARE(ready.count(), 0);
        QCOMPARE(view->getLoadedPixmap().size(), QSize(31, 25));
        QCOMPARE(digest(sourcePath), sourceDigest);
    }

    void semitransparentInputReportsError() {
        const auto path = files.filePath("semitransparent.png");
        QImage image(32, 24, QImage::Format_ARGB32); image.fill(QColor(100, 50, 10, 128));
        QVERIFY(image.save(path));
        window->openFile(path);
        QTRY_COMPARE(view->getImageCore().getSourceImage().size(), QSize(32, 24));
        QSignalSpy failed(controller, &Vector::Controller::failed);
        controller->generate();
        QTRY_VERIFY_WITH_TIMEOUT(failed.count() > 0, 15000);
        QVERIFY(failed.last().first().toString().contains(QStringLiteral("半透明")));
        QVERIFY(!controller->hasResult());
        QCOMPARE(view->getImageCore().getSourceImage().pixelColor(3, 3).alpha(), 128);
    }

    void supersamplingCancelsPendingVectorWithoutLosingCachedResult() {
        auto* sr = window->findChild<Sr::Controller*>(); QVERIFY(sr);
        const auto previous = sr->configuration();
        auto fake = previous;
        fake.workerPath = QStringLiteral(SR_SOURCE_DIR)+"/tests/fake_worker.py";
        fake.runtimeRoot = files.filePath("runtime"); fake.modelPath = files.filePath("model.xml");
        fake.devices = "normal"; fake.scale = 2;
        QVERIFY(QDir().mkpath(fake.runtimeRoot+"/deployment_tools/inference_engine/lib/intel64"));
        for (const auto& path : {fake.modelPath, fake.runtimeRoot+"/deployment_tools/inference_engine/lib/intel64/libmyriadPlugin.so"}) {
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("test");
        }
        sr->setConfiguration(fake);
        QTRY_VERIFY_WITH_TIMEOUT(sr->backendReady(), 5000);
        QVERIFY2(generate(), qPrintable(failure));
        const auto cached = controller->resultImage();
        QVERIFY(controller->showingResult());
        QVERIFY(view->hasVectorPreview());
        QSignalSpy vectorReady(controller, &Vector::Controller::resultReady);
        QSignalSpy srReady(sr, &Sr::Controller::resultReady);
        controller->generate(); QVERIFY(controller->isBusy());
        sr->start();
        QTRY_COMPARE_WITH_TIMEOUT(srReady.count(), 1, 8000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller->isBusy(), 5000);
        QCOMPARE(vectorReady.count(), 0);
        QVERIFY(sr->showingSr()); QVERIFY(!controller->showingResult());
        QVERIFY(!view->hasVectorPreview());
        QCOMPARE(controller->resultImage(), cached);
        controller->setShowingResult(true);
        QVERIFY(controller->showingResult()); QVERIFY(!sr->showingSr());
        QVERIFY(view->hasVectorPreview());
        QString error;
        QVERIFY2(controller->savePng(files.filePath("after-sr.png"), 2, &error), qPrintable(error));
        QCOMPARE(QImage(files.filePath("after-sr.png")).size(), QSize(128, 96));
        controller->setShowingResult(false);
        QVERIFY(sr->hasResult());
        sr->toggle(); QVERIFY(sr->showingSr());
        QVERIFY(!view->hasVectorPreview());
        QCOMPARE(sr->resultImage().size(), QSize(128, 96));
        QCOMPARE(digest(sourcePath), sourceDigest);
        sr->setConfiguration(previous);
    }

    void realUserImage() {
        const auto source = qEnvironmentVariable("QVIEWSR_TEST_VECTOR_IMAGE");
        if (source.isEmpty()) QSKIP("Set QVIEWSR_TEST_VECTOR_IMAGE to enable the opt-in real image test.");
        QVERIFY(QFileInfo(source).isReadable());
        const auto hash = digest(source);
        window->openFile(source);
        QTRY_COMPARE_WITH_TIMEOUT(view->getCurrentFileDetails().fileInfo.absoluteFilePath(), QFileInfo(source).absoluteFilePath(), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(view->getCurrentFileDetails().isPixmapLoaded, 15000);
        controller->showPanel(); QTRY_VERIFY(panel->isVisible());
        QSignalSpy ready(controller, &Vector::Controller::resultReady);
        auto* button = panel->findChild<QPushButton*>("vectorGenerate"); QVERIFY(button->isEnabled());
        QTest::mouseClick(button, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(ready.count() > 0 || !failure.isEmpty(), 180000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure)); QVERIFY(waitIdle(30000));
        QVERIFY(controller->showingResult());
        const auto artifacts = qEnvironmentVariable("ARTIFACTS");
        if (!artifacts.isEmpty()) {
            QVERIFY(QDir().mkpath(artifacts));
            QString error;
            QVERIFY2(controller->saveSvg(artifacts+"/vector.svg", &error), qPrintable(error));
            QVERIFY2(controller->savePng(artifacts+"/vector-4x.png", 4, &error), qPrintable(error));
            QVERIFY(window->grab().save(artifacts+"/vector-window.png"));
            QVERIFY(panel->grab().save(artifacts+"/vector-panel.png"));
        }
        QCOMPARE(digest(source), hash);
    }
};

int main(int argc, char** argv) {
    QTemporaryDir settings;
    qputenv("XDG_CONFIG_HOME", settings.path().toUtf8());
    qputenv("XDG_CACHE_HOME", settings.filePath("cache").toUtf8());
    QCoreApplication::setOrganizationName("qViewSR-vector-tests");
    QCoreApplication::setApplicationName("qViewSR-vector-tests");
    QSettings s; s.setValue("firstlaunch", true); s.setValue("updatenotifications", false); s.sync();
    QVApplication app(argc, argv);
    VectorTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "tst_vectortests.moc"
