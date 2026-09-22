// SPDX-License-Identifier: GPL-3.0-or-later
#include <QtTest>
#include <QColorSpace>
#include <QFile>
#include <QImageReader>
#include <QImageWriter>
#include <QElapsedTimer>
#include <QDialog>
#include <QMenu>
#include <QPushButton>
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
        connect(controller,&Sr::Controller::failed,this,[](const QString& message){qWarning().noquote()<<message;});
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
    void japaneseMenus() {
        QCOMPARE(QCoreApplication::translate("ActionManager","&File"),QStringLiteral("ファイル"));
        QCOMPARE(QCoreApplication::translate("QVOptionsDialog","Settings"),QStringLiteral("設定"));
        QVERIFY(QCoreApplication::translate("QPlatformTheme","Cancel")!=QStringLiteral("Cancel"));
        const auto actions=qvApp->getActionManager().getAllClonesOfAction("open",window);
        QVERIFY(!actions.isEmpty());
        QVERIFY(actions.first()->text().contains(QStringLiteral("開く")));
    }
    void singleLineAndFullscreen() {
        auto* toolbar=window->findChild<QToolBar*>("srToolbar");
        auto* label=window->findChild<QLabel*>("srStatus");
        const int height=toolbar->height();
        auto broken=config; broken.devices="long-error"; controller->setConfiguration(broken);
        QSignalSpy failure(controller,&Sr::Controller::failed);
        click("srRun"); QTRY_COMPARE_WITH_TIMEOUT(failure.count(),1,8000);
        QVERIFY(!label->wordWrap()); QVERIFY(!label->text().contains('\n'));
        QCOMPARE(toolbar->height(),height);
        const auto message=label->text(); QVERIFY(message.size()>1000);
        window->toggleFullScreen(); QTRY_VERIFY(window->isFullScreen());
        QVERIFY(!toolbar->isVisible());
        QTest::keyClick(window,Qt::Key_U,Qt::ControlModifier);
        QTRY_COMPARE_WITH_TIMEOUT(failure.count(),2,8000);
        window->toggleFullScreen(); QTRY_VERIFY(!window->isFullScreen());
        QVERIFY(toolbar->isVisible()); QCOMPARE(toolbar->height(),height);
    }
    void persistentWorkerReuse() {
        QTRY_VERIFY_WITH_TIMEOUT(controller->backendReady(),5000);
        const qint64 pid=controller->backendPid(); QVERIFY(pid>0);
        QSignalSpy initialized(controller,&Sr::Controller::backendInitialized);
        QSignalSpy ready(controller,&Sr::Controller::resultReady);
        click("srRun"); QTRY_COMPARE_WITH_TIMEOUT(ready.count(),1,8000);
        auto noise=config; noise.denoise=6; noise.halo=32; controller->setConfiguration(noise);
        click("srRun"); QTRY_COMPARE_WITH_TIMEOUT(ready.count(),2,8000);
        QCOMPARE(controller->backendPid(),pid); QCOMPARE(initialized.count(),0);
        noise.devices="different-device"; controller->setConfiguration(noise);
        QTRY_COMPARE_WITH_TIMEOUT(initialized.count(),1,5000);
        QVERIFY(controller->backendPid()!=pid);
    }
    void scanIncludesHeldDevices() {
        QTRY_VERIFY_WITH_TIMEOUT(controller->backendReady(),5000);
        bool included=false;
        QTimer::singleShot(0,window,[this,&included] {
            for(auto* widget:QApplication::topLevelWidgets()) {
                auto* dialog=qobject_cast<QDialog*>(widget);
                if(!dialog || dialog->windowTitle()!=QStringLiteral("qViewSR 設定")) continue;
                for(auto* button:dialog->findChildren<QPushButton*>()) {
                    if(button->text()!=QStringLiteral("接続デバイスを確認")) continue;
                    button->click();
                    auto* process=dialog->findChild<QProcess*>();
                    connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),dialog,[dialog,&included] {
                        const auto text=dialog->findChild<QLabel*>("srDeviceList")->text();
                        included=text.contains("FAKE") && text.contains("UNOWNED"); dialog->reject();
                    },Qt::QueuedConnection);
                    QTimer::singleShot(5000,dialog,&QDialog::reject);
                }
            }
        });
        controller->showSettings();
        QVERIFY(included);
    }
    void cancelRetainsWorker() {
        auto delayed=config; delayed.devices="delay"; controller->setConfiguration(delayed);
        QTRY_VERIFY_WITH_TIMEOUT(controller->backendReady(),5000);
        const auto pid=controller->backendPid();
        click("srRun");
        QTRY_VERIFY_WITH_TIMEOUT(controller->statusText().contains(QStringLiteral("準備済み")),5000);
        click("srCancel"); QTRY_VERIFY_WITH_TIMEOUT(!controller->isBusy(),3000);
        QVERIFY(controller->backendReady()); QCOMPARE(controller->backendPid(),pid);
        QSignalSpy ready(controller,&Sr::Controller::resultReady);
        click("srRun"); QTRY_COMPARE_WITH_TIMEOUT(ready.count(),1,8000);
        QCOMPARE(controller->backendPid(),pid);
    }
    void decoderFormats_data() {
        QTest::addColumn<QByteArray>("format");
        const auto available=QImageWriter::supportedImageFormats();
        for(const QByteArray format:{"webp","bmp","tiff","ppm","ico","xpm"})
            if(available.contains(format)) QTest::newRow(format.constData())<<format;
        QTest::newRow("svg")<<QByteArray("svg");
    }
    void decoderFormats() {
        QFETCH(QByteArray,format);
        const auto path=files.filePath("extra."+QString::fromLatin1(format));
        if(format=="svg") {
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"64\" height=\"48\"><rect width=\"64\" height=\"48\" fill=\"#7090b0\"/></svg>");
        } else {
            QImage source(64,48,QImage::Format_RGB32); source.fill(QColor(70,130,190));
            source.setColorSpace(QColorSpace::AdobeRgb); QVERIFY(source.save(path,format.constData()));
        }
        window->openFile(path);
        QTRY_COMPARE_WITH_TIMEOUT(view->getCurrentFileDetails().fileInfo.absoluteFilePath(),QFileInfo(path).absoluteFilePath(),5000);
        QTRY_VERIFY_WITH_TIMEOUT(window->findChild<QAction*>("srRun")->isEnabled(),5000);
        const auto size=view->getImageCore().getSourceImage().size();
        QSignalSpy ready(controller,&Sr::Controller::resultReady);
        click("srRun"); QTRY_COMPARE_WITH_TIMEOUT(ready.count(),1,12000);
        QCOMPARE(controller->resultImage().size(),size*4);
    }
    void animatedFrameAndCmyk() {
        const auto animated=files.filePath("animated.gif");
        QFile gif(animated); QVERIFY(gif.open(QIODevice::WriteOnly));
        gif.write(QByteArray::fromBase64("R0lGODlhIAAYAIEAAP8AAAAAAAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQAFAAAACwAAAAAIAAYAAAILwABCBxIsKDBgwgTKlzIsKHDhxAjSpxIsaLFixgzatzIsaPHjyBDihxJsqTJjQEBACH5BAEUAAEALAAAAAAgABgAgQAA/wAAAAAAAAAAAAgvAAEIHEiwoMGDCBMqXMiwocOHECNKnEixosWLGDNq3Mixo8ePIEOKHEmypMmNAQEAOw==")); gif.close();
        window->openFile(animated);
        QTRY_VERIFY_WITH_TIMEOUT(view->getCurrentFileDetails().isMovieLoaded,5000);
        QTRY_COMPARE_WITH_TIMEOUT(view->getLoadedMovie().currentFrameNumber(),1,5000);
        window->pause();
        QSignalSpy ready(controller,&Sr::Controller::resultReady);
        click("srRun"); QTRY_COMPARE_WITH_TIMEOUT(ready.count(),1,8000);
        QCOMPARE(controller->resultImage().size(),QSize(128,96));
        view->zoom(1.25); QTest::qWait(100);
        QCOMPARE(view->getLoadedPixmap().size(),QSize(128,96));
        click("srToggle");
        QCOMPARE(view->getImageCore().getSourceImage().pixelColor(5,5),QColor(Qt::blue));
        window->pause();
        QVERIFY(view->getCurrentFileDetails().isMovieLoaded); QVERIFY(!controller->hasResult());
        const auto cmyk=files.filePath("cmyk.jpg");
        QFile jpeg(cmyk); QVERIFY(jpeg.open(QIODevice::WriteOnly));
        jpeg.write(QByteArray::fromBase64("/9j/7gAOQWRvYmUAZAAAAAAA/9sAQwAIBgYHBgUIBwcHCQkICgwUDQwLCwwZEhMPFB0aHx4dGhwcICQuJyAiLCMcHCg3KSwwMTQ0NB8nOT04MjwuMzQy/8AAFAgAGAAgBEMRAE0RAFkRAEsRAP/EAB8AAAEFAQEBAQEBAAAAAAAAAAABAgMEBQYHCAkKC//EALUQAAIBAwMCBAMFBQQEAAABfQECAwAEEQUSITFBBhNRYQcicRQygZGhCCNCscEVUtHwJDNicoIJChYXGBkaJSYnKCkqNDU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6g4SFhoeIiYqSk5SVlpeYmZqio6Slpqeoqaqys7S1tre4ubrCw8TFxsfIycrS09TV1tfY2drh4uPk5ebn6Onq8fLz9PX29/j5+v/aAA4EQwBNAFkASwAAPwD2uuvr3+vdaKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK//Z")); jpeg.close();
        window->openFile(cmyk);
        QTRY_COMPARE_WITH_TIMEOUT(view->getCurrentFileDetails().fileInfo.absoluteFilePath(),QFileInfo(cmyk).absoluteFilePath(),5000);
        QTRY_VERIFY_WITH_TIMEOUT(window->findChild<QAction*>("srRun")->isEnabled(),5000);
        QVERIFY(view->getImageCore().getSourceProfile().assumedSrgb);
        click("srRun"); QTRY_COMPARE_WITH_TIMEOUT(ready.count(),2,8000);
        QCOMPARE(controller->resultImage().size(),QSize(128,96));
    }
    void realHardwareGui() {
        if(!qEnvironmentVariableIsSet("QVIEWSR_REAL_WORKER")) QSKIP("Set QVIEWSR_REAL_WORKER/RUNTIME/MODEL/IMAGE for opt-in real-device GUI test.");
        auto real=config;
        real.workerPath=qEnvironmentVariable("QVIEWSR_REAL_WORKER"); real.runtimeRoot=qEnvironmentVariable("QVIEWSR_REAL_RUNTIME");
        real.modelPath=qEnvironmentVariable("QVIEWSR_REAL_MODEL"); real.devices="all";
        QSignalSpy initialized(controller,&Sr::Controller::backendInitialized),failure(controller,&Sr::Controller::failed);
        QElapsedTimer initialization; initialization.start();
        controller->setConfiguration(real);
        QTRY_VERIFY_WITH_TIMEOUT(controller->backendReady() || !failure.isEmpty(),120000);
        QVERIFY2(failure.isEmpty(),qPrintable(controller->statusText()));
        const auto initializationMs=initialization.elapsed();
        const auto pid=controller->backendPid();
        QCOMPARE(initialized.count(),1);
        const auto path=qEnvironmentVariable("QVIEWSR_REAL_IMAGE");
        window->openFile(path);
        QTRY_COMPARE_WITH_TIMEOUT(view->getCurrentFileDetails().fileInfo.absoluteFilePath(),QFileInfo(path).absoluteFilePath(),5000);
        QTRY_VERIFY_WITH_TIMEOUT(window->findChild<QAction*>("srRun")->isEnabled(),5000);
        const auto size=view->getImageCore().getSourceImage().size();
        QSignalSpy ready(controller,&Sr::Controller::resultReady);
        click("srRun");
        QTRY_VERIFY_WITH_TIMEOUT(ready.count()==1 || failure.count()>0,120000);
        QVERIFY2(failure.isEmpty(),qPrintable(controller->statusText()));
        QVERIFY(controller->showingSr()); QCOMPARE(controller->resultImage().size(),QSize(size.width()*4,size.height()*4));
        click("srToggle"); QVERIFY(!controller->showingSr());
        click("srToggle"); QVERIFY(controller->showingSr());
        const auto firstResult=controller->resultImage();
        const auto firstSummary=window->findChild<QLabel*>("srStatus")->toolTip();
        QCOMPARE(firstSummary.count(QStringLiteral("タイル")),4);
        real.denoise=6; controller->setConfiguration(real);
        click("srRun");
        QTRY_VERIFY_WITH_TIMEOUT(ready.count()==2 || failure.count()>0,120000);
        QVERIFY2(failure.isEmpty(),qPrintable(controller->statusText()));
        QCOMPARE(initialized.count(),1); QCOMPARE(controller->backendPid(),pid);
        QVERIFY(controller->resultImage()!=firstResult);
        const auto secondSummary=window->findChild<QLabel*>("srStatus")->toolTip();
        click("srRun");
        QTRY_VERIFY_WITH_TIMEOUT(controller->statusText().startsWith(QStringLiteral("処理中 ")),30000);
        controller->cancel(); QTRY_VERIFY_WITH_TIMEOUT(!controller->isBusy(),15000);
        QCOMPARE(controller->backendPid(),pid); QCOMPARE(initialized.count(),1);
        real.devices="ncs2"; controller->setConfiguration(real);
        QTRY_VERIFY_WITH_TIMEOUT(initialized.count()==2 || !failure.isEmpty(),120000);
        QVERIFY2(failure.isEmpty(),qPrintable(controller->statusText()));
        QVERIFY(controller->backendPid()!=pid);
        click("srRun"); QTRY_VERIFY_WITH_TIMEOUT(ready.count()==3 || failure.count()>0,120000);
        QVERIFY2(failure.isEmpty(),qPrintable(controller->statusText()));
        QCOMPARE(initialized.count(),2);
        QCOMPARE(window->findChild<QLabel*>("srStatus")->toolTip().count(QStringLiteral("タイル")),2);
        const auto artifacts=qEnvironmentVariable("QVIEWSR_GUI_ARTIFACTS");
        if(!artifacts.isEmpty()) {
            QDir().mkpath(artifacts);
            QString error;
            QVERIFY2(controller->saveResult(artifacts+"/sr-result.png","png",&error),qPrintable(error));
            QVERIFY2(controller->saveResult(artifacts+"/sr-result.jpg","jpeg",&error),qPrintable(error));
            window->resize(1200,850); QTest::qWait(200);
            QVERIFY(window->grab().save(artifacts+"/gui-sr.png"));
            QFile summary(artifacts+"/device-summary.txt"); QVERIFY(summary.open(QIODevice::WriteOnly));
            summary.write(QStringLiteral("初期化: %1 ms / 全4本PID: %2 / 初期化回数: %3（NCS2へ切替後）\n全4本の連続2処理と中止後は初期化1回・同一PID\n1枚目:\n%4\n2枚目:\n%5\n切替後:\n%6\n")
                .arg(initializationMs).arg(pid).arg(initialized.count()).arg(firstSummary,secondSummary,window->findChild<QLabel*>("srStatus")->toolTip()).toUtf8());
            window->toggleFullScreen(); QTest::qWait(200);
            QVERIFY(!window->findChild<QToolBar*>("srToolbar")->isVisible());
            QVERIFY(window->grab().save(artifacts+"/gui-fullscreen.png"));
            window->toggleFullScreen();
            QTimer::singleShot(300,window,[this,artifacts] {
                for(auto* widget:QApplication::topLevelWidgets()) {
                    auto* dialog=qobject_cast<QDialog*>(widget);
                    if(dialog && dialog->windowTitle()==QStringLiteral("qViewSR 設定")) {
                        dialog->grab().save(artifacts+"/gui-settings.png"); dialog->reject();
                    }
                }
            });
            controller->showSettings();
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
