// SPDX-License-Identifier: GPL-3.0-or-later
#include <QtTest>
#include <QColorSpace>
#include <QFile>
#include <QImageReader>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolBar>
#include <QToolButton>
#include <QLabel>
#include "qvapplication.h"
#include "sr/color_pipeline.h"
#include "sr/sr_controller.h"

class SrTests : public QObject {
    Q_OBJECT
    QTemporaryDir files;
    QString sourcePath;
    MainWindow* window=nullptr;
    QVGraphicsView* view=nullptr;
    Sr::Controller* controller=nullptr;
    Sr::Configuration config;

    void click(const char* name) {
        auto* action=window->findChild<QAction*>(name);
        QVERIFY(action); QVERIFY(action->isEnabled());
        auto* toolbar=window->findChild<QToolBar*>("srToolbar");
        auto* button=qobject_cast<QToolButton*>(toolbar->widgetForAction(action));
        QVERIFY(button); QTest::mouseClick(button,Qt::LeftButton);
    }
private slots:
    void initTestCase() {
        QVERIFY(files.isValid());
        sourcePath=files.filePath("source.png");
        QImage source(64,48,QImage::Format_RGBA8888);
        source.fill(QColor(70,130,190,180)); source.setColorSpace(QColorSpace::AdobeRgb);
        QVERIFY(source.save(sourcePath));
        config.workerPath=QStringLiteral(SR_SOURCE_DIR)+"/tests/fake_worker.py";
        config.runtimeRoot=files.filePath("runtime");
        QDir().mkpath(config.runtimeRoot+"/deployment_tools/inference_engine/lib/intel64");
        QFile marker(config.runtimeRoot+"/deployment_tools/inference_engine/lib/intel64/libmyriadPlugin.so");
        QVERIFY(marker.open(QIODevice::WriteOnly)); marker.close();
        config.modelPath=files.filePath("model.xml");
        QFile model(config.modelPath); QVERIFY(model.open(QIODevice::WriteOnly)); model.write("test"); model.close();
        config.displayProfile="sRGB"; config.devices="normal";
    }
    void init() {
        window=new MainWindow; window->resize(1000,650); window->show();
        view=window->findChild<QVGraphicsView*>(); QVERIFY(view);
        controller=window->findChild<Sr::Controller*>(); QVERIFY(controller);
        controller->setConfiguration(config);
        window->openFile(sourcePath);
        QTRY_COMPARE_WITH_TIMEOUT(view->getImageCore().getSourceImage().size(),QSize(64,48),5000);
        QTest::qWait(100);
    }
    void cleanup() {
        if(!window) return;
        QPointer<MainWindow> guard=window;
        window->close();
        QTRY_VERIFY_WITH_TIMEOUT(guard.isNull(),5000);
        window=nullptr;
    }
    void profilesAndAlpha() {
        const auto raw=view->getImageCore().getSourceImage();
        const auto profile=view->getImageCore().getSourceProfile();
        QVERIFY2(profile.error.isEmpty(),qPrintable(profile.error));
        QVERIFY(!profile.icc.isEmpty());
        QString error;
        auto actual=Sr::toSrgb(raw,profile,&error);
        QVERIFY2(!actual.isNull(),qPrintable(error));
        auto reference=raw.convertedToColorSpace(QColorSpace::SRgb);
        const auto a=actual.pixelColor(10,10),b=reference.pixelColor(10,10);
        QVERIFY(qAbs(a.red()-b.red())<=2); QVERIFY(qAbs(a.green()-b.green())<=2); QVERIFY(qAbs(a.blue()-b.blue())<=2);
        QCOMPARE(a.alpha(),raw.pixelColor(10,10).alpha());
        auto invalid=profile; invalid.icc="invalid ICC";
        QVERIFY(Sr::toSrgb(raw,invalid,&error).isNull());
        QVERIFY(!error.isEmpty());
    }
    void runToggleExport() {
        view->originalSize(); view->zoom(1.5);
        const qreal visibleWidth=view->transform().m11()*view->getLoadedPixmap().width();
        const auto beforeColor=view->getImageCore().getSourceImage().pixelColor(10,10);
        QSignalSpy ready(controller,&Sr::Controller::resultReady);
        click("srRun");
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(),1,8000);
        QVERIFY(!controller->isBusy()); QVERIFY(controller->showingSr());
        QCOMPARE(controller->resultImage().size(),QSize(256,192));
        QCOMPARE(controller->resultImage().pixelColor(50,50).alpha(),180);
        QVERIFY(qAbs(view->transform().m11()*view->getLoadedPixmap().width()-visibleWidth)<1);
        QCOMPARE(view->getImageCore().getSourceImage().pixelColor(10,10),beforeColor);
        click("srToggle"); QVERIFY(!controller->showingSr()); QCOMPARE(view->getLoadedPixmap().size(),QSize(64,48));
        QVERIFY(qAbs(view->transform().m11()*view->getLoadedPixmap().width()-visibleWidth)<1);
        click("srToggle");
        QString error;
        QVERIFY2(controller->saveResult(files.filePath("result.png"),"png",&error),qPrintable(error));
        QVERIFY2(controller->saveResult(files.filePath("result.jpg"),"jpeg",&error),qPrintable(error));
        for(const auto& path:{files.filePath("result.png"),files.filePath("result.jpg")}) {
            QImageReader reader(path); auto image=reader.read(); QCOMPARE(image.size(),QSize(256,192));
            QCOMPARE(image.colorSpace(),QColorSpace(QColorSpace::SRgb));
        }
        QVERIFY(!controller->saveResult(sourcePath,"png",&error));
        const auto bytesBefore=QImage(files.filePath("result.png"));
        QFile monitor(files.filePath("monitor.icc")); QVERIFY(monitor.open(QIODevice::WriteOnly));
        monitor.write(QColorSpace(QColorSpace::AdobeRgb).iccProfile()); monitor.close();
        auto alternate=config; alternate.displayProfile=monitor.fileName(); controller->setConfiguration(alternate);
        QVERIFY(view->getLoadedPixmap().toImage().pixelColor(50,50)!=controller->resultImage().pixelColor(50,50));
        QVERIFY(controller->saveResult(files.filePath("other-monitor.png"),"png",&error));
        QCOMPARE(QImage(files.filePath("other-monitor.png")),bytesBefore);
        view->rotateImage(90);
        QVERIFY(controller->saveResult(files.filePath("rotated.png"),"png",&error));
        QCOMPARE(QImage(files.filePath("rotated.png")).size(),QSize(192,256));
    }
    void cancelOnNavigation() {
        auto delayed=config; delayed.devices="delay"; controller->setConfiguration(delayed);
        click("srRun"); QVERIFY(controller->isBusy());
        QTRY_VERIFY_WITH_TIMEOUT(controller->findChild<QProcess*>() &&
            controller->findChild<QProcess*>()->state()==QProcess::Running,5000);
        QImage second(83,39,QImage::Format_RGB32); second.fill(Qt::red); second.setColorSpace(QColorSpace::SRgb);
        const auto path=files.filePath("second.png"); QVERIFY(second.save(path));
        window->openFile(path);
        QTRY_COMPARE_WITH_TIMEOUT(view->getImageCore().getSourceImage().size(),QSize(83,39),5000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller->isBusy(),5000);
        QTest::qWait(300);
        QVERIFY(!controller->hasResult()); QCOMPARE(view->getLoadedPixmap().size(),QSize(83,39));
    }
    void corruptResultRejected() {
        auto broken=config; broken.devices="bad-hash"; controller->setConfiguration(broken);
        QSignalSpy failure(controller,&Sr::Controller::failed);
        click("srRun"); QTRY_COMPARE_WITH_TIMEOUT(failure.count(),1,8000);
        QVERIFY(!controller->hasResult()); QVERIFY(!controller->isBusy());
        QCOMPARE(view->getLoadedPixmap().size(),QSize(64,48));
    }
    void workerCrashRetainsOriginal() {
        auto broken=config; broken.devices="crash"; controller->setConfiguration(broken);
        QSignalSpy failure(controller,&Sr::Controller::failed);
        click("srRun"); QTRY_COMPARE_WITH_TIMEOUT(failure.count(),1,8000);
        QVERIFY(!controller->hasResult()); QVERIFY(!controller->isBusy());
        QCOMPARE(view->getLoadedPixmap().size(),QSize(64,48));
    }
    void closeWhileWorkerRuns() {
        auto delayed=config; delayed.devices="delay"; controller->setConfiguration(delayed);
        click("srRun");
        QTRY_VERIFY_WITH_TIMEOUT(controller->findChild<QProcess*>() &&
            controller->findChild<QProcess*>()->state()==QProcess::Running,5000);
        QPointer<QProcess> process=controller->findChild<QProcess*>();
        QPointer<MainWindow> guard=window;
        window->close();
        QTRY_VERIFY_WITH_TIMEOUT(guard.isNull(),5000);
        QVERIFY(process.isNull());
        window=nullptr;
    }
    void realHardwareGui() {
        if(!qEnvironmentVariableIsSet("QVIEWSR_REAL_WORKER")) QSKIP("Set QVIEWSR_REAL_WORKER/RUNTIME/MODEL/IMAGE for opt-in real-device GUI test.");
        auto real=config;
        real.workerPath=qEnvironmentVariable("QVIEWSR_REAL_WORKER"); real.runtimeRoot=qEnvironmentVariable("QVIEWSR_REAL_RUNTIME");
        real.modelPath=qEnvironmentVariable("QVIEWSR_REAL_MODEL"); real.devices="all";
        controller->setConfiguration(real);
        const auto path=qEnvironmentVariable("QVIEWSR_REAL_IMAGE");
        window->openFile(path);
        QTRY_COMPARE_WITH_TIMEOUT(view->getCurrentFileDetails().fileInfo.absoluteFilePath(),QFileInfo(path).absoluteFilePath(),5000);
        QTRY_VERIFY_WITH_TIMEOUT(window->findChild<QAction*>("srRun")->isEnabled(),5000);
        const auto size=view->getImageCore().getSourceImage().size();
        QSignalSpy ready(controller,&Sr::Controller::resultReady),failure(controller,&Sr::Controller::failed);
        click("srRun");
        QTRY_VERIFY_WITH_TIMEOUT(ready.count()==1 || failure.count()>0,120000);
        QVERIFY2(failure.isEmpty(),qPrintable(controller->statusText()));
        QVERIFY(controller->showingSr()); QCOMPARE(controller->resultImage().size(),QSize(size.width()*4,size.height()*4));
        click("srToggle"); QVERIFY(!controller->showingSr());
        click("srToggle"); QVERIFY(controller->showingSr());
        const auto artifacts=qEnvironmentVariable("QVIEWSR_GUI_ARTIFACTS");
        if(!artifacts.isEmpty()) {
            QDir().mkpath(artifacts);
            QString error;
            QVERIFY2(controller->saveResult(artifacts+"/sr-result.png","png",&error),qPrintable(error));
            QVERIFY2(controller->saveResult(artifacts+"/sr-result.jpg","jpeg",&error),qPrintable(error));
            window->resize(1200,850); QTest::qWait(200);
            QVERIFY(window->grab().save(artifacts+"/gui-sr.png"));
            QFile summary(artifacts+"/device-summary.txt"); QVERIFY(summary.open(QIODevice::WriteOnly));
            summary.write(window->findChild<QLabel*>("srStatus")->toolTip().toUtf8());
        }
    }
};

int main(int argc,char** argv) {
    QTemporaryDir settings;
    qputenv("XDG_CONFIG_HOME",settings.path().toUtf8());
    QCoreApplication::setOrganizationName("qViewSR-tests");
    QCoreApplication::setApplicationName("qViewSR-tests");
    QSettings s; s.setValue("firstlaunch",true); s.setValue("updatenotifications",false); s.sync();
    QVApplication app(argc,argv);
    SrTests tests;
    return QTest::qExec(&tests,argc,argv);
}
#include "tst_srtests.moc"
