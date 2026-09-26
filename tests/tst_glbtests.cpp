// SPDX-License-Identifier: GPL-3.0-or-later
#include "glb_fixture.h"
#include "model3d/glb_document.h"
#include "model3d/model_view.h"
#include "qvapplication.h"
#include "mainwindow.h"
#include "sr/sr_controller.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QSettings>
#include <QPointer>
#include <QThreadPool>
#include <QToolBar>
#include <QLabel>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QFileDialog>
#include <QLineEdit>
#include <QtQuick3D/qquick3d.h>

class GlbTests : public QObject {
    Q_OBJECT
    QTemporaryDir files;
    QPointer<MainWindow> window;
    QByteArray digest(const QString &path) {
        QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {};
        QCryptographicHash hash(QCryptographicHash::Sha256); hash.addData(&file); return hash.result();
    }
    bool hasModelPixels(const QImage &image) {
        if (image.isNull()) return false;
        const auto background = image.pixelColor(0,0);
        int different = 0;
        for (int y = 0; y < image.height(); y += 8) for (int x = 0; x < image.width(); x += 8) {
            const auto pixel = image.pixelColor(x,y);
            if (qAbs(pixel.red()-background.red()) + qAbs(pixel.green()-background.green())
                    + qAbs(pixel.blue()-background.blue()) > 80) ++different;
        }
        return different > 50;
    }
private slots:
    void initTestCase() {
        QVERIFY(files.isValid());
        QVERIFY(GlbFixture::cube(files.filePath("_2.GLB")));
        QImage image(80,60,QImage::Format_RGB32); image.fill(Qt::green);
        QVERIFY(image.save(files.filePath("_1.png")));
        QVERIFY(image.save(files.filePath("_3.png")));
        const QByteArray gif = QByteArray::fromBase64("R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7");
        QVERIFY(GlbFixture::write(files.filePath("_4.gif"), gif));
        Sr::Configuration config = Sr::Configuration::load();
        config.workerPath = QStringLiteral(SR_SOURCE_DIR)+"/tests/fake_worker.py";
        config.runtimeRoot = files.filePath("runtime"); config.modelPath = files.filePath("model.xml");
        config.displayProfile = "sRGB"; config.devices = "normal";
        QVERIFY(QDir().mkpath(config.runtimeRoot+"/deployment_tools/inference_engine/lib/intel64"));
        QVERIFY(GlbFixture::write(config.runtimeRoot+"/deployment_tools/inference_engine/lib/intel64/libmyriadPlugin.so", "test"));
        QVERIFY(GlbFixture::write(config.modelPath,"test")); config.save();
        QSettings settings; settings.setValue("options/preloadingmode",0);
        settings.setValue("options/sortmode",0); settings.setValue("options/sortdescending",false);
        settings.setValue("options/loopfoldersenabled",true); settings.setValue("options/windowresizemode",0);
        qvApp->getSettingsManager().loadSettings();
    }
    void cleanup() {
        if (window) {
            QThreadPool::globalInstance()->waitForDone(); QCoreApplication::processEvents();
            window->close(); QTRY_VERIFY(window.isNull());
        }
    }
    void validatesGlb() {
        const auto document = Model3D::inspectGlb(files.filePath("_2.GLB"));
        QVERIFY2(document.error.isEmpty(),qPrintable(document.error)); QCOMPARE(document.meshes,1);
        QVERIFY(Model3D::isGlb("/model.TEST.GlB")); QVERIFY(!Model3D::isGlb("x.glb.png"));
    }
    void corruptContainers_data() {
        QTest::addColumn<QByteArray>("bytes");
        QJsonObject json{{"asset",QJsonObject{{"version","2.0"}}},{"meshes",QJsonArray{QJsonObject()}}};
        const auto valid = GlbFixture::container(json);
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("wrong-magic") << QByteArray("not a model at all");
        QTest::newRow("truncated") << valid.chopped(1);
        auto future = valid; future[4] = 3; QTest::newRow("future-version") << future;
        auto length = valid; length[12] = char(255); QTest::newRow("invalid-chunk-size") << length;
        auto wrongType = valid; wrongType[16] = 0; QTest::newRow("missing-json") << wrongType;
        json["extensionsRequired"] = QJsonArray{"KHR_draco_mesh_compression"};
        QTest::newRow("unsupported-required-extension") << GlbFixture::container(json);
    }
    void corruptContainers() {
        QFETCH(QByteArray,bytes); const auto path = files.filePath("bad.glb");
        QVERIFY(GlbFixture::write(path,bytes)); QVERIFY(!Model3D::inspectGlb(path).error.isEmpty());
        QVERIFY(QFile::remove(path));
    }
    void optionalExtensionAndStaticAnimation() {
        const auto path = files.filePath("optional.glb");
        QVERIFY(GlbFixture::write(path,GlbFixture::container({{"asset",QJsonObject{{"version","2.0"}}},
            {"meshes",QJsonArray{QJsonObject()}},{"animations",QJsonArray{QJsonObject()}},
            {"extensionsUsed",QJsonArray{"VENDOR_optional"}}})));
        const auto document = Model3D::inspectGlb(path);
        QVERIFY(document.error.isEmpty()); QVERIFY(document.hasAnimations); QCOMPARE(document.warnings.size(),2);
        QVERIFY(QFile::remove(path));
    }
    void sharedNavigationAndCache() {
        QVGraphicsView view; auto &core = view.getImageCore();
        core.loadFile(files.filePath("_1.png")); QTRY_VERIFY(core.getCurrentFileDetails().isPixmapLoaded);
        QCOMPARE(core.getCurrentFileDetails().folderFileInfoList.size(),4);
        core.requestCachingFile(files.filePath("_2.GLB"),QColorSpace::SRgb);
        core.loadFile(files.filePath("_2.GLB"));
        QVERIFY(core.getCurrentFileDetails().isModelDocument); QVERIFY(!core.getCurrentFileDetails().isPixmapLoaded);
        QVERIFY(core.getSourceImage().isNull()); QVERIFY(core.getLoadedPixmap().isNull());
        QCOMPARE(core.getCurrentFileDetails().loadedIndexInFolder,1);
        core.updateFolderInfo(); QCOMPARE(core.getCurrentFileDetails().loadedIndexInFolder,1);
        core.loadFile(files.filePath("_3.png")); QTRY_VERIFY(core.getCurrentFileDetails().isPixmapLoaded);
        QVERIFY(!core.getCurrentFileDetails().isModelDocument);
    }
    void gpuControlsExportAndNavigation() {
        if (!qEnvironmentVariableIsSet("QVIEWSR_TEST_3D")) QSKIP("Set QVIEWSR_TEST_3D=1 on an OpenGL-capable desktop to exercise real rendering.");
        const auto modelPath = files.filePath("_2.GLB"); const auto originalHash = digest(modelPath);
        window = new MainWindow; window->resize(900,640); window->show(); window->openFile(modelPath);
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->activateWindow(); QVERIFY(QTest::qWaitForWindowActive(window));
        auto *model = window->findChild<Model3D::View *>(); QVERIFY(model);
        QTRY_VERIFY2_WITH_TIMEOUT(model->isReady(),qPrintable(model->message()),20000);
        QVERIFY((model->modelCenter()-QVector3D(123,-57,19)).length()<0.01f);
        QVERIFY(model->radius()>3.5f && model->radius()<4.0f);
        auto *controller = window->findChild<Sr::Controller *>(); QVERIFY(controller); QCOMPARE(controller->backendPid(),0);
        QVERIFY(!window->findChild<QAction *>("srRun")->isEnabled());
        QVERIFY(!window->findChild<QToolBar *>("srToolbar")->isVisible());
        QVERIFY(window->findChild<QToolBar *>("modelToolbar")->isVisible());
        QTRY_VERIFY(window->findChild<QLabel *>("modelStatus")->width() > 200);
        QTest::qWait(500); const auto initial = model->capture();
        QVERIFY(hasModelPixels(initial)); QCOMPARE(initial.size(),model->exportSize());
        const auto originalDistance = model->distance(); const auto originalFov = model->fieldOfView();
        QWheelEvent wheel(QPointF(200,200),model->mapToGlobal(QPoint(200,200)),{},QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
        QApplication::sendEvent(model,&wheel);
        QVERIFY(model->distance()<originalDistance); QCOMPARE(model->fieldOfView(),originalFov);
        QWheelEvent field(QPointF(200,200),model->mapToGlobal(QPoint(200,200)),{},QPoint(0,120),Qt::NoButton,Qt::ShiftModifier,Qt::NoScrollPhase,false);
        const auto distance = model->distance(); QApplication::sendEvent(model,&field);
        QCOMPARE(model->distance(),distance); QVERIFY(model->fieldOfView()<originalFov);
        QTest::mousePress(model,Qt::LeftButton,Qt::ControlModifier,QPoint(250,250));
        QMouseEvent move(QEvent::MouseMove,QPointF(350,290),model->mapToGlobal(QPoint(350,290)),Qt::NoButton,Qt::LeftButton,Qt::ControlModifier);
        QApplication::sendEvent(model,&move); QTest::mouseRelease(model,Qt::LeftButton,Qt::ControlModifier,QPoint(350,290));
        QVERIFY(!model->modelRotation().isIdentity()); QTest::qWait(100);
        const auto rotated = model->capture(); QVERIFY(rotated!=initial); QVERIFY(hasModelPixels(rotated));
        const auto beforePan = model->cameraPosition(); model->panCamera(QPointF(30,-10)); QVERIFY(model->cameraPosition()!=beforePan);
        QString error; const auto exported = files.filePath("export.png");
        QVERIFY2(model->savePng(exported,&error),qPrintable(error));
        QImage saved(exported); QCOMPARE(saved.size(),model->exportSize()); QVERIFY(saved.colorSpace().isValid());
        QCOMPARE(saved.colorSpace(),QColorSpace(QColorSpace::SRgb)); QVERIFY(hasModelPixels(saved));
        QVERIFY(!model->savePng(modelPath,&error)); QCOMPARE(digest(modelPath),originalHash);
        QVERIFY(QFile::remove(exported));
        QTimer::singleShot(150,window,[this,exported] {
            if (auto *dialog = window->findChild<QFileDialog *>()) {
                if (auto *edit = dialog->findChild<QLineEdit *>("fileNameEdit"))
                    edit->setText(QFileInfo(exported).completeBaseName());
                QMetaObject::invokeMethod(dialog,"accept",Qt::DirectConnection);
            }
        });
        QTimer::singleShot(3000,window,[] {
            if (auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) dialog->reject();
        });
        model->setFocus(); QTest::keyClick(window,Qt::Key_S,Qt::ControlModifier|Qt::ShiftModifier);
        QVERIFY(QFile::exists(exported)); QVERIFY(QFile::remove(exported));
        const auto captureDirectory = qEnvironmentVariable("QVIEWSR_TEST_CAPTURE_DIR");
        if (!captureDirectory.isEmpty()) {
            QVERIFY(QDir().mkpath(captureDirectory));
            QVERIFY(window->grab().save(QDir(captureDirectory).filePath("controls-window.png")));
        }
        window->resetZoom(); QVERIFY(model->modelRotation().isIdentity()); QCOMPARE(model->fieldOfView(),45.0f);
        window->resize(580,850); QTest::qWait(100);
        QVERIFY(hasModelPixels(model->capture())); QCOMPARE(model->capture().size(),model->exportSize());
        window->toggleFullScreen(); QTRY_VERIFY(window->isFullScreen()); QTest::qWait(200);
        QVERIFY(!window->findChild<QToolBar *>("modelToolbar")->isVisible());
        QVERIFY(!window->findChild<QToolBar *>("srToolbar")->isVisible());
        window->toggleFullScreen(); QTRY_VERIFY(!window->isFullScreen()); QTest::qWait(200);
        QVERIFY(window->findChild<QToolBar *>("modelToolbar")->isVisible());
        model->setFocus(); QTest::keyClick(window,Qt::Key_Right);
        QTRY_VERIFY(window->getCurrentFileDetails().isPixmapLoaded);
        QCOMPARE(window->getCurrentFileDetails().fileInfo.fileName(),QString("_3.png"));
        QVERIFY(!model->isVisible()); QVERIFY(model->modelSource().isEmpty());
        QVERIFY(window->findChild<QAction *>("srRun")->isEnabled()); QVERIFY(!window->findChild<QAction *>("modelSave")->isEnabled());
        QVERIFY(window->findChild<QToolBar *>("srToolbar")->isVisible());
        QTRY_VERIFY(controller->backendReady()); const auto backendPid = controller->backendPid(); QVERIFY(backendPid > 0);
        QTest::keyClick(window,Qt::Key_Left); QTRY_VERIFY(model->isReady());
        QCOMPARE(controller->backendPid(),backendPid);
        QCOMPARE(window->getCurrentFileDetails().fileInfo.fileName(),QString("_2.GLB"));
        window->reloadFile(); QTRY_VERIFY(model->isReady());
        const auto invalid = files.filePath("bad.glb"); QVERIFY(GlbFixture::write(invalid,"bad"));
        window->openFile(invalid); QVERIFY(!model->isReady()); QVERIFY(!window->findChild<QAction *>("modelSave")->isEnabled());
        QVERIFY(!model->message().isEmpty());
        window->openFile(modelPath); window->openFile(files.filePath("_1.png"));
        QTRY_VERIFY(window->getCurrentFileDetails().isPixmapLoaded); QTest::qWait(100);
        QVERIFY(model->modelSource().isEmpty());
        window->openFile(files.filePath("_4.gif")); QTRY_VERIFY(window->getCurrentFileDetails().isPixmapLoaded);
        QCOMPARE(digest(modelPath),originalHash); QVERIFY(QFile::remove(invalid));
    }
    void localModels() {
        const auto directory = qEnvironmentVariable("QVIEWSR_TEST_MODEL_DIR");
        if (directory.isEmpty() || !qEnvironmentVariableIsSet("QVIEWSR_TEST_3D"))
            QSKIP("Optional: QVIEWSR_TEST_MODEL_DIR points to local, read-only GLB fixtures.");
        const auto models = QDir(directory).entryInfoList({"*.glb","*.GLB"},QDir::Files,QDir::Name);
        QVERIFY(!models.isEmpty());
        const auto output = qEnvironmentVariable("QVIEWSR_TEST_CAPTURE_DIR");
        if (!output.isEmpty()) QVERIFY(QDir().mkpath(output));
        window = new MainWindow; window->resize(1024,768); window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        for (int i = 0; i < models.size(); ++i) {
            const auto path = models[i].absoluteFilePath(); const auto original = digest(path);
            window->openFile(path);
            auto *model = window->findChild<Model3D::View *>(); QVERIFY(model);
            QTRY_VERIFY2_WITH_TIMEOUT(model->isReady(),qPrintable(models[i].fileName()+": "+model->message()),30000);
            QTest::qWait(150);
            const auto frame = model->capture(); QVERIFY2(hasModelPixels(frame),qPrintable(models[i].fileName()));
            if (!output.isEmpty()) {
                QString error; QVERIFY2(model->savePng(QDir(output).filePath(QString("model-%1.png").arg(i,2,10,QChar('0'))),&error),qPrintable(error));
            }
            model->rotateModel(QPointF(80,20)); model->dolly(1); QTest::qWait(50);
            QVERIFY(hasModelPixels(model->capture()));
            QCOMPARE(digest(path),original);
            qInfo().noquote() << "Rendered local GLB" << (i+1) << "/" << models.size() << models[i].fileName();
        }
    }
    void staticAnimation() {
        if (!qEnvironmentVariableIsSet("QVIEWSR_TEST_3D")) QSKIP("Requires the real 3D renderer.");
        const auto path = files.filePath("animated.glb"); QVERIFY(GlbFixture::cube(path,true));
        window = new MainWindow; window->resize(800,600); window->show(); window->openFile(path);
        auto *model = window->findChild<Model3D::View *>(); QVERIFY(model);
        QTRY_VERIFY_WITH_TIMEOUT(model->isReady(),20000);
        QTest::qWait(300); const auto before = model->capture();
        QTest::qWait(450); const auto after = model->capture();
        QVERIFY(hasModelPixels(before)); QCOMPARE(after,before);
        QVERIFY(QFile::remove(path));
    }
};

int main(int argc,char **argv)
{
    QTemporaryDir settings; qputenv("XDG_CONFIG_HOME",settings.path().toUtf8());
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QCoreApplication::setOrganizationName("qViewSR-tests"); QCoreApplication::setApplicationName("glb-tests");
    QSettings config; config.setValue("firstlaunch",true); config.setValue("sr/worker",settings.filePath("no-worker")); config.sync();
    QVApplication app(argc,argv);
    if (qEnvironmentVariableIsSet("QVIEWSR_TEST_3D")) QSurfaceFormat::setDefaultFormat(QQuick3D::idealSurfaceFormat());
    GlbTests tests; return QTest::qExec(&tests,argc,argv);
}
#include "tst_glbtests.moc"
