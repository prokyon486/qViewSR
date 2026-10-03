// SPDX-License-Identifier: GPL-3.0-or-later
#include "qvapplication.h"
#include "mainwindow.h"
#include "sr/sr_controller.h"
#include "sr/color_pipeline.h"
#include "vector/vector_controller.h"
#include "vector/cancellable_save_file.h"
#include "vector/memory_budget.h"
#include "vector/generated_svg_renderer.h"
#include "vector/vector_editor.h"
#include <QtTest>
#include <QCheckBox>
#include <QColorSpace>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDomDocument>
#include <QDoubleSpinBox>
#include <QFile>
#include <QImageWriter>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QScrollArea>
#include <QSvgRenderer>
#include <QTemporaryDir>
#include <QSettings>
#include <QScrollBar>
#include <QScopeGuard>
#include <QThreadPool>
#include <limits>

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

    void dragAt(QPoint from, QPoint to) {
        auto* viewport = view->viewport();
        QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, from);
        QMouseEvent move(QEvent::MouseMove, to, viewport->mapToGlobal(to), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &move);
        QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, to);
    }

    QPoint editorPoint(QPointF point) const {
        return view->vectorToViewportTransform(QRectF(0, 0, 64, 48)).map(point).toPoint();
    }

    bool configureFakeSr(Sr::Controller* sr, const QString& mode, double scale = 2) {
        auto configuration = sr->configuration();
        configuration.workerPath = QStringLiteral(SR_SOURCE_DIR)+"/tests/fake_worker.py";
        configuration.runtimeRoot = files.filePath("vector-sr-runtime");
        configuration.modelPath = files.filePath("vector-sr-model.xml");
        configuration.devices = mode; configuration.scale = scale; configuration.denoise = 5;
        if (!QDir().mkpath(configuration.runtimeRoot+"/deployment_tools/inference_engine/lib/intel64")) return false;
        for (const auto& path : {configuration.modelPath,
                 configuration.runtimeRoot+"/deployment_tools/inference_engine/lib/intel64/libmyriadPlugin.so"}) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write("test") != 4) return false;
        }
        sr->setConfiguration(configuration);
        return true;
    }

    static QImage opaqueFixture() {
        // Preserve a non-sRGB source so accidentally applying its profile to
        // already-sRGB SR pixels produces a detectable color difference.
        QImage image(64, 48, QImage::Format_RGB32); image.fill(Qt::white);
        { QPainter painter(&image); painter.drawImage(0, 0, fixture()); }
        image.setColorSpace(QColorSpace::AdobeRgb);
        return image;
    }

    static QString preparedInput(const QTemporaryDir& temporary) {
        const auto folders = QDir(temporary.path()).entryList({"qviewsr-vector-*"}, QDir::Dirs | QDir::NoDotAndDotDot);
        if (folders.size() != 1) return {};
        return QDir(temporary.path()).filePath(folders.first()+"/input.png");
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

    static QByteArray longCurveSvg() {
        QByteArray path("M10 30");
        const QPointF corners[] = {{10,30}, {90,30}, {90,90}, {10,90}, {10,30}};
        for (int side=0; side<4; ++side) for (int i=0; i<4000; ++i) {
            const auto start = corners[side], delta = corners[side+1]-start;
            path += 'C';
            for (int control=1; control<=3; ++control) {
                const auto point = start + delta*((i+control/3.)/4000.);
                path += QByteArray::number(point.x(), 'f', 8)+' '+QByteArray::number(point.y(), 'f', 8)+' ';
            }
        }
        // Opposite winding keeps this inner contour transparent. A final
        // disconnected subpath must survive after all 48000 cubic elements.
        path += "ZM30 45L30 75L70 75L70 45ZM92 32L98 32L98 38L92 38Z";
        return "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='80' viewBox='10 20 100 80'>"
            "<g transform='translate(2 1)'><path fill='#804020' fill-opacity='.5' fill-rule='nonzero' d='"+path+"'/></g></svg>";
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
    void generatedSvgSmallKeepsQtRendering() {
        const auto svg = curvedSvg();
        Vector::GeneratedSvgRenderer renderer(svg);
        QVERIFY2(renderer.isValid(), qPrintable(renderer.errorString()));
        QVERIFY(!renderer.usesExtendedRenderer());
        QImage image(256, 192, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
        image.setColorSpace(QColorSpace::SRgb);
        QString error;
        QVERIFY2(renderer.render(image, QTransform::fromScale(4, 4), &error), qPrintable(error));
        QCOMPARE(image, rasterizedSvg(svg, image.size()));
    }

    void generatedSvgLongCurvesKeepHolesAndTransforms() {
        QTest::failOnWarning(QRegularExpression(QStringLiteral(".*path truncated.*")));
        const auto svg = longCurveSvg();
        Vector::GeneratedSvgRenderer renderer(svg);
        if (!renderer.isValid() && renderer.errorString().contains("librsvg"))
            QSKIP("Optional librsvg/Cairo runtime unavailable; explicit rejection is tested separately.");
        QVERIFY2(renderer.isValid(), qPrintable(renderer.errorString()));
        QVERIFY(renderer.usesExtendedRenderer());
        QCOMPARE(renderer.viewBoxF(), QRectF(10, 20, 100, 80));
        QPainterPath shape;
        shape.setFillRule(Qt::WindingFill);
        shape.addRect(QRectF(10, 30, 80, 60));
        shape.moveTo(30, 45); shape.lineTo(30, 75); shape.lineTo(70, 75); shape.lineTo(70, 45); shape.closeSubpath();
        shape.addRect(QRectF(92, 32, 6, 6));
        const QTransform transforms[] = {
            QTransform::fromTranslate(-10, -20)*QTransform::fromScale(2, 2),
            QTransform::fromTranslate(-10, -20)*QTransform().rotate(90)*QTransform::fromTranslate(80, 0)
                *QTransform::fromScale(-2, 2)*QTransform::fromTranslate(160, 0),
            QTransform::fromTranslate(-40, -30)*QTransform::fromScale(3, 3)
        };
        for (const auto& transform : transforms) {
            QImage image(200, 200, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
            QImage expected(image.size(), image.format()); expected.fill(Qt::transparent);
            { QPainter painter(&expected); painter.setRenderHint(QPainter::Antialiasing);
              painter.setWorldTransform(QTransform::fromTranslate(2, 1)*transform);
              painter.fillPath(shape, QColor(128, 64, 32, 128)); }
            QString error;
            QVERIFY2(renderer.render(image, transform, &error), qPrintable(error));
            QVERIFY2(imageError(image, expected) < .2, qPrintable(QString::number(imageError(image, expected))));
        }
        QString error;
        const auto image = Vector::rasterizeSvg(svg, QSize(200, 160), &error);
        QVERIFY2(!image.isNull(), qPrintable(error));
        QCOMPARE(image.pixelColor(84, 84).alpha(), 0); // Hole remains transparent.
        QCOMPARE(image.pixelColor(174, 34).alpha(), 128); // Last subpath was not truncated.
        QCOMPARE(image.colorSpace(), QColorSpace(QColorSpace::SRgb));
        QImage tooWide(32768, 1, QImage::Format_ARGB32_Premultiplied);
        QVERIFY(!renderer.render(tooWide, QTransform(), &error));
        QVERIFY(error.contains("32767"));
    }

    void generatedSvgLongPathsRequireBackendAndRestrictedInput() {
        const auto svg = longCurveSvg();
        Vector::GeneratedSvgRenderer unavailable(svg, false);
        QVERIFY(!unavailable.isValid());
        QVERIFY(unavailable.errorString().contains("librsvg"));
        QImage image(10, 10, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
        QString error;
        QVERIFY(!unavailable.render(image, QTransform(), &error));
        QVERIFY(error.contains("librsvg"));
        for (const auto& addition : {QByteArray("<image href='file:///tmp/unexpected.png'/>"),
                                     QByteArray("<path style='fill:red' d='M0 0L1 1'/>")}) {
            auto invalid = svg; invalid.replace("</svg>", addition+"</svg>");
            Vector::GeneratedSvgRenderer renderer(invalid);
            QVERIFY(!renderer.isValid()); QVERIFY(!renderer.errorString().isEmpty());
        }
        for (const auto& element : {QByteArray("<svg "), QByteArray("<g ")}) {
            auto opacityGroup = svg; opacityGroup.replace(element, element+"opacity='.5' ");
            Vector::GeneratedSvgRenderer rejected(opacityGroup);
            QVERIFY(!rejected.isValid());
            QVERIFY(rejected.errorString().contains(QStringLiteral("グループの不透明度"))
                    || rejected.errorString().contains(QStringLiteral("librsvg")));
        }
        Vector::GeneratedSvgRenderer dtd("<!DOCTYPE svg [<!ENTITY x 'value'>]>"+svg);
        QVERIFY(!dtd.isValid()); QVERIFY(dtd.errorString().contains("DOCTYPE"));
    }

    void generatedSvgHybridPreservesLayerOrderAndExportTransform() {
        QTest::failOnWarning(QRegularExpression(QStringLiteral(".*path truncated.*")));
        QDomDocument original;
        QVERIFY(original.setContent(longCurveSvg()));
        const auto longPath = original.elementsByTagName("path").item(0).toElement().attribute("d").toUtf8();
        const QByteArray simplePath("M10 30L90 30L90 90L10 90ZM30 45L30 75L70 75L70 45ZM92 32L98 32L98 38L92 38Z");
        const auto document = [](const QByteArray& path, bool transform) {
            QByteArray svg("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 120 120' width='120' height='120'>");
            if (transform) svg += "<g transform='matrix(0 1 -1 0 120 0)'>";
            svg += "<rect id='background' width='120' height='120' fill='white'/>"
                "<g id='fill'><path d='M0 0H100V100H0Z' fill='#f0bbbb'/>"
                "<path fill='#804020' fill-opacity='.5' fill-rule='nonzero' d='"+path+"'/>"
                "<path d='M40 30H50V45H40Z' fill='#2244dd'/></g>"
                "<g id='lines' opacity='.5' fill='none' stroke='black' stroke-width='4'>"
                "<path d='M20 30V90'/><path d='M10 50H90'/></g>";
            if (transform) svg += "</g>";
            return svg+"</svg>";
        };
        for (bool transform : {false, true}) {
            const auto svg = document(longPath, transform);
            Vector::GeneratedSvgRenderer renderer(svg);
            if (!renderer.isValid() && renderer.errorString().contains("librsvg"))
                QSKIP("Optional librsvg/Cairo runtime unavailable.");
            QVERIFY2(renderer.isValid(), qPrintable(renderer.errorString()));
            QVERIFY(renderer.usesExtendedRenderer());
            QImage image(240, 240, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
            QString error;
            QVERIFY2(renderer.render(image, QTransform::fromScale(2, 2), &error), qPrintable(error));
            const auto expected = rasterizedSvg(document(simplePath, transform), image.size());
            QVERIFY2(imageError(image, expected) < .3, qPrintable(QString::number(imageError(image, expected))));
        }
    }

    void svgMemoryBudgetBoundaries() {
        constexpr quint64 MiB = 1024ULL*1024;
        constexpr quint64 GiB = 1024*MiB;
        // Unknown memory preserves the old limit, inclusive at its boundary.
        QVERIFY(Vector::svgMemoryError(64*MiB, 0).isEmpty());
        QVERIFY(Vector::svgMemoryError(64*MiB+1, 0).contains("取得できない"));
        QVERIFY(Vector::svgMemoryError(GiB+1, 0).contains("1GiB"));
        // The real large-image fill is above the old serialized cap, but its
        // DOM estimate fits comfortably in this simulated known memory budget.
        QVERIFY(Vector::svgMemoryError(173547721, 16*GiB).isEmpty());
        constexpr quint64 bytes = 173547721 + 40458392;
        constexpr quint64 required = 32*bytes + 64*MiB;
        constexpr quint64 minimumAvailable = (required*10+6)/7;
        QVERIFY(Vector::svgMemoryError(bytes, minimumAvailable).isEmpty());
        QVERIFY(Vector::svgMemoryError(bytes, minimumAvailable-1).contains("不足"));
        QVERIFY(Vector::svgMemoryError(1024, 1).contains("不足"));
        QVERIFY(Vector::svgMemoryError(GiB, 64*GiB).isEmpty());
        QVERIFY(Vector::svgMemoryError(600*MiB, 64*GiB).isEmpty());
        QVERIFY(Vector::svgMemoryError(450*MiB, 64*GiB).isEmpty());
        QVERIFY(Vector::svgMemoryError(600*MiB+450*MiB, 64*GiB).contains("1GiB"));
        QVERIFY(Vector::svgMemoryError(std::numeric_limits<quint64>::max(),
                                        std::numeric_limits<quint64>::max()).contains("1GiB"));
    }

    void svgAssetReadRejectsOversizedAndOutsideFiles() {
        QTemporaryDir cache(files.filePath("svg-cache-XXXXXX")); QVERIFY(cache.isValid());
        const auto path = cache.filePath("layer.svg");
        const auto source = curvedSvg();
        { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(source), source.size()); }
        QString error;
        QCOMPARE(Vector::readSvgAsset(path, cache.path(), &error), source);
        QVERIFY(error.isEmpty());
        // A sparse oversized file must fail from its metadata, before trying
        // to allocate/read a gigabyte or parse its contents.
        { QFile file(path); QVERIFY(file.open(QIODevice::ReadWrite)); QVERIFY(file.resize(qint64(Vector::MaxSvgSerializedBytes)+1)); }
        QVERIFY(Vector::readSvgAsset(path, cache.path(), &error).isEmpty());
        QVERIFY(error.contains("1GiB"));
        QVERIFY(Vector::readSvgAsset(sourcePath, cache.path(), &error).isEmpty());
        QVERIFY(error.contains("不正"));
        { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); }
        QVERIFY(Vector::readSvgAsset(path, cache.path(), &error).isEmpty());
    }

    void svgAssetReadAboveOldLimit() {
        constexpr qint64 bytes = 64LL*1024*1024+1;
        if (!Vector::svgMemoryError(bytes).isEmpty())
            QSKIP("Known memory headroom is insufficient for the >64MiB I/O regression.");
        QTemporaryDir cache(files.filePath("svg-large-cache-XXXXXX")); QVERIFY(cache.isValid());
        const auto path = cache.filePath("layer.svg");
        const auto prefix = curvedSvg();
        { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(prefix), prefix.size()); QVERIFY(file.resize(bytes)); }
        QString error;
        const auto actual = Vector::readSvgAsset(path, cache.path(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(actual.size(), bytes);
        QVERIFY(actual.startsWith(prefix));
        QCOMPARE(actual.back(), char(0));
    }

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

    void correctionTogglePreservesFillAndRestoresOriginalLines() {
        QVERIFY2(generate(), qPrintable(failure));
        panel->findChild<QCheckBox*>("vectorShowFill")->setChecked(true);
        QVERIFY(waitIdle());
        const auto original = savedSvg(); QVERIFY(!original.isNull());
        auto* correction = panel->findChild<QCheckBox*>("vectorCorrectLines"); QVERIFY(correction);
        QVERIFY(!correction->isChecked());
        correction->setChecked(true); QVERIFY(waitIdle());
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        const auto corrected = savedSvg(); QVERIFY(!corrected.isNull());
        const auto paths = [](const QDomElement& group) {
            QStringList values;
            const auto elements = group.elementsByTagName("path");
            for (int i = 0; i < elements.size(); ++i) {
                const auto element = elements.at(i).toElement();
                values << element.attribute("d") << element.attribute("fill") << element.attribute("stroke");
            }
            return values;
        };
        QCOMPARE(paths(group(corrected, "fill")), paths(group(original, "fill")));
        QVERIFY(QSettings().value("vector/correctLines").toBool());
        correction->setChecked(false); QVERIFY(waitIdle());
        const auto restored = savedSvg(); QVERIFY(!restored.isNull());
        QCOMPARE(paths(group(restored, "lines")), paths(group(original, "lines")));
        QVERIFY(!QSettings().value("vector/correctLines").toBool());
        QVERIFY(view->hasVectorPreview());
    }

    void cleanupAndColorEdgeControlsRegenerateAndRestore() {
        auto* cleaning = panel->findChild<QCheckBox*>("vectorCleanLines"); QVERIFY(cleaning);
        auto* fitting = panel->findChild<QCheckBox*>("vectorCorrectLines"); QVERIFY(fitting);
        auto* minimum = panel->findChild<QDoubleSpinBox*>("vectorMinLineLength"); QVERIFY(minimum);
        auto* gap = panel->findChild<QDoubleSpinBox*>("vectorJoinDistance"); QVERIFY(gap);
        auto* tolerance = panel->findChild<QDoubleSpinBox*>("vectorShapeTolerance"); QVERIFY(tolerance);
        auto* branch = panel->findChild<QSlider*>("vectorBranchStrength"); QVERIFY(branch);
        auto* scroll = panel->findChild<QScrollArea*>("vectorParameters"); QVERIFY(scroll);
        QVERIFY(!cleaning->isChecked()); QVERIFY(!minimum->isEnabled()); QVERIFY(!tolerance->isEnabled());
        QVERIFY2(generate(), qPrintable(failure));
        const auto original = savedSvg(); QVERIFY(!original.isNull());
        const auto geometry = [](const QDomElement& layer) {
            QStringList result;
            for (auto path = layer.firstChildElement(); !path.isNull(); path = path.nextSiblingElement())
                result << path.attribute("d") << path.attribute("stroke");
            return result;
        };
        cleaning->setChecked(true); minimum->setValue(8); gap->setValue(3); branch->setValue(60);
        fitting->setChecked(true); tolerance->setValue(3);
        QVERIFY(select("vectorMaskGap", "1"));
        QVERIFY(minimum->isEnabled()); QVERIFY(tolerance->isEnabled());
        QVERIFY(waitIdle()); QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QVERIFY(panel->findChild<QLabel*>("vectorStatus")->text().contains(QStringLiteral("主線整理ON")));
        QVERIFY(select("vectorLineMode", "color")); QVERIFY(waitIdle());
        QVERIFY(!panel->findChild<QComboBox*>("vectorMaskGap")->isEnabled());
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QVERIFY(!group(savedSvg(), "lines").firstChildElement().isNull());
        QCOMPARE(QSettings().value("vector/lineMode").toString(), QString("color"));
        QCOMPARE(QSettings().value("vector/minLineLength").toDouble(), 8.);
        QCOMPARE(QSettings().value("vector/shapeTolerance").toDouble(), 3.);
        QCOMPARE(QSettings().value("vector/maskGap").toString(), QString("1"));
        QVERIFY(select("vectorLineMode", "dark")); cleaning->setChecked(false); fitting->setChecked(false);
        QVERIFY(select("vectorMaskGap", "0"));
        QVERIFY(waitIdle()); QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QCOMPARE(geometry(group(savedSvg(), "lines")), geometry(group(original, "lines")));
        QCOMPARE(digest(sourcePath), sourceDigest);
        controller->showPanel(); panel->resize(500, 600);
        scroll->ensureWidgetVisible(panel->findChild<QDoubleSpinBox*>("vectorPreviewScale"));
        QCoreApplication::processEvents();
        QVERIFY(panel->findChild<QPushButton*>("vectorSavePng")->isVisibleTo(panel));
        const auto artifacts = qEnvironmentVariable("ARTIFACTS");
        if (!artifacts.isEmpty()) {
            QVERIFY(QDir().mkpath(artifacts));
            scroll->ensureWidgetVisible(panel->findChild<QComboBox*>("vectorLineMode"));
            QCoreApplication::processEvents();
            QVERIFY(panel->grab().save(artifacts+"/line-controls.png"));
        }
    }

    void manyNestedLinePathsPreserveDocumentOrder() {
        const auto worker = files.filePath("nested-path-worker.py");
        const QByteArray script =
            "import json, sys\n"
            "from pathlib import Path\n"
            "cache = Path(sys.argv[sys.argv.index('--cache')+1])\n"
            "cache.mkdir(parents=True, exist_ok=True)\n"
            "prefix = '<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"64\" height=\"48\" viewBox=\"0 0 64 48\">'\n"
            "fill = cache/'fill.svg'\n"
            "lines = cache/'lines.svg'\n"
            "fill.write_text(prefix+'</svg>')\n"
            "parts = [prefix]\n"
            "for i in range(4096):\n"
            "    if i % 32 == 0:\n"
            "        parts.append('<g><g>')\n"
            "    if i % 32 == 16:\n"
            "        parts.append('</g><g>')\n"
            "    color = '#123456' if i % 2 else '#654321'\n"
            "    parts.append(f'<path id=\"p{i}\" d=\"M{i%64} 4L{i%64} 8\" stroke=\"{color}\" stroke-width=\"9\"/>')\n"
            "    if i % 32 == 31:\n"
            "        parts.append('</g></g>')\n"
            "parts.append('</svg>')\n"
            "lines.write_text(''.join(parts))\n"
            "print(json.dumps(dict(width=64, height=48, fill_svg=str(fill), lines_svg=str(lines))))\n";
        { QFile file(worker); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(script), script.size()); }
        const auto oldWorker = qgetenv("QVIEWSR_VECTOR_WORKER");
        const bool hadWorker = qEnvironmentVariableIsSet("QVIEWSR_VECTOR_WORKER");
        qputenv("QVIEWSR_VECTOR_WORKER", worker.toUtf8());
        const auto restoreWorker = qScopeGuard([&] {
            if (hadWorker) qputenv("QVIEWSR_VECTOR_WORKER", oldWorker);
            else qunsetenv("QVIEWSR_VECTOR_WORKER");
        });
        QVERIFY2(generate(), qPrintable(failure));
        const auto svg = savedSvg();
        QVERIFY2(!svg.isNull(), qPrintable(failure));
        const auto paths = group(svg, "lines").elementsByTagName("path");
        QCOMPARE(paths.size(), 4096);
        for (int i = 0; i < 4096; ++i) {
            const auto path = paths.item(i).toElement();
            QCOMPARE(path.attribute("id"), QString("p%1").arg(i));
            QCOMPARE(path.attribute("d"), QString("M%1 4L%1 8").arg(i%64));
            QCOMPARE(path.attribute("stroke"), i%2 ? QString("#123456") : QString("#654321"));
            QCOMPARE(path.attribute("fill"), QString("none"));
            QVERIFY(!path.hasAttribute("stroke-width"));
        }
        QCOMPARE(digest(sourcePath), sourceDigest);
    }

    void largeDimensionsKeepSvgAndTransferResolution() {
        const QString path = files.filePath("wide.png");
        QImage input(12001, 40, QImage::Format_RGB32); input.fill(Qt::white);
        { QPainter painter(&input); painter.setPen(QPen(Qt::black, 2)); painter.drawLine(10, 10, 11990, 30); }
        QVERIFY(input.save(path));
        window->openFile(path);
        QTRY_COMPARE_WITH_TIMEOUT(view->getImageCore().getSourceImage().size(), input.size(), 5000);
        QVERIFY2(generate(30000), qPrintable(failure));
        QVERIFY(controller->resultImage().width() <= 8192);
        QVERIFY(view->hasVectorPreview());
        view->originalSize();
        const auto logical = view->transform().mapRect(QRectF(QPointF(), QSizeF(view->getLoadedPixmap().size())));
        QVERIFY(qAbs(logical.width() - input.width()) < .01);
        QVERIFY(qAbs(logical.height() - input.height()) < .01);
        const auto svg = savedSvg();
        QCOMPARE(svg.documentElement().attribute("viewBox"), QString("0 0 12001 40"));
        QString error;
        QVERIFY(!controller->savePng(files.filePath("too-wide.png"), 4, &error));
        QVERIFY(error.contains("32768"));
        std::unique_ptr<QMimeData> oversized(view->getFileDragMimeData(&error));
        QVERIFY(oversized && oversized->urls().isEmpty());
        QVERIFY(error.contains("倍率"));
        QVERIFY(controller->savePng(files.filePath("small.png"), .5, &error));
        QCOMPARE(QImage(files.filePath("small.png")).size(), QSize(6001, 20));
        panel->findChild<QDoubleSpinBox*>("vectorPreviewScale")->setValue(1);
        QVERIFY(waitIdle());
        std::unique_ptr<QMimeData> mime(view->getFileDragMimeData(&error));
        QVERIFY2(mime != nullptr, qPrintable(error));
        QCOMPARE(QImage(mime->urls().first().toLocalFile()).size(), input.size());
        // The requested transfer must never silently receive the reduced proxy.
        QVERIFY(controller->resultImage().width() < input.width());
    }

    void largeSquareImage() {
        const auto path = qEnvironmentVariable("QVIEWSR_TEST_LARGE_VECTOR_IMAGE");
        if (path.isEmpty()) QSKIP("Opt-in full-resolution large-image test.");
        QTest::failOnWarning(QRegularExpression(QStringLiteral(".*path truncated.*")));
        const auto hash = digest(path); QVERIFY(!hash.isEmpty());
        window->openFile(path);
        QTRY_VERIFY_WITH_TIMEOUT(view->getCurrentFileDetails().fileInfo.absoluteFilePath() == QFileInfo(path).absoluteFilePath()
                                  && view->getCurrentFileDetails().isPixmapLoaded, 60000);
        const auto sourceSize = view->getImageCore().getSourceImage().size();
        QVERIFY(sourceSize.width() > 10000);
        panel->findChild<QCheckBox*>("vectorCorrectLines")->setChecked(true);
        QVERIFY2(generate(3300000), qPrintable(failure));
        auto* fill = panel->findChild<QCheckBox*>("vectorShowFill");
        if (!fill->isChecked()) {
            QSignalSpy rendered(controller, &Vector::Controller::resultReady);
            fill->setChecked(true);
            QTRY_VERIFY_WITH_TIMEOUT(rendered.count() && !controller->isBusy(), 300000);
        }
        const auto svg = savedSvg();
        QVERIFY(!group(svg, "fill").isNull());
        QCOMPARE(svg.documentElement().attribute("viewBox"), QString("0 0 %1 %2").arg(sourceSize.width()).arg(sourceSize.height()));
        const auto proxy = controller->resultImage();
        QVERIFY(qint64(proxy.width()) * proxy.height() <= 16000000);
        QVERIFY(view->hasVectorPreview());
        const auto artifacts = qEnvironmentVariable("ARTIFACTS");
        if (!artifacts.isEmpty()) {
            QVERIFY(QDir().mkpath(artifacts)); QString error;
            QVERIFY2(controller->saveSvg(artifacts+"/large.svg", &error), qPrintable(error));
            QVERIFY2(controller->savePng(artifacts+"/large-quarter.png", .25, &error), qPrintable(error));
            controller->showPanel();
            QVERIFY(panel->grab().save(artifacts+"/large-panel.png"));
            QVERIFY(window->grab().save(artifacts+"/large-window.png"));
        }
        QCOMPARE(digest(path), hash);
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

    void pngPreparationStopsAtWriteBoundaryAndKeepsPreviousFile() {
        const auto path = files.filePath("cancelled-input.png");
        const QByteArray original("previous complete input");
        { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(original), original.size()); }
        const auto cancelled = std::make_shared<std::atomic_bool>(false);
        class CancelAfterFirstChunk : public Vector::CancellableSaveFile {
        public:
            CancelAfterFirstChunk(const QString& path, const std::shared_ptr<std::atomic_bool>& flag)
                : CancellableSaveFile(path, flag), flag_(flag) {}
            int rejectedWrites = 0;
        protected:
            qint64 writeData(const char* bytes, qint64 size) override {
                const auto written = CancellableSaveFile::writeData(bytes, size);
                if (written > 0) flag_->store(true, std::memory_order_relaxed);
                else if (written < 0) ++rejectedWrites;
                return written;
            }
        private:
            std::shared_ptr<std::atomic_bool> flag_;
        };
        // Noise ensures multiple PNG chunks, while keeping the test below 1MiB.
        QImage image(256, 256, QImage::Format_RGB32);
        quint32 state = 0x92345;
        for (int y = 0; y < image.height(); ++y) {
            auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                state ^= state << 13; state ^= state >> 17; state ^= state << 5;
                row[x] = 0xff000000 | (state & 0x00ffffff);
            }
        }
        {
            CancelAfterFirstChunk file(path, cancelled);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QImageWriter writer(&file, "png");
            QVERIFY(!writer.write(image));
            QVERIFY(file.rejectedWrites > 0);
            QVERIFY(!file.commit());
        }
        { QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), original); }

        // A stop after the final data chunk must also prevent publication.
        cancelled->store(false, std::memory_order_relaxed);
        const auto unpublished = files.filePath("unpublished-input.png");
        {
            Vector::CancellableSaveFile file(unpublished, cancelled);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QImageWriter writer(&file, "png");
            QVERIFY(writer.write(fixture()));
            cancelled->store(true, std::memory_order_relaxed);
            QVERIFY(!file.commit());
        }
        QVERIFY(!QFileInfo::exists(unpublished));
    }

    void cancellingQueuedPreparationSkipsInputAndAllowsRetry() {
        QTemporaryDir temporary(files.filePath("prepare-XXXXXX")); QVERIFY(temporary.isValid());
        const auto oldTemporary = qgetenv("TMPDIR");
        const bool hadTemporary = qEnvironmentVariableIsSet("TMPDIR");
        qputenv("TMPDIR", temporary.path().toUtf8());
        const auto restoreTemporary = qScopeGuard([&] {
            if (hadTemporary) qputenv("TMPDIR", oldTemporary); else qunsetenv("TMPDIR");
        });
        QCOMPARE(QDir::tempPath(), temporary.path());
        auto* pool = QThreadPool::globalInstance();
        QVERIFY(pool->waitForDone(5000));
        const auto oldMaximum = pool->maxThreadCount();
        pool->setMaxThreadCount(1); pool->reserveThread();
        bool reserved = true;
        const auto restorePool = qScopeGuard([&] {
            if (reserved) pool->releaseThread();
            pool->setMaxThreadCount(oldMaximum);
        });
        QSignalSpy ready(controller, &Vector::Controller::resultReady);
        controller->generate(); QVERIFY(controller->isBusy());
        const auto folders = QDir(temporary.path()).entryList({"qviewsr-vector-*"}, QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(folders.size(), 1);
        const auto input = QDir(temporary.path()).filePath(folders.first()+"/input.png");
        controller->cancel();
        pool->releaseThread(); reserved = false; pool->setMaxThreadCount(oldMaximum);
        QVERIFY(waitIdle(5000));
        QVERIFY(!QFileInfo::exists(input));
        QCOMPARE(ready.count(), 0);
        QVERIFY(!controller->hasResult());
        QVERIFY2(generate(), qPrintable(failure));
        QVERIFY(QFileInfo::exists(input));
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

    void vectorInputUsesLatestSrPixelsAcrossPreviewAndSourceSelection() {
        QTemporaryDir temporary(files.filePath("sr-input-XXXXXX")); QVERIFY(temporary.isValid());
        const auto oldTemporary = qgetenv("TMPDIR");
        const bool hadTemporary = qEnvironmentVariableIsSet("TMPDIR");
        qputenv("TMPDIR", temporary.path().toUtf8());
        const auto restoreTemporary = qScopeGuard([&] {
            if (hadTemporary) qputenv("TMPDIR", oldTemporary); else qunsetenv("TMPDIR");
        });
        sourcePath = files.filePath("opaque-sr-source.png");
        QVERIFY(opaqueFixture().save(sourcePath)); sourceDigest = digest(sourcePath);
        window->openFile(sourcePath);
        QTRY_COMPARE(view->getCurrentFileDetails().fileInfo.absoluteFilePath(), sourcePath);
        QTRY_VERIFY(view->getCurrentFileDetails().isPixmapLoaded);
        auto* inputChoice = panel->findChild<QComboBox*>("vectorInputSource"); QVERIFY(inputChoice);
        auto* inputStatus = panel->findChild<QLabel*>("vectorInputStatus"); QVERIFY(inputStatus);
        QCOMPARE(inputChoice->currentData().toString(), QStringLiteral("auto"));
        QVERIFY(inputStatus->text().contains(QStringLiteral("元画像")));
        QVERIFY2(generate(), qPrintable(failure));
        const auto originalInput = QImage(preparedInput(temporary));
        QCOMPARE(originalInput.size(), QSize(64, 48));
        QCOMPARE(QSvgRenderer(savedSvg().toByteArray()).defaultSize(), QSize(64, 48));

        auto* sr = window->findChild<Sr::Controller*>(); QVERIFY(sr);
        const auto previous = sr->configuration();
        const auto restoreSr = qScopeGuard([&] { sr->setConfiguration(previous); });
        QVERIFY(configureFakeSr(sr, "normal"));
        QTRY_VERIFY_WITH_TIMEOUT(sr->backendReady(), 5000);
        QSignalSpy srReady(sr, &Sr::Controller::resultReady);
        sr->start();
        QTRY_COMPARE_WITH_TIMEOUT(srReady.count(), 1, 8000);
        const auto processed = sr->resultImage();
        QCOMPARE(processed.size(), QSize(128, 96));
        QCOMPARE(processed.pixelColor(30, 30), QColor(80, 160, 208));
        QVERIFY(inputStatus->text().contains(QStringLiteral("超解像")));
        QVERIFY(inputStatus->text().contains(QStringLiteral("128")));
        QVERIFY2(generate(), qPrintable(failure));
        const auto srInputPath = preparedInput(temporary); QVERIFY(!srInputPath.isEmpty());
        const auto srInput = QImage(srInputPath);
        QCOMPARE(srInput.size(), processed.size());
        QCOMPARE(srInput.colorSpace(), QColorSpace(QColorSpace::SRgb));
        QVERIFY2(imageError(srInput, processed) < .1, "Vector input must contain SR pixels without applying the original Adobe RGB profile again");
        QCOMPARE(QSvgRenderer(savedSvg().toByteArray()).defaultSize(), QSize(128, 96));
        QVERIFY(controller->showingResult()); QVERIFY(!sr->showingSr());
        QVERIFY(view->hasVectorPreview());

        // Regenerating from the vector preview must keep the processed input,
        // even though the SR display flag is no longer set.
        QVERIFY(select("vectorDetail", "fine"));
        QCOMPARE(preparedInput(temporary), srInputPath);
        QVERIFY(imageError(QImage(preparedInput(temporary)), processed) < .1);
        QCOMPARE(QSvgRenderer(savedSvg().toByteArray()).defaultSize(), QSize(128, 96));

        controller->setShowingResult(false);
        QVERIFY(!sr->showingSr());
        QCOMPARE(view->getLoadedPixmap().size(), QSize(64, 48));
        QVERIFY2(generate(), qPrintable(failure));
        QVERIFY(imageError(QImage(preparedInput(temporary)), processed) < .1);
        QCOMPARE(QSvgRenderer(savedSvg().toByteArray()).defaultSize(), QSize(128, 96));

        // Explicit original mode restores the original pixels and ICC path.
        QVERIFY(select("vectorInputSource", "original"));
        QVERIFY(inputStatus->text().contains(QStringLiteral("元画像")));
        QCOMPARE(QImage(preparedInput(temporary)).size(), QSize(64, 48));
        QVERIFY(imageError(QImage(preparedInput(temporary)), originalInput) < .1);
        QCOMPARE(QSvgRenderer(savedSvg().toByteArray()).defaultSize(), QSize(64, 48));
        QVERIFY(select("vectorInputSource", "auto"));
        QVERIFY(imageError(QImage(preparedInput(temporary)), processed) < .1);
        QCOMPARE(QSvgRenderer(savedSvg().toByteArray()).defaultSize(), QSize(128, 96));
        QCOMPARE(digest(sourcePath), sourceDigest);
    }

    void refreshedSrInputInvalidatesCacheAndSurvivesFailedJobs() {
        QTemporaryDir temporary(files.filePath("sr-refresh-XXXXXX")); QVERIFY(temporary.isValid());
        const auto oldTemporary = qgetenv("TMPDIR");
        const bool hadTemporary = qEnvironmentVariableIsSet("TMPDIR");
        qputenv("TMPDIR", temporary.path().toUtf8());
        const auto restoreTemporary = qScopeGuard([&] {
            if (hadTemporary) qputenv("TMPDIR", oldTemporary); else qunsetenv("TMPDIR");
        });
        sourcePath = files.filePath("opaque-refresh-source.png");
        QVERIFY(opaqueFixture().save(sourcePath)); sourceDigest = digest(sourcePath);
        window->openFile(sourcePath);
        QTRY_COMPARE(view->getCurrentFileDetails().fileInfo.absoluteFilePath(), sourcePath);
        QTRY_VERIFY(view->getCurrentFileDetails().isPixmapLoaded);
        auto* sr = window->findChild<Sr::Controller*>(); QVERIFY(sr);
        const auto previous = sr->configuration();
        const auto restoreSr = qScopeGuard([&] { sr->setConfiguration(previous); });
        QVERIFY(configureFakeSr(sr, "normal"));
        QTRY_VERIFY_WITH_TIMEOUT(sr->backendReady(), 5000);
        QSignalSpy srReady(sr, &Sr::Controller::resultReady);
        sr->start();
        QTRY_COMPARE_WITH_TIMEOUT(srReady.count(), 1, 8000);
        QVERIFY2(generate(), qPrintable(failure));
        const auto firstInput = QImage(preparedInput(temporary));
        QCOMPARE(firstInput.size(), QSize(128, 96));

        // The fake worker's animation-* mode changes its output color based on
        // the input hash; the loaded document remains a still image.
        QVERIFY(configureFakeSr(sr, "animation-input-color"));
        QTRY_VERIFY_WITH_TIMEOUT(sr->backendReady(), 5000);
        sr->start();
        QTRY_COMPARE_WITH_TIMEOUT(srReady.count(), 2, 8000);
        QVERIFY(!sr->hasAnimation());
        const auto replaced = sr->resultImage();
        QCOMPARE(replaced.size(), firstInput.size());
        QVERIFY(imageError(replaced, firstInput) > 1);
        QVERIFY2(generate(), qPrintable(failure));
        QVERIFY2(imageError(QImage(preparedInput(temporary)), replaced) < .1,
                 "A same-sized new SR result must replace the prepared PNG cache");

        sr->startAgain();
        QTRY_COMPARE_WITH_TIMEOUT(srReady.count(), 3, 8000);
        const auto repeated = sr->resultImage();
        QCOMPARE(repeated.size(), QSize(256, 192));
        QVERIFY2(generate(), qPrintable(failure));
        QVERIFY(imageError(QImage(preparedInput(temporary)), repeated) < .1);
        QCOMPARE(QSvgRenderer(savedSvg().toByteArray()).defaultSize(), QSize(256, 192));
        const auto stableInputPath = preparedInput(temporary);
        const auto stableInputDigest = digest(stableInputPath);

        QVERIFY(configureFakeSr(sr, "long-error"));
        QTRY_VERIFY_WITH_TIMEOUT(sr->backendReady(), 5000);
        QSignalSpy failed(sr, &Sr::Controller::failed);
        sr->start();
        QVERIFY(!panel->findChild<QPushButton*>("vectorGenerate")->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(failed.count() > 0, 8000);
        QTRY_VERIFY(!sr->isBusy());
        QCOMPARE(sr->resultImage(), repeated);
        QVERIFY2(generate(), qPrintable(failure));
        QCOMPARE(preparedInput(temporary), stableInputPath);
        QCOMPARE(digest(preparedInput(temporary)), stableInputDigest);

        QVERIFY(configureFakeSr(sr, "delay"));
        QTRY_VERIFY_WITH_TIMEOUT(sr->backendReady(), 5000);
        sr->start(); QVERIFY(sr->isBusy());
        sr->cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!sr->isBusy(), 8000);
        QCOMPARE(sr->resultImage(), repeated);
        QVERIFY2(generate(), qPrintable(failure));
        QCOMPARE(preparedInput(temporary), stableInputPath);
        QCOMPARE(digest(preparedInput(temporary)), stableInputDigest);

        const auto other = files.filePath("after-sr-other.png");
        QImage different(31, 25, QImage::Format_RGB32); different.fill(Qt::green); QVERIFY(different.save(other));
        const auto otherDigest = digest(other);
        QSignalSpy vectorReady(controller, &Vector::Controller::resultReady);
        controller->generate(); QVERIFY(controller->isBusy());
        window->openFile(other);
        QTRY_COMPARE(view->getImageCore().getSourceImage().size(), QSize(31, 25));
        QVERIFY(waitIdle(5000));
        QCOMPARE(vectorReady.count(), 0);
        QVERIFY(!sr->hasResult()); QVERIFY(!controller->hasResult());
        QVERIFY(!controller->showingResult()); QVERIFY(!view->hasVectorPreview());
        QVERIFY2(generate(), qPrintable(failure));
        QCOMPARE(QImage(preparedInput(temporary)).size(), QSize(31, 25));
        QCOMPARE(QSvgRenderer(savedSvg().toByteArray()).defaultSize(), QSize(31, 25));
        QVERIFY(panel->findChild<QLabel*>("vectorInputStatus")->text().contains(QStringLiteral("元画像")));
        QCOMPARE(digest(sourcePath), sourceDigest); QCOMPARE(digest(other), otherDigest);
    }

    void editBezierAddColorDeleteUndoAndExport() {
        QVERIFY2(generate(), qPrintable(failure));
        const auto originalSvg = savedSvg().toByteArray(-1);
        controller->setEditing(true);
        QTRY_VERIFY_WITH_TIMEOUT(controller->isEditing(), 10000); QVERIFY(waitIdle());
        auto* editor = view->findChild<Vector::Editor*>(); QVERIFY(editor);
        const int beforeCount = editor->document().pathCount();
        editor->setAddLine(true); editor->setLineWidth(1.5);
        dragAt(editorPoint({8, 3}), editorPoint({56, 3}));
        QVERIFY(waitIdle()); QVERIFY(controller->hasEdits());
        QCOMPARE(editor->document().pathCount(), beforeCount+1);
        const int added = editor->selectedPath(); QVERIFY(added >= 0);
        editor->setAddLine(false); // Keep selection by reselecting the new path.
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, editorPoint({30, 3}));
        QCOMPARE(editor->selectedPath(), added);
        editor->setColor(Qt::red); QVERIFY(waitIdle());
        QCOMPARE(editor->document().color(added), QColor(Qt::red));
        const auto beforeCurve = editor->svg();
        const auto handles = editor->document().handles(added);
        bool moved = false;
        for (const auto& handle : handles) {
            if (handle.kind != Vector::EditDocument::Handle::Control1) continue;
            dragAt(editorPoint(handle.point), editorPoint(handle.point+QPointF(0, 8)));
            moved = true; break;
        }
        QVERIFY(moved); QVERIFY(waitIdle()); QVERIFY(editor->svg() != beforeCurve);
        const auto curved = editor->svg();
        QTest::keyClick(view->viewport(), Qt::Key_Z, Qt::ControlModifier); QVERIFY(waitIdle());
        QCOMPARE(editor->svg(), beforeCurve);
        QTest::keyClick(view->viewport(), Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier); QVERIFY(waitIdle());
        QCOMPARE(editor->svg(), curved);
        window->rotateRight(); window->mirror(); QCoreApplication::processEvents();
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier,
                          editorPoint(editor->document().path(added).pointAtPercent(.5)));
        const auto transformedHandles = editor->document().handles(added);
        for (const auto& handle : transformedHandles) {
            if (handle.kind != Vector::EditDocument::Handle::Control2) continue;
            dragAt(editorPoint(handle.point), editorPoint(handle.point+QPointF(2, 0)));
            QVERIFY(waitIdle());
            const auto changedHandles = editor->document().handles(added);
            bool checked = false;
            for (const auto& changed : changedHandles) {
                if (changed.kind != handle.kind || changed.index != handle.index) continue;
                QVERIFY(QLineF(changed.point, handle.point+QPointF(2,0)).length() < .3); checked = true;
            }
            QVERIFY(checked); break;
        }
        editor->undo(); QVERIFY(waitIdle()); QCOMPARE(editor->svg(), curved);
        window->mirror(); window->rotateLeft(); QCoreApplication::processEvents();
        // Restore selection after undo/redo, then Delete must remove one path.
        const auto middle = editor->document().path(added).pointAtPercent(.5);
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, editorPoint(middle));
        QCOMPARE(editor->selectedPath(), added);
        QTest::keyClick(view->viewport(), Qt::Key_Delete); QVERIFY(waitIdle());
        QVERIFY(editor->document().path(added).isEmpty());
        editor->undo(); QVERIFY(waitIdle()); QCOMPARE(editor->svg(), curved);
        QString error;
        const auto svgPath = files.filePath("edited.svg"), pngPath = files.filePath("edited.png");
        QVERIFY2(controller->saveSvg(svgPath, &error), qPrintable(error));
        QVERIFY2(controller->savePng(pngPath, 2, &error), qPrintable(error));
        const QImage png(pngPath); QCOMPARE(png.size(), QSize(128, 96));
        int red = 0;
        for (int y=0; y<png.height(); ++y) for(int x=0; x<png.width(); ++x) {
            const auto c = png.pixelColor(x,y); red += c.red()>200 && c.green()<100 && c.blue()<100;
        }
        QVERIFY(red > 10);
        QVERIFY(savedSvg().toByteArray(-1) != originalSvg);
        controller->setEditing(false); QVERIFY(!controller->isEditing()); QVERIFY(controller->hasEdits());
        QVERIFY(!panel->findChild<QPushButton*>("vectorGenerate")->isEnabled());
        const auto retained = savedSvg().toByteArray(-1);
        controller->setEditing(true); QTRY_VERIFY_WITH_TIMEOUT(controller->isEditing(), 10000); QVERIFY(waitIdle());
        QCOMPARE(savedSvg().toByteArray(-1), retained);
        controller->setEditing(false); controller->discardEdits(); QVERIFY(!controller->hasEdits());
        QCOMPARE(savedSvg().toByteArray(-1), originalSvg);
        QCOMPARE(digest(sourcePath), sourceDigest);
    }

    void editFillAndSpacePanBlockFileNavigation() {
        QVERIFY2(generate(), qPrintable(failure)); controller->setEditing(true);
        QTRY_VERIFY_WITH_TIMEOUT(controller->isEditing(), 10000); QVERIFY(waitIdle());
        auto* editor = view->findChild<Vector::Editor*>(); QVERIFY(editor);
        QVERIFY(select("vectorEditLayer", "fill"));
        panel->findChild<QDoubleSpinBox*>("vectorBrushRadius")->setValue(12);
        int fill = -1;
        for(int i=0; i<editor->document().pathCount(); ++i)
            if(editor->document().layer(i)==Vector::EditDocument::Layer::Fill) fill=i;
        QVERIFY(fill>=0);
        const auto boundary = editor->document().path(fill).pointAtPercent(.35);
        const auto before = editor->svg();
        dragAt(editorPoint(boundary), editorPoint(boundary+QPointF(3,2)));
        QVERIFY(waitIdle()); QVERIFY(editor->svg()!=before);
        QVERIFY(editor->document().canUndo()); editor->undo(); QVERIFY(waitIdle());
        QCOMPARE(editor->svg(), before);
        const auto other = files.filePath("z-navigation-editor.png"); QVERIFY(fixture().save(other));
        const auto current = view->getCurrentFileDetails().fileInfo.absoluteFilePath();
        QTest::keyClick(view->viewport(), Qt::Key_Right);
        QTest::keyClick(view->viewport(), Qt::Key_Left);
        window->nextFile(); window->previousFile(); QTest::qWait(100);
        QCOMPARE(view->getCurrentFileDetails().fileInfo.absoluteFilePath(), current);
        QVERIFY(controller->isEditing());
        view->setTransform(QTransform::fromScale(30, 30)); QCoreApplication::processEvents();
        auto* bar = view->horizontalScrollBar(); QVERIFY(bar->maximum()>bar->minimum());
        bar->setValue((bar->minimum()+bar->maximum())/2);
        const int initial = bar->value();
        const auto center = view->viewport()->rect().center();
        QTest::keyPress(view->viewport(), Qt::Key_Space);
        dragAt(center, center+QPoint(60, 0));
        QTest::keyRelease(view->viewport(), Qt::Key_Space);
        QVERIFY(bar->value()!=initial); QCOMPARE(editor->svg(), before);
        const auto artifacts = qEnvironmentVariable("ARTIFACTS");
        if (!artifacts.isEmpty()) {
            controller->showPanel(); QCoreApplication::processEvents();
            QVERIFY(panel->grab().save(artifacts+"/vector-editor-panel.png"));
            QVERIFY(window->grab().save(artifacts+"/vector-editor-window.png"));
        }
        controller->setEditing(false); QVERIFY(!view->property("vectorEditing").toBool());
        QCOMPARE(digest(sourcePath), sourceDigest);
    }

    void editHistoryAndVisibilityAreScopedToCurrentImage() {
        QVERIFY2(generate(), qPrintable(failure));
        const auto baseline = savedSvg().toByteArray(-1);
        controller->setEditing(true); QTRY_VERIFY_WITH_TIMEOUT(controller->isEditing(), 10000); QVERIFY(waitIdle());
        auto* editor = view->findChild<Vector::Editor*>(); QVERIFY(editor);
        controller->setEditing(false); QCOMPARE(savedSvg().toByteArray(-1), baseline);
        controller->setEditing(true); QTRY_VERIFY_WITH_TIMEOUT(controller->isEditing(), 10000); QVERIFY(waitIdle());
        editor->setAddLine(true); dragAt(editorPoint({10, 3}), editorPoint({50, 3})); QVERIFY(waitIdle());
        editor->setAddLine(false);
        const int added = editor->selectedPath(); QVERIFY(added >= 0);
        const auto complete = editor->svg();
        const auto handle = editor->document().handles(added).at(1);
        auto* viewport = view->viewport(); const auto from = editorPoint(handle.point), to = editorPoint(handle.point+QPointF(0,10));
        QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, from);
        QMouseEvent move(QEvent::MouseMove, to, viewport->mapToGlobal(to), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &move);
        QTest::keyClick(viewport, Qt::Key_Escape);
        QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, to);
        QCOMPARE(editor->svg(), complete);
        editor->undo(); QVERIFY(waitIdle()); QVERIFY(!controller->hasEdits()); QVERIFY(editor->document().canRedo());
        controller->setEditing(false); QCOMPARE(savedSvg().toByteArray(-1), baseline);
        const auto identical = files.filePath("identical-editor-image.png"); QVERIFY(fixture().save(identical));
        window->openFile(identical);
        QTRY_COMPARE(view->getCurrentFileDetails().fileInfo.absoluteFilePath(), identical);
        QTRY_VERIFY(view->getCurrentFileDetails().isPixmapLoaded);
        QVERIFY2(generate(), qPrintable(failure));
        controller->setEditing(true); QTRY_VERIFY_WITH_TIMEOUT(controller->isEditing(), 10000); QVERIFY(waitIdle());
        QVERIFY(!editor->document().canUndo()); QVERIFY(!editor->document().canRedo());
        QCOMPARE(digest(sourcePath), sourceDigest);
    }

    void largeEditUndoRunsAsynchronouslyAndCancelsOnReset() {
        QVERIFY2(generate(), qPrintable(failure)); controller->setEditing(true);
        QTRY_VERIFY_WITH_TIMEOUT(controller->isEditing(), 10000); QVERIFY(waitIdle());
        auto* editor = view->findChild<Vector::Editor*>(); QVERIFY(editor);
        auto padded = editor->svg();
        const auto at = padded.lastIndexOf("</svg>"); QVERIFY(at > 0);
        padded.insert(at, QByteArray("<!--") + QByteArray(9*1024*1024, 'x') + "-->");
        QString error; QVERIFY2(editor->begin(padded, &error), qPrintable(error));
        editor->setAddLine(true); dragAt(editorPoint({8,3}), editorPoint({55,3})); QVERIFY(waitIdle());
        const auto modified = editor->svg(); QVERIFY(modified != padded);
        editor->undo(); QVERIFY(editor->isBusy()); QVERIFY(controller->isBusy());
        QVERIFY(!panel->findChild<QPushButton*>("vectorSaveSvg")->isEnabled());
        bool responsive = false; QTimer::singleShot(0, [&]{ responsive = true; });
        QTRY_VERIFY(responsive);
        QVERIFY(waitIdle(30000)); QCOMPARE(editor->svg(), padded);
        editor->redo(); QVERIFY(editor->isBusy()); QVERIFY(waitIdle(30000)); QCOMPARE(editor->svg(), modified);
        editor->undo(); QVERIFY(editor->isBusy());
        const auto other = files.filePath("large-edit-reset.png"); QVERIFY(fixture().save(other));
        window->openFile(other);
        QTRY_COMPARE(view->getCurrentFileDetails().fileInfo.absoluteFilePath(), other);
        QThreadPool::globalInstance()->waitForDone(); QCoreApplication::processEvents();
        QVERIFY(!editor->active()); QVERIFY(!editor->isBusy());
        QVERIFY(!controller->hasResult()); QVERIFY(!controller->hasEdits());
        QCOMPARE(digest(sourcePath), sourceDigest);
    }

    void realUserImage() {
        const auto source = qEnvironmentVariable("QVIEWSR_TEST_VECTOR_IMAGE");
        if (source.isEmpty()) QSKIP("Set QVIEWSR_TEST_VECTOR_IMAGE to enable the opt-in real image test.");
        QVERIFY(QFileInfo(source).isReadable());
        const auto hash = digest(source);
        window->openFile(source);
        QTRY_COMPARE_WITH_TIMEOUT(view->getCurrentFileDetails().fileInfo.absoluteFilePath(), QFileInfo(source).absoluteFilePath(), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(view->getCurrentFileDetails().isPixmapLoaded, 15000);
        if (qEnvironmentVariableIsSet("QVIEWSR_TEST_REFINE_LINES")) {
            QVERIFY(select("vectorMaskGap", "1"));
            panel->findChild<QCheckBox*>("vectorCleanLines")->setChecked(true);
            panel->findChild<QCheckBox*>("vectorCorrectLines")->setChecked(true);
            panel->findChild<QDoubleSpinBox*>("vectorShapeTolerance")->setValue(3);
            QVERIFY(select("vectorColor", "black"));
        }
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
    if (!qEnvironmentVariableIsEmpty("QVIEWSR_TEST_LARGE_VECTOR_IMAGE")
        && qEnvironmentVariableIsEmpty("QTEST_FUNCTION_TIMEOUT"))
        qputenv("QTEST_FUNCTION_TIMEOUT", "3600000");
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
