// SPDX-License-Identifier: GPL-3.0-or-later
#include <QtTest>
#include <QColorSpace>
#include <QFile>
#include <QImageReader>
#include <QImageWriter>
#include <QElapsedTimer>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolBar>
#include <QToolButton>
#include <QLabel>
#include <QScrollBar>
#include <QFileDialog>
#include <QStandardPaths>
#include "sr/gif_export.h"
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
    // Locally generated GIF: moving opaque blocks on transparency; disposal=2.
    QString gifFixture(bool looping=true) {
        auto data=QByteArray::fromBase64("R0lGODlhIAAYAIEAAAAAAP8AAAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQJCAAAACwAAAAAIAAYAAAITgABCBxIsKDBgwgTKlzIsKHDhwgDSJwoEWJEihMtHsSYUWNBjhU9EgQZQORIkCYHkkwpcCVLlylhmpQpkqZHmxpxWtQJkSfLn0CDCtUYEAAh+QQJEAAAACwAAAAAIAAYAIEAAAAA/wAAAAAAAAAITwABCBxIsKDBgwgTKlzIsKHDhxAFBphIcWLEgxUrXjSYkeLGgh0tfhwYMsBIkiFPSkypsqRKAC5bsjwZk+bMkTVx3vyYk+fOl0CDCh16MCAAIfkECRgAAAAsAAAAACAAGACBAAAAAAD/AAAAAAAACE4AAQgcSLCgwYMIEypcyLChw4cQGQaYSHFixIMVK140mJHixoIdLX4cGDLASJIhTwosqRIAS5UvT8YcOfNjzY03L+aMuBNiz5ZAgwodGRAAOw==");
        if(!looping) data.remove(data.indexOf("NETSCAPE2.0")-3,19);
        const auto path=files.filePath(looping?"animated.gif":"once.gif");
        QFile file(path);
        if(!file.open(QIODevice::WriteOnly) || file.write(data)!=data.size()) return {};
        return path;
    }
    void loadAnimation(const QString& path, const QString& mode="animation") {
        QFile::remove(files.filePath("model.frames.jsonl"));
        auto animationConfig=config; animationConfig.devices=mode; animationConfig.scale=2; animationConfig.denoise=3;
        controller->setConfiguration(animationConfig);
        window->openFile(path);
        QTRY_VERIFY_WITH_TIMEOUT(view->getCurrentFileDetails().isMovieLoaded,5000);
        QTRY_VERIFY_WITH_TIMEOUT(controller->backendReady(),5000);
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
        QSettings().setValue("options/scalingenabled",true);
        qvApp->getSettingsManager().loadSettings();
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
    void dragDisplayedResult() {
        QString error;
        std::unique_ptr<QMimeData> original(view->getFileDragMimeData(&error));
        QCOMPARE(original->urls(),QList<QUrl>{QUrl::fromLocalFile(sourcePath)});
        QCOMPARE(original->formats(),QStringList{"text/uri-list"}); QVERIFY(!original->hasImage());
        QSignalSpy ready(controller,&Sr::Controller::resultReady); click("srRun");
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(),1,8000);
        QFile monitor(files.filePath("drag-monitor.icc")); QVERIFY(monitor.open(QIODevice::WriteOnly));
        monitor.write(QColorSpace(QColorSpace::AdobeRgb).iccProfile()); monitor.close();
        auto alternate = config; alternate.displayProfile = monitor.fileName(); controller->setConfiguration(alternate);
        view->originalSize(); view->zoom(2.5);
        std::unique_ptr<QMimeData> sr(view->getFileDragMimeData(&error));
        QVERIFY2(error.isEmpty(),qPrintable(error)); QCOMPARE(sr->urls().size(),1); QVERIFY(!sr->hasImage());
        const auto path = sr->urls().first().toLocalFile(); QVERIFY(path != sourcePath);
        const QImage snapshot(path);
        QCOMPARE(snapshot,controller->resultImage().convertToFormat(snapshot.format())); QCOMPARE(snapshot.colorSpace(),QColorSpace(QColorSpace::SRgb));
        QVERIFY(snapshot.pixelColor(50,50)!=view->getLoadedPixmap().toImage().pixelColor(50,50));
        std::unique_ptr<QMimeData> repeated(view->getFileDragMimeData(&error)); QCOMPARE(repeated->urls(),sr->urls());
        view->rotateImage(90);
        std::unique_ptr<QMimeData> rotated(view->getFileDragMimeData(&error));
        QCOMPARE(QImage(rotated->urls().first().toLocalFile()).size(),QSize(192,256));
        QCOMPARE(QImage(path),snapshot); // A later drag cannot overwrite an in-flight upload.
        click("srToggle");
        std::unique_ptr<QMimeData> restored(view->getFileDragMimeData(&error)); QCOMPARE(restored->urls(),original->urls());
        cleanup(); // The receiving browser can read the file after this window has closed.
        QCOMPARE(QImage(path),snapshot);
    }
    void dragCurrentAnimationFrame() {
        const auto gif = gifFixture(); loadAnimation(gif);
        QSignalSpy ready(controller,&Sr::Controller::resultReady); click("srRun");
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(),1,8000); controller->setAnimationPaused(true);
        QString error;
        std::unique_ptr<QMimeData> first(view->getFileDragMimeData(&error)); QVERIFY2(error.isEmpty(),qPrintable(error));
        const auto firstPath = first->urls().first().toLocalFile(); const auto snapshot = controller->resultImage().convertToFormat(QImage::Format_ARGB32);
        QCOMPARE(QImage(firstPath),snapshot);
        window->mirror();
        std::unique_ptr<QMimeData> mirrored(view->getFileDragMimeData(&error));
        QCOMPARE(QImage(mirrored->urls().first().toLocalFile()),snapshot.mirrored(true,false));
        window->flip();
        std::unique_ptr<QMimeData> flipped(view->getFileDragMimeData(&error));
        QCOMPARE(QImage(flipped->urls().first().toLocalFile()),snapshot.mirrored(true,true));
        window->mirror(); window->flip();
        controller->stepAnimation();
        std::unique_ptr<QMimeData> next(view->getFileDragMimeData(&error));
        QVERIFY(next->urls()!=first->urls()); QCOMPARE(QImage(next->urls().first().toLocalFile()),controller->resultImage().convertToFormat(QImage::Format_ARGB32));
        QCOMPARE(QImage(firstPath),snapshot);
        controller->toggle();
        std::unique_ptr<QMimeData> original(view->getFileDragMimeData(&error));
        QCOMPARE(original->urls(),QList<QUrl>{QUrl::fromLocalFile(gif)});
    }
    void dragExportFailureAndExpiry() {
        QSignalSpy ready(controller,&Sr::Controller::resultReady); click("srRun");
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(),1,8000);
        const auto cacheRoot = qgetenv("XDG_CACHE_HOME");
        const auto blocker = files.filePath("blocked-cache");
        { QFile file(blocker); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("not a directory"); }
        qputenv("XDG_CACHE_HOME",blocker.toUtf8());
        QString error; std::unique_ptr<QMimeData> failed(view->getFileDragMimeData(&error));
        qputenv("XDG_CACHE_HOME",cacheRoot);
        QVERIFY(!failed->hasUrls()); QVERIFY(!error.isEmpty()); QVERIFY(controller->showingSr());
        const QDir cache(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)+"/drag-exports");
        QVERIFY(QDir().mkpath(cache.absolutePath()));
        for (const auto &name : {"qviewsr-sr-expired.png","keep.txt"}) {
            QFile file(cache.filePath(name)); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("old");
            QVERIFY(file.flush());
            QVERIFY(file.setFileTime(QDateTime::currentDateTimeUtc().addDays(-2),QFileDevice::FileModificationTime));
        }
        std::unique_ptr<QMimeData> valid(view->getFileDragMimeData(&error));
        QVERIFY2(error.isEmpty(),qPrintable(error)); QCOMPARE(valid->urls().size(),1);
        QVERIFY(!QFileInfo::exists(cache.filePath("qviewsr-sr-expired.png"))); QVERIFY(QFileInfo::exists(cache.filePath("keep.txt")));
        view->setDragImageProvider([] { return std::optional<QImage>(QImage()); });
        std::unique_ptr<QMimeData> missing(view->getFileDragMimeData(&error));
        QVERIFY(!missing->hasUrls()); QVERIFY(!error.isEmpty()); // Never silently fall back to the original.
        view->closeImage(); std::unique_ptr<QMimeData> empty(view->getFileDragMimeData(&error)); QVERIFY(!empty->hasUrls());
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
    void selectableScale_data() {
        QTest::addColumn<double>("scale");
        QTest::newRow("2x")<<2.0; QTest::newRow("3x")<<3.0;
        QTest::newRow("1.5x")<<1.5; QTest::newRow("2.25x")<<2.25;
    }
    void selectableScale() {
        QFETCH(double,scale);
        QImage source(63,47,QImage::Format_RGBA8888); source.fill(QColor(70,130,190,180));
        source.setColorSpace(QColorSpace::AdobeRgb);
        const auto path=files.filePath("odd-size.png"); QVERIFY(source.save(path));
        window->openFile(path);
        QTRY_COMPARE(view->getImageCore().getSourceImage().size(),source.size());
        auto* scaleInput=window->findChild<QDoubleSpinBox*>("srScale"); QVERIFY(scaleInput);
        scaleInput->setValue(scale); QCOMPARE(controller->configuration().scale,scale);
        QCOMPARE(Sr::Configuration::load().scale,scale);
        QSignalSpy ready(controller,&Sr::Controller::resultReady);
        click("srRun"); QTRY_COMPARE_WITH_TIMEOUT(ready.count(),1,8000);
        const QSize expected(qRound(63*scale),qRound(47*scale));
        QCOMPARE(controller->resultImage().size(),expected);
        QCOMPARE(controller->resultImage().colorSpace(),QColorSpace(QColorSpace::SRgb));
        QVERIFY(qAbs(controller->resultImage().pixelColor(10,10).alpha()-180)<=1);
        QCOMPARE(controller->resultScale(),scale); QCOMPARE(controller->resultPasses(),1);
        QString error; const auto saved=files.filePath("scaled.png");
        QVERIFY2(controller->saveResult(saved,"png",&error),qPrintable(error));
        QCOMPARE(QImage(saved).size(),expected);
    }
    void repeatFromResultKeepsOriginal() {
        QTRY_VERIFY_WITH_TIMEOUT(controller->backendReady(),5000);
        const auto pid=controller->backendPid();
        const auto original=view->getImageCore().getSourceImage();
        auto* scale=window->findChild<QDoubleSpinBox*>("srScale");
        scale->setValue(2);
        QSignalSpy ready(controller,&Sr::Controller::resultReady);
        click("srRun"); QTRY_COMPARE_WITH_TIMEOUT(ready.count(),1,8000);
        const auto first=controller->resultImage();
        QCOMPARE(first.size(),QSize(128,96));
        QFile monitor(files.filePath("repeat-monitor.icc")); QVERIFY(monitor.open(QIODevice::WriteOnly));
        monitor.write(QColorSpace(QColorSpace::AdobeRgb).iccProfile()); monitor.close();
        auto next=controller->configuration(); next.displayProfile=monitor.fileName(); next.scale=3;
        controller->setConfiguration(next);
        // A repeat explicitly uses the prior sRGB result, even when the user is
        // comparing the original or using a different monitor colour profile.
        click("srToggle"); QVERIFY(!controller->showingSr());
        click("srRepeat"); QTRY_COMPARE_WITH_TIMEOUT(ready.count(),2,8000);
        QCOMPARE(controller->resultImage().size(),QSize(384,288));
        QCOMPARE(controller->resultScale(),6.0); QCOMPARE(controller->resultPasses(),2);
        const QImage input(files.filePath("model.last-input.png"));
        QCOMPARE(input.size(),first.size());
        const auto color=input.pixelColor(10,10),before=first.pixelColor(10,10);
        QCOMPARE(color.red(),before.red()); QCOMPARE(color.green(),before.green()); QCOMPARE(color.blue(),before.blue());
        QVERIFY(qAbs(controller->resultImage().pixelColor(10,10).alpha()-180)<=1);
        QCOMPARE(view->getImageCore().getSourceImage(),original);
        click("srToggle"); QCOMPARE(view->getLoadedPixmap().size(),original.size());
        QString error; QVERIFY(controller->saveResult(files.filePath("repeat.png"),"png",&error));
        QCOMPARE(QImage(files.filePath("repeat.png")).size(),QSize(384,288));
        QCOMPARE(controller->backendPid(),pid);
        click("srRun"); QTRY_COMPARE_WITH_TIMEOUT(ready.count(),3,8000);
        QCOMPARE(controller->resultImage().size(),QSize(192,144));
        QCOMPARE(controller->resultPasses(),1); QCOMPARE(controller->resultScale(),3.0);
    }
    void failedOrCancelledRepeatKeepsResult() {
        QSignalSpy ready(controller,&Sr::Controller::resultReady);
        click("srRun"); QTRY_COMPARE_WITH_TIMEOUT(ready.count(),1,8000);
        const auto previous=controller->resultImage();
        auto broken=controller->configuration(); broken.devices="bad-hash";
        controller->setConfiguration(broken);
        QSignalSpy failure(controller,&Sr::Controller::failed);
        click("srRepeat"); QTRY_COMPARE_WITH_TIMEOUT(failure.count(),1,8000);
        QCOMPARE(controller->resultImage(),previous); QVERIFY(controller->showingSr());
        QCOMPARE(controller->resultScale(),4.0); QCOMPARE(controller->resultPasses(),1);
        broken.devices="delay"; controller->setConfiguration(broken);
        click("srRepeat");
        QTRY_VERIFY_WITH_TIMEOUT(controller->statusText().contains(QStringLiteral("準備済み")),5000);
        click("srCancel"); QTRY_VERIFY_WITH_TIMEOUT(!controller->isBusy(),3000);
        QCOMPARE(controller->resultImage(),previous); QCOMPARE(controller->resultPasses(),1);
        QVERIFY(controller->showingSr());
        click("srRepeat");
        QTRY_VERIFY_WITH_TIMEOUT(controller->statusText().contains(QStringLiteral("準備済み")),5000);
        QImage second(80,60,QImage::Format_RGB32); second.fill(Qt::red);
        const auto path=files.filePath("new-original.png"); QVERIFY(second.save(path));
        window->openFile(path); QTRY_COMPARE(view->getImageCore().getSourceImage().size(),second.size());
        QTRY_VERIFY_WITH_TIMEOUT(!controller->isBusy(),3000);
        QVERIFY(!controller->hasResult()); QCOMPARE(controller->resultPasses(),0);
        QVERIFY(!window->findChild<QAction*>("srRepeat")->isEnabled());
    }
    void oversizedRepeatKeepsResult() {
        QImage image(400,400,QImage::Format_RGB32); image.fill(Qt::blue);
        const auto path=files.filePath("memory-limit.png"); QVERIFY(image.save(path));
        window->openFile(path); QTRY_COMPARE(view->getImageCore().getSourceImage().size(),image.size());
        auto small=config; small.memoryMiB=1024; controller->setConfiguration(small);
        QSignalSpy ready(controller,&Sr::Controller::resultReady),failure(controller,&Sr::Controller::failed);
        click("srRun"); QTRY_COMPARE_WITH_TIMEOUT(ready.count(),1,8000);
        const auto previous=controller->resultImage();
        click("srRepeat"); QCOMPARE(failure.count(),1); QVERIFY(!controller->isBusy());
        QCOMPARE(controller->resultImage(),previous); QCOMPARE(controller->resultPasses(),1);
        QVERIFY(controller->statusText().contains(QStringLiteral("メモリー不足")));
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
        const auto message=label->text();
        QVERIFY(message.endsWith(QStringLiteral("とても長いエラー表示です")));
        QVERIFY(!message.contains(QStringLiteral("詳細")));
        QVERIFY(label->toolTip().size()>1000); QVERIFY(label->toolTip().contains('\n'));
        window->resize(1500,650); QTest::qWait(100);
        const auto labelRight=label->mapTo(toolbar,QPoint(label->width(),0)).x();
        QVERIFY2(toolbar->width()-labelRight<16,"Status must extend to the right edge");
        QVERIFY(label->width()>label->fontMetrics().horizontalAdvance(message)+2*label->margin());
        const int wide=label->width();
        window->resize(1200,650); QTest::qWait(100);
        QVERIFY(label->width()<wide); QCOMPARE(toolbar->height(),height);
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
    void animationFramesPlaybackAndRepeat() {
        const auto path=gifFixture(); QVERIFY(!path.isEmpty()); loadAnimation(path);
        const auto pid=controller->backendPid();
        QSignalSpy initialized(controller,&Sr::Controller::backendInitialized);
        QSignalSpy ready(controller,&Sr::Controller::resultReady),changed(controller,&Sr::Controller::animationFrameChanged);
        click("srRun"); QTRY_COMPARE_WITH_TIMEOUT(ready.count(),1,8000);
        QVERIFY(controller->hasAnimation()); QCOMPARE(controller->animationFrameCount(),3);
        QCOMPARE(controller->animationLoopCount(),-1);
        QCOMPARE(controller->animationDelay(0),80); QCOMPARE(controller->animationDelay(1),160); QCOMPARE(controller->animationDelay(2),240);
        QTRY_VERIFY_WITH_TIMEOUT(changed.count()>=3,2000);
        QVERIFY(controller->animationPlaying()); window->pause(); QVERIFY(!controller->animationPlaying());
        const int paused=controller->animationFrameIndex(); QTest::qWait(300); QCOMPARE(controller->animationFrameIndex(),paused);
        while(controller->animationFrameIndex()!=0) window->nextFrame();
        view->originalSize(); view->zoom(1.25);
        const qreal shownWidth=view->transform().m11()*view->getLoadedPixmap().width();
        QImageReader reader(path);
        QVector<QImage> originals,firstResults;
        for(int i=0;i<3;++i) {
            originals<<reader.read();
            QCOMPARE(controller->animationFrameIndex(),i);
            QCOMPARE(controller->resultImage().size(),QSize(64,48)); firstResults<<controller->resultImage();
            const QImage input(files.filePath(QStringLiteral("model.frame-%1.png").arg(i+1)));
            QVERIFY(!input.isNull()); QCOMPARE(input.size(),QSize(32,24));
            QCOMPARE(input.pixelColor(i*10+3,10),originals.last().pixelColor(i*10+3,10));
            QCOMPARE(input.pixelColor(31,0),QColor(Qt::black)); // Hidden RGB excluded from SR.
            QCOMPARE(controller->resultImage().pixelColor(i*20+6,20).alpha(),255);
            QCOMPARE(controller->resultImage().pixelColor(62,0).alpha(),0);
            click("srToggle"); QCOMPARE(view->getLoadedPixmap().size(),QSize(32,24));
            const auto displayed=view->getLoadedPixmap().toImage();
            QCOMPARE(displayed.pixelColor(i*10+3,10),originals.last().pixelColor(i*10+3,10));
            QCOMPARE(displayed.pixelColor(31,0).alpha(),0);
            if(i>0) QCOMPARE(displayed.pixelColor((i-1)*10+3,10).alpha(),0); // Disposal removes the preceding block.
            click("srToggle");
            QVERIFY(qAbs(view->transform().m11()*view->getLoadedPixmap().width()-shownWidth)<1);
            window->nextFrame();
        }
        QVERIFY(firstResults[0]!=firstResults[1]); QVERIFY(firstResults[1]!=firstResults[2]);
        window->increaseSpeed(); QCOMPARE(controller->animationSpeed(),125);
        window->decreaseSpeed(); QCOMPARE(controller->animationSpeed(),100);
        window->decreaseSpeed(); window->resetSpeed(); QCOMPARE(controller->animationSpeed(),100);
        for(auto* action:window->findChildren<QAction*>()) {
            const auto data=action->data().toStringList();
            if(!data.isEmpty() && data.last()=="gifdisable") QVERIFY(action->isEnabled());
        }
        click("srRepeat"); QTRY_COMPARE_WITH_TIMEOUT(ready.count(),2,8000); controller->setAnimationPaused(true);
        QCOMPARE(controller->animationFrameCount(),3); QCOMPARE(controller->resultImage().size(),QSize(128,96));
        QCOMPARE(controller->resultScale(),4.0); QCOMPARE(controller->resultPasses(),2);
        QCOMPARE(controller->backendPid(),pid); QCOMPARE(initialized.count(),0);
        while(controller->animationFrameIndex()!=0) window->nextFrame();
        for(int i=0;i<3;++i) {
            const QImage input(files.filePath(QStringLiteral("model.frame-%1.png").arg(i+4)));
            QCOMPARE(input.size(),QSize(64,48));
            QCOMPARE(input.pixelColor(i*20+6,20),QColor(firstResults[i].pixelColor(i*20+6,20).rgb()));
            click("srToggle");
            QCOMPARE(view->getLoadedPixmap().size(),originals[i].size());
            QCOMPARE(view->getLoadedPixmap().toImage().pixelColor(i*10+3,10),originals[i].pixelColor(i*10+3,10));
            click("srToggle"); window->nextFrame();
        }
        view->rotateImage(90); window->nextFrame();
        QCOMPARE(view->getLoadedPixmap().size(),QSize(96,128));
        QString error; QVERIFY2(controller->saveResult(files.filePath("gif-frame.png"),"png",&error),qPrintable(error));
        QCOMPARE(QImage(files.filePath("gif-frame.png")).size(),QSize(96,128));
        click("srRun"); QTRY_COMPARE_WITH_TIMEOUT(ready.count(),3,8000); controller->setAnimationPaused(true);
        QCOMPARE(controller->resultScale(),2.0); QCOMPARE(controller->resultPasses(),1);
        QCOMPARE(controller->resultImage().size(),QSize(64,48));
        // Navigation releases both frame sequences and stops the playback timer.
        window->openFile(sourcePath);
        QTRY_VERIFY(!controller->hasAnimation()); const int events=changed.count();
        QTest::qWait(300); QCOMPARE(changed.count(),events); QVERIFY(!controller->hasResult());
    }
    void animationPanStaysFixed_data() {
        QTest::addColumn<bool>("sr"); QTest::addColumn<bool>("smooth"); QTest::addColumn<int>("rotation");
        for(bool sr:{false,true}) for(bool smooth:{false,true}) for(int rotation:{0,90})
            QTest::newRow(qPrintable(QString("%1-smooth%2-rot%3").arg(sr?"sr":"original").arg(smooth).arg(rotation)))<<sr<<smooth<<rotation;
    }
    void animationPanStaysFixed() {
        QFETCH(bool,sr); QFETCH(bool,smooth); QFETCH(int,rotation);
        QSettings().setValue("options/scalingenabled",smooth);
        qvApp->getSettingsManager().loadSettings();
        loadAnimation(gifFixture());
        if(sr) { QSignalSpy ready(controller,&Sr::Controller::resultReady); click("srRun"); QTRY_COMPARE_WITH_TIMEOUT(ready.count(),1,8000); controller->setAnimationPaused(true); }
        else view->setPaused(true);
        if(rotation) view->rotateImage(rotation);
        view->resetScale(); view->zoom(2.35); QTest::qWait(80);
        // Test the raw and resampled display paths after dragging away from centre.
        if(smooth) view->scaleExpensively(); else view->makeUnscaled();
        view->horizontalScrollBar()->setValue(view->horizontalScrollBar()->value()+57);
        view->verticalScrollBar()->setValue(view->verticalScrollBar()->value()+39);
        const auto transform=view->transform();
        const QPoint before(view->horizontalScrollBar()->value(),view->verticalScrollBar()->value());
        QSignalSpy srFrames(controller,&Sr::Controller::animationFrameChanged);
        QSignalSpy originalFrames(&view->getLoadedMovie(),&QMovie::frameChanged);
        for(int i=0;i<120;++i) {
            if(sr) controller->stepAnimation();
            else view->jumpToNextFrame();
        }
        QCOMPARE(sr?srFrames.count():originalFrames.count(),120);
        const QPoint after(view->horizontalScrollBar()->value(),view->verticalScrollBar()->value());
        qInfo()<<"Pan before/after"<<before<<after;
        QCOMPARE(view->transform(),transform); QCOMPARE(after,before);
        if(sr) {
            for(int i=0;i<20;++i) { controller->toggle(); controller->toggle(); }
            QVERIFY(qAbs(view->horizontalScrollBar()->value()-before.x())<=1);
            QVERIFY(qAbs(view->verticalScrollBar()->value()-before.y())<=1);
        }
    }
    void gifExportRoundTrip_data() {
        QTest::addColumn<int>("loops"); QTest::addColumn<int>("rotation");
        for(int loops:{-1,0,2}) for(int rotation:{0,90})
            QTest::newRow(qPrintable(QString("loops%1-rot%2").arg(loops).arg(rotation)))<<loops<<rotation;
    }
    void gifExportRoundTrip() {
        QFETCH(int,loops); QFETCH(int,rotation);
        QVector<QImage> frames;
        for(int i=0;i<3;++i) {
            QImage image(32,24,QImage::Format_RGBA8888); image.fill(Qt::transparent); image.setColorSpace(QColorSpace::SRgb);
            for(int y=4;y<20;++y) for(int x=i*10;x<i*10+8;++x) image.setPixelColor(x,y,QColor(i==0?255:0,i==1?255:0,i==2?255:0));
            image.setPixelColor(30,23,QColor(120,80,40,127)); image.setPixelColor(31,23,QColor(120,80,40,128));
            frames<<image;
        }
        const QVector<int> delays{80,160,240}; std::atomic_bool cancelled{false};
        const auto path=files.filePath("保存 % GIF.gif");
        QString error=Sr::writeGif(path,frames,delays,loops,rotation,cancelled);
        QVERIFY2(error.isEmpty(),qPrintable(error));
        QImageReader reader(path); QCOMPARE(reader.imageCount(),3); QCOMPARE(reader.loopCount(),loops);
        QTransform transform; transform.rotate(rotation);
        for(int i=0;i<3;++i) {
            const auto actual=reader.read(); const auto expected=frames[i].transformed(transform);
            QCOMPARE(actual.size(),expected.size()); QCOMPARE(reader.nextImageDelay(),delays[i]);
            for(int y=0;y<actual.height();++y) for(int x=0;x<actual.width();++x) {
                auto color=expected.pixelColor(x,y); const bool visible=color.alpha()>=128;
                QCOMPARE(actual.pixelColor(x,y).alpha(),visible?255:0);
                if(visible) { color.setAlpha(255); QCOMPARE(actual.pixelColor(x,y),color); }
            }
        }
        QVERIFY(!reader.canRead());
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); const auto bytes=file.readAll();
        int offset=bytes.indexOf("ICCRGBG1012"); QVERIFY(offset>0); offset+=11;
        QByteArray icc;
        while(offset<bytes.size() && uchar(bytes[offset])) {
            const int count=uchar(bytes[offset++]); icc+=bytes.mid(offset,count); offset+=count;
        }
        QCOMPARE(icc,Sr::srgbProfile());
    }
    void gifExportQuantizationAndCancel_data() {
        QTest::addColumn<bool>("alpha"); QTest::newRow("opaque")<<false; QTest::newRow("transparent")<<true;
    }
    void gifExportQuantizationAndCancel() {
        QFETCH(bool,alpha);
        QImage gradient(256,128,QImage::Format_RGBA8888); gradient.setColorSpace(QColorSpace::SRgb);
        for(int y=0;y<128;++y) for(int x=0;x<256;++x) gradient.setPixelColor(x,y,QColor(x,y*2,(x+y)%256));
        if(alpha) for(int x=0;x<256;++x) gradient.setPixelColor(x,0,Qt::transparent);
        const QVector<QImage> frames{gradient,gradient,gradient}; const QVector<int> delays{100,200,300};
        std::atomic_bool cancelled{false}; const auto path=files.filePath("quantized.gif");
        auto error=Sr::writeGif(path,frames,delays,-1,0,cancelled);
        QVERIFY2(error.isEmpty(),qPrintable(error));
        QImageReader reader(path); const auto first=reader.read();
        double squaredError=0;
        for(int y=0;y<128;++y) for(int x=0;x<256;++x) {
            const auto a=first.pixelColor(x,y),b=gradient.pixelColor(x,y);
            QCOMPARE(a.alpha(),b.alpha());
            if(!b.alpha()) continue;
            squaredError+=qPow(a.red()-b.red(),2)+qPow(a.green()-b.green(),2)+qPow(a.blue()-b.blue(),2);
        }
        const auto rmse=qSqrt(squaredError/(256*128*3)); qInfo()<<"GIF RGB RMSE"<<rmse; QVERIFY(rmse<18);
        for(int frame=1;frame<3;++frame) {
            const auto next=reader.read();
            // Qt may change hidden RGB values under alpha=0 after disposal.
            // Compare displayed pixels and alpha, including palette/dither stability.
            QCOMPARE(next.convertToFormat(QImage::Format_ARGB32_Premultiplied),
                     first.convertToFormat(QImage::Format_ARGB32_Premultiplied));
        }
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); const auto previous=file.readAll(); file.close();
        error=Sr::writeGif(path,frames,delays,-1,0,cancelled,[&](int progress) { if(progress==4) cancelled=true; });
        QVERIFY(!error.isEmpty()); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(),previous); file.close();
        cancelled=false;
        auto invalid=frames; invalid[2]=QImage(10,10,QImage::Format_RGB32);
        QVERIFY(!Sr::writeGif(path,invalid,delays,-1,0,cancelled).isEmpty());
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(),previous);
        QVERIFY(!Sr::writeGif(files.filePath("no-directory/file.gif"),frames,delays,-1,0,cancelled).isEmpty());
    }
    void animationExportController() {
        const auto original=gifFixture(); loadAnimation(original);
        QSignalSpy ready(controller,&Sr::Controller::resultReady),saved(controller,&Sr::Controller::animationSaved);
        click("srRun"); QTRY_COMPARE_WITH_TIMEOUT(ready.count(),1,8000); controller->setAnimationPaused(true);
        QString error; QVERIFY(!controller->saveAnimation(original,&error));
        const auto path=files.filePath("animation-export.gif");
        QVERIFY2(controller->saveAnimation(path,&error),qPrintable(error));
        QVERIFY(controller->isBusy()); QVERIFY(!window->findChild<QAction*>("srRun")->isEnabled());
        QTRY_COMPARE_WITH_TIMEOUT(saved.count(),1,8000); QVERIFY(!controller->isBusy());
        QImageReader reader(path); QCOMPARE(reader.imageCount(),3); QCOMPARE(reader.size(),QSize(64,48)); QCOMPARE(reader.loopCount(),-1);
        QFile before(path); QVERIFY(before.open(QIODevice::ReadOnly)); const auto bytes=before.readAll(); before.close();
        QFile profile(files.filePath("gif-display.icc")); QVERIFY(profile.open(QIODevice::WriteOnly)); profile.write(QColorSpace(QColorSpace::AdobeRgb).iccProfile()); profile.close();
        auto alternate=controller->configuration(); alternate.displayProfile=profile.fileName(); controller->setConfiguration(alternate);
        QVERIFY(controller->saveAnimation(path,&error)); QTRY_COMPARE_WITH_TIMEOUT(saved.count(),2,8000);
        QVERIFY(before.open(QIODevice::ReadOnly)); QCOMPARE(before.readAll(),bytes); before.close();
        view->rotateImage(90); QVERIFY(controller->saveAnimation(path,&error)); QTRY_COMPARE_WITH_TIMEOUT(saved.count(),3,8000);
        QImageReader rotated(path); QCOMPARE(rotated.size(),QSize(48,64));
        // Cancel immediately, including on source navigation, before publishing a replacement.
        QVERIFY(before.open(QIODevice::ReadOnly)); const auto rotatedBytes=before.readAll(); before.close();
        QVERIFY(controller->saveAnimation(path,&error)); controller->cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!controller->isBusy(),8000); QCOMPARE(saved.count(),3);
        QVERIFY(before.open(QIODevice::ReadOnly)); QCOMPARE(before.readAll(),rotatedBytes); before.close();
        QVERIFY(controller->saveAnimation(path,&error)); window->openFile(sourcePath);
        QTRY_VERIFY_WITH_TIMEOUT(!controller->isBusy(),8000); QCOMPARE(saved.count(),3); QVERIFY(!controller->hasAnimation());
        QVERIFY(before.open(QIODevice::ReadOnly)); QCOMPARE(before.readAll(),rotatedBytes);
    }
    void animationSaveDialog_data() {
        QTest::addColumn<bool>("gif"); QTest::newRow("whole-gif")<<true; QTest::newRow("png-frame")<<false;
    }
    void animationSaveDialog() {
        QFETCH(bool,gif); loadAnimation(gifFixture());
        QSignalSpy ready(controller,&Sr::Controller::resultReady),saved(controller,&Sr::Controller::animationSaved);
        click("srRun"); QTRY_COMPARE_WITH_TIMEOUT(ready.count(),1,8000);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        const auto path=files.filePath(gif?"dialog-save-gif":"dialog-save-png"); bool sawDialog=false;
        QTimer::singleShot(0,window,[&,this] {
            auto* dialog=window->findChild<QFileDialog*>();
            if(!dialog) return;
            sawDialog=dialog->selectedNameFilter().startsWith("GIF");
            if(!gif) {
                const auto filter=dialog->nameFilters()[1]; dialog->selectNameFilter(filter);
                QMetaObject::invokeMethod(dialog,"filterSelected",Qt::DirectConnection,Q_ARG(QString,filter));
            }
            dialog->selectFile(path); QMetaObject::invokeMethod(dialog,"accept",Qt::DirectConnection);
        });
        controller->saveAs(); QVERIFY(sawDialog);
        QTRY_VERIFY_WITH_TIMEOUT(!controller->isBusy(),8000);
        QImageReader reader(path+(gif?".gif":".png")); QVERIFY2(reader.canRead(),qPrintable(reader.errorString()));
        QCOMPARE(reader.size(),QSize(64,48));
        if(gif) { QCOMPARE(reader.imageCount(),3); QCOMPARE(saved.count(),1); }
        else QCOMPARE(reader.imageCount(),1);
    }
    void animationFinitePlayback_data() {
        QTest::addColumn<int>("repeats");
        QTest::newRow("once")<<0; QTest::newRow("repeat-once")<<1;
    }
    void animationFinitePlayback() {
        QFETCH(int,repeats);
        const auto path=gifFixture(repeats>0);
        if(repeats>0) {
            QFile file(path); QVERIFY(file.open(QIODevice::ReadWrite)); auto bytes=file.readAll();
            bytes[bytes.indexOf("NETSCAPE2.0")+13]=char(repeats);
            QVERIFY(file.seek(0)); QCOMPARE(file.write(bytes),bytes.size()); file.close();
        }
        loadAnimation(path);
        QSignalSpy ready(controller,&Sr::Controller::resultReady),changed(controller,&Sr::Controller::animationFrameChanged);
        click("srRun"); QTRY_COMPARE_WITH_TIMEOUT(ready.count(),1,8000);
        QCOMPARE(controller->animationLoopCount(),repeats);
        QTRY_VERIFY_WITH_TIMEOUT(!controller->animationPlaying(),2000);
        QCOMPARE(controller->animationFrameIndex(),2); QCOMPARE(changed.count(),3*(repeats+1)-1);
        window->pause(); QCOMPARE(controller->animationFrameIndex(),0); QVERIFY(controller->animationPlaying());
        window->pause(); window->nextFrame(); window->nextFrame();
        QCOMPARE(controller->animationFrameIndex(),2);
        window->pause(); QCOMPARE(controller->animationFrameIndex(),2); // A manual pause on the last frame must not rewind.
        QTRY_VERIFY_WITH_TIMEOUT(!controller->animationPlaying(),2000);
    }
    void animationFailure_data() {
        QTest::addColumn<bool>("existing");
        QTest::newRow("initial")<<false; QTest::newRow("repeat")<<true;
    }
    void animationFailure() {
        QFETCH(bool,existing);
        loadAnimation(gifFixture(),existing?"animation":"animation-bad-second");
        QSignalSpy ready(controller,&Sr::Controller::resultReady),failed(controller,&Sr::Controller::failed);
        QImage previous;
        if(existing) {
            click("srRun"); QTRY_COMPARE_WITH_TIMEOUT(ready.count(),1,8000);
            controller->setAnimationPaused(true); previous=controller->resultImage();
            auto bad=controller->configuration(); bad.devices="animation-bad-second"; controller->setConfiguration(bad);
        }
        if(existing) click("srRepeat"); else click("srRun");
        QTRY_VERIFY_WITH_TIMEOUT(!controller->isBusy(),8000);
        QCOMPARE(failed.count(),1); QCOMPARE(ready.count(),existing?1:0);
        QCOMPARE(controller->hasAnimation(),existing);
        if(existing) { QCOMPARE(controller->resultImage(),previous); QCOMPARE(controller->resultPasses(),1); }
        else { QVERIFY(!controller->hasResult()); QVERIFY(view->getCurrentFileDetails().isMovieLoaded); }
    }
    void animationCancelAndNavigate_data() {
        QTest::addColumn<bool>("navigate");
        QTest::newRow("cancel")<<false; QTest::newRow("navigate")<<true;
    }
    void animationCancelAndNavigate() {
        QFETCH(bool,navigate); loadAnimation(gifFixture());
        QSignalSpy ready(controller,&Sr::Controller::resultReady);
        click("srRun"); QTRY_COMPARE_WITH_TIMEOUT(ready.count(),1,8000);
        controller->setAnimationPaused(true); const auto previous=controller->resultImage();
        auto delayed=controller->configuration(); delayed.devices="animation-delay"; controller->setConfiguration(delayed);
        QTRY_VERIFY_WITH_TIMEOUT(controller->backendReady(),5000);
        const auto pid=controller->backendPid(); QFile::remove(files.filePath("model.frames.jsonl"));
        click("srRepeat");
        auto inputs=[this] { QFile log(files.filePath("model.frames.jsonl")); if(!log.open(QIODevice::ReadOnly)) return 0; return int(log.readAll().count('\n')); };
        QTRY_COMPARE_WITH_TIMEOUT(inputs(),2,5000); // First frame finished; second is still running.
        QVERIFY(controller->isBusy()); QCOMPARE(controller->resultImage(),previous); QCOMPARE(ready.count(),1);
        if(navigate) window->openFile(sourcePath); else click("srCancel");
        QTRY_VERIFY_WITH_TIMEOUT(!controller->isBusy(),5000);
        QCOMPARE(ready.count(),1); QCOMPARE(controller->backendPid(),pid);
        if(navigate) { QVERIFY(!controller->hasAnimation()); QVERIFY(!controller->hasResult()); }
        else { QCOMPARE(controller->resultImage(),previous); QCOMPARE(controller->animationFrameCount(),3); }
    }
    void animationMemoryBudget() {
        auto path=gifFixture(); QFile file(path); QVERIFY(file.open(QIODevice::ReadWrite));
        // Large logical screen with small delta frames: reject based on composed frames.
        QVERIFY(file.seek(6)); QCOMPARE(file.write(QByteArray::fromHex("00080008")),qint64(4)); file.close();
        loadAnimation(path);
        auto limited=controller->configuration(); limited.animationMemoryMiB=1024; controller->setConfiguration(limited);
        QSignalSpy failed(controller,&Sr::Controller::failed);
        click("srRun"); QTRY_VERIFY_WITH_TIMEOUT(!controller->isBusy(),5000);
        QCOMPARE(failed.count(),1); QVERIFY(controller->statusText().contains(QStringLiteral("GIFメモリー不足")));
        QVERIFY(!controller->hasResult()); QVERIFY(view->getCurrentFileDetails().isMovieLoaded);
    }
    void cmykDecodedRgb() {
        QSignalSpy ready(controller,&Sr::Controller::resultReady);
        const auto cmyk=files.filePath("cmyk.jpg");
        QFile jpeg(cmyk); QVERIFY(jpeg.open(QIODevice::WriteOnly));
        jpeg.write(QByteArray::fromBase64("/9j/7gAOQWRvYmUAZAAAAAAA/9sAQwAIBgYHBgUIBwcHCQkICgwUDQwLCwwZEhMPFB0aHx4dGhwcICQuJyAiLCMcHCg3KSwwMTQ0NB8nOT04MjwuMzQy/8AAFAgAGAAgBEMRAE0RAFkRAEsRAP/EAB8AAAEFAQEBAQEBAAAAAAAAAAABAgMEBQYHCAkKC//EALUQAAIBAwMCBAMFBQQEAAABfQECAwAEEQUSITFBBhNRYQcicRQygZGhCCNCscEVUtHwJDNicoIJChYXGBkaJSYnKCkqNDU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6g4SFhoeIiYqSk5SVlpeYmZqio6Slpqeoqaqys7S1tre4ubrCw8TFxsfIycrS09TV1tfY2drh4uPk5ebn6Onq8fLz9PX29/j5+v/aAA4EQwBNAFkASwAAPwD2uuvr3+vdaKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK//Z")); jpeg.close();
        window->openFile(cmyk);
        QTRY_COMPARE_WITH_TIMEOUT(view->getCurrentFileDetails().fileInfo.absoluteFilePath(),QFileInfo(cmyk).absoluteFilePath(),5000);
        QTRY_VERIFY_WITH_TIMEOUT(window->findChild<QAction*>("srRun")->isEnabled(),5000);
        QVERIFY(view->getImageCore().getSourceProfile().assumedSrgb);
        click("srRun"); QTRY_COMPARE_WITH_TIMEOUT(ready.count(),1,8000);
        QCOMPARE(controller->resultImage().size(),QSize(128,96));
    }
    void realAnimationGui() {
        if(!qEnvironmentVariableIsSet("QVIEWSR_REAL_WORKER")) QSKIP("Opt-in real-device GIF test.");
        auto real=config; real.workerPath=qEnvironmentVariable("QVIEWSR_REAL_WORKER");
        real.runtimeRoot=qEnvironmentVariable("QVIEWSR_REAL_RUNTIME"); real.modelPath=qEnvironmentVariable("QVIEWSR_REAL_MODEL");
        real.devices="all"; real.scale=2.0;
        QSignalSpy initialized(controller,&Sr::Controller::backendInitialized),failure(controller,&Sr::Controller::failed);
        controller->setConfiguration(real);
        QTRY_VERIFY_WITH_TIMEOUT(controller->backendReady() || !failure.isEmpty(),120000);
        QVERIFY2(failure.isEmpty(),qPrintable(controller->statusText()));
        const auto pid=controller->backendPid(); QCOMPARE(initialized.count(),1);
        const auto path=qEnvironmentVariable("QVIEWSR_REAL_IMAGE");
        QImageReader reader(path); QVector<QImage> originals; QVector<int> delays;
        while(reader.canRead()) { originals<<reader.read(); delays<<reader.nextImageDelay(); }
        QVERIFY(originals.size()>1); const int loops=reader.loopCount();
        window->openFile(path);
        QTRY_VERIFY_WITH_TIMEOUT(view->getCurrentFileDetails().isMovieLoaded,5000);
        QSignalSpy ready(controller,&Sr::Controller::resultReady),frames(controller,&Sr::Controller::animationFrameChanged);
        click("srRun"); QTRY_VERIFY_WITH_TIMEOUT(ready.count()==1 || !failure.isEmpty(),120000);
        QVERIFY2(failure.isEmpty(),qPrintable(controller->statusText()));
        QCOMPARE(controller->animationFrameCount(),originals.size()); QCOMPARE(controller->animationLoopCount(),loops);
        const auto firstSummary=window->findChild<QLabel*>("srStatus")->toolTip();
        QCOMPARE(firstSummary.count(QStringLiteral("タイル")),4);
        QTRY_VERIFY_WITH_TIMEOUT(frames.count()>=3,3000);
        controller->setAnimationPaused(true);
        while(controller->animationFrameIndex()!=0) window->nextFrame();
        const auto artifacts=qEnvironmentVariable("QVIEWSR_GUI_ARTIFACTS");
        if(!artifacts.isEmpty()) QDir().mkpath(artifacts);
        QString error; window->resize(1400,850);
        for(int i=0;i<originals.size();++i) {
            QCOMPARE(controller->animationFrameIndex(),i);
            QCOMPARE(controller->animationDelay(i),delays[i]);
            QCOMPARE(controller->resultImage().size(),originals[i].size()*2);
            if(!artifacts.isEmpty()) {
                QVERIFY2(controller->saveResult(artifacts+QStringLiteral("/sr-frame-%1.png").arg(i),"png",&error),qPrintable(error));
                QTest::qWait(100); QVERIFY(window->grab().save(artifacts+QStringLiteral("/gui-frame-%1.png").arg(i)));
            }
            click("srToggle"); QCOMPARE(view->getLoadedPixmap().size(),originals[i].size());
            const auto displayed=view->getLoadedPixmap().toImage();
            QCOMPARE(displayed.pixelColor(30,30),originals[i].pixelColor(30,30));
            click("srToggle"); window->nextFrame();
        }
        click("srRepeat"); QTRY_VERIFY_WITH_TIMEOUT(ready.count()==2 || !failure.isEmpty(),120000);
        QVERIFY2(failure.isEmpty(),qPrintable(controller->statusText())); controller->setAnimationPaused(true);
        QCOMPARE(controller->resultImage().size(),originals[0].size()*4);
        QCOMPARE(controller->animationFrameCount(),originals.size()); QCOMPARE(controller->resultPasses(),2);
        QCOMPARE(controller->backendPid(),pid); QCOMPARE(initialized.count(),1);
        if(!artifacts.isEmpty()) {
            QFile summary(artifacts+"/animation-summary.txt"); QVERIFY(summary.open(QIODevice::WriteOnly));
            summary.write(QStringLiteral("初期化: %1回 / worker PID: %2 / 再生通知: %3\n全フレーム2倍:\n%4\n全フレーム2倍→2倍:\n%5\n")
                .arg(initialized.count()).arg(pid).arg(frames.count()).arg(firstSummary,window->findChild<QLabel*>("srStatus")->toolTip()).toUtf8());
        }
        const auto output=artifacts.isEmpty()?files.filePath("hardware-sr.gif"):artifacts+"/sr-animation.gif";
        QSignalSpy saved(controller,&Sr::Controller::animationSaved);
        QVERIFY2(controller->saveAnimation(output,&error),qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(saved.count()==1 || !failure.isEmpty(),30000);
        QVERIFY2(failure.isEmpty(),qPrintable(controller->statusText()));
        QImageReader exported(output); QCOMPARE(exported.imageCount(),originals.size()); QCOMPARE(exported.loopCount(),loops);
        for(int i=0;i<originals.size();++i) {
            QCOMPARE(exported.read().size(),originals[i].size()*4); QCOMPARE(exported.nextImageDelay(),delays[i]);
        }
        // Exercise actual timed playback while zoomed beyond the viewport, after panning.
        view->resetScale(); view->zoom(2.35); QTest::qWait(80);
        view->horizontalScrollBar()->setValue(view->horizontalScrollBar()->value()+71);
        view->verticalScrollBar()->setValue(view->verticalScrollBar()->value()+53);
        const QPoint pan(view->horizontalScrollBar()->value(),view->verticalScrollBar()->value());
        const auto zoom=view->transform(); const int frameEvents=frames.count();
        controller->setAnimationSpeed(500); controller->setAnimationPaused(false);
        QTRY_VERIFY_WITH_TIMEOUT(frames.count()>=frameEvents+60,10000); controller->setAnimationPaused(true);
        QCOMPARE(view->transform(),zoom);
        QCOMPARE(QPoint(view->horizontalScrollBar()->value(),view->verticalScrollBar()->value()),pan);
        if(!artifacts.isEmpty()) QVERIFY(window->grab().save(artifacts+"/gui-gif-export-pan.png"));
        window->openFile(output); QTRY_VERIFY_WITH_TIMEOUT(view->getCurrentFileDetails().isMovieLoaded,5000);
        QCOMPARE(view->getLoadedMovie().frameCount(),originals.size());
    }
    void realScaleAndRepeatGui() {
        if(!qEnvironmentVariableIsSet("QVIEWSR_REAL_WORKER")) QSKIP("Opt-in real-device test.");
        auto real=config; real.workerPath=qEnvironmentVariable("QVIEWSR_REAL_WORKER");
        real.runtimeRoot=qEnvironmentVariable("QVIEWSR_REAL_RUNTIME"); real.modelPath=qEnvironmentVariable("QVIEWSR_REAL_MODEL");
        real.devices="all"; real.scale=2.0;
        QSignalSpy initialized(controller,&Sr::Controller::backendInitialized),failure(controller,&Sr::Controller::failed);
        controller->setConfiguration(real);
        QTRY_VERIFY_WITH_TIMEOUT(controller->backendReady() || !failure.isEmpty(),120000);
        QVERIFY2(failure.isEmpty(),qPrintable(controller->statusText()));
        const auto pid=controller->backendPid(); QCOMPARE(initialized.count(),1);
        const auto path=qEnvironmentVariable("QVIEWSR_REAL_IMAGE"); window->openFile(path);
        QTRY_COMPARE_WITH_TIMEOUT(view->getCurrentFileDetails().fileInfo.absoluteFilePath(),QFileInfo(path).absoluteFilePath(),5000);
        QTRY_VERIFY(window->findChild<QAction*>("srRun")->isEnabled());
        const auto original=view->getImageCore().getSourceImage();
        QSignalSpy ready(controller,&Sr::Controller::resultReady);
        click("srRun"); QTRY_VERIFY_WITH_TIMEOUT(ready.count()==1 || !failure.isEmpty(),120000);
        QVERIFY2(failure.isEmpty(),qPrintable(controller->statusText()));
        QCOMPARE(controller->resultImage().size(),original.size()*2);
        const auto firstSummary=window->findChild<QLabel*>("srStatus")->toolTip();
        QCOMPARE(firstSummary.count(QStringLiteral("タイル")),4);
        window->findChild<QDoubleSpinBox*>("srScale")->setValue(3);
        click("srRepeat"); QTRY_VERIFY_WITH_TIMEOUT(ready.count()==2 || !failure.isEmpty(),120000);
        QVERIFY2(failure.isEmpty(),qPrintable(controller->statusText()));
        QCOMPARE(controller->resultImage().size(),original.size()*6);
        QCOMPARE(controller->resultPasses(),2); QCOMPARE(controller->resultScale(),6.0);
        QCOMPARE(controller->backendPid(),pid); QCOMPARE(initialized.count(),1);
        QCOMPARE(view->getImageCore().getSourceImage(),original);
        click("srToggle"); QVERIFY(!controller->showingSr()); QCOMPARE(view->getLoadedPixmap().size(),original.size());
        click("srToggle");
        const auto artifacts=qEnvironmentVariable("QVIEWSR_GUI_ARTIFACTS");
        if(!artifacts.isEmpty()) {
            QDir().mkpath(artifacts); QString error;
            QVERIFY2(controller->saveResult(artifacts+"/sr-repeat-6x.png","png",&error),qPrintable(error));
            QCOMPARE(QImage(artifacts+"/sr-repeat-6x.png").size(),original.size()*6);
            window->resize(1400,850); QTest::qWait(200);
            QVERIFY(window->grab().save(artifacts+"/gui-repeat-6x.png"));
            QFile summary(artifacts+"/scale-summary.txt"); QVERIFY(summary.open(QIODevice::WriteOnly));
            summary.write(QStringLiteral("初期化回数: %1 / worker PID: %2\n2倍:\n%3\n2倍→3倍（累計6倍）:\n%4\n")
                .arg(initialized.count()).arg(pid).arg(firstSummary,window->findChild<QLabel*>("srStatus")->toolTip()).toUtf8());
        }
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
    qputenv("XDG_CACHE_HOME",settings.filePath("cache").toUtf8());
    QCoreApplication::setOrganizationName("qViewSR-tests");
    QCoreApplication::setApplicationName("qViewSR-tests");
    QSettings s; s.setValue("firstlaunch",true); s.setValue("updatenotifications",false); s.sync();
    QVApplication app(argc,argv);
    SrTests tests;
    return QTest::qExec(&tests,argc,argv);
}
#include "tst_srtests.moc"
