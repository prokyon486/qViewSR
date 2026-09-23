// SPDX-License-Identifier: GPL-3.0-or-later
#include <QtTest>
#include <QDateTime>
#include <QThreadPool>
#include <algorithm>
#include "qvapplication.h"
#include "qvgraphicsview.h"

class NavigationTests : public QObject
{
    Q_OBJECT
    QTemporaryDir files;
    QPointer<MainWindow> window;

    static QStringList names(const QList<QVImageCore::CompatibleFile> &entries)
    {
        QStringList result;
        for (const auto &entry : entries) result << entry.fileName;
        return result;
    }
    static void configure(int mode, bool descending = false, int preloading = 0)
    {
        QSettings settings;
        settings.setValue("options/sortmode", mode);
        settings.setValue("options/sortdescending", descending);
        settings.setValue("options/preloadingmode", preloading);
        settings.setValue("options/loopfoldersenabled", true);
        qvApp->getSettingsManager().loadSettings();
    }
    void makeImages(const QString &directory, const QStringList &fileNames)
    {
        QVERIFY(QDir().mkpath(directory));
        QImage image(24,16,QImage::Format_RGB32); image.fill(QColor(50,110,170));
        for (const auto &name : fileNames) {
            const auto path=QDir(directory).filePath(name);
            QVERIFY(image.save(path));
            QFile file(path); QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.setFileTime(QDateTime::fromMSecsSinceEpoch(1700000000000), QFileDevice::FileModificationTime));
        }
    }
private slots:
    void initTestCase()
    {
        QVERIFY(files.isValid());
        makeImages(files.filePath("mixed"), {"z10.png","a10.PNG","_2.png","a2.png","A2.png","_1.PNG","a02.png","Z1.png"});
    }
    void cleanup()
    {
        if (window) {
            QThreadPool::globalInstance()->waitForDone();
            QCoreApplication::processEvents();
            window->close();
            QTRY_VERIFY(window.isNull());
        }
    }
    void refreshKeepsOrder_data()
    {
        QTest::addColumn<int>("mode"); QTest::addColumn<bool>("descending");
        for (int mode=0;mode<=5;++mode) for (bool descending : {false,true})
            QTest::newRow(qPrintable(QString("mode%1-desc%2").arg(mode).arg(descending))) << mode << descending;
    }
    void refreshKeepsOrder()
    {
        QFETCH(int,mode); QFETCH(bool,descending); configure(mode,descending);
        QVImageCore core; core.updateFolderInfo(files.filePath("mixed"));
        const auto expected=names(core.getCurrentFileDetails().folderFileInfoList);
        QCOMPARE(expected.size(),8);
        for (int refresh=0;refresh<5;++refresh) {
            core.updateFolderInfo(files.filePath("mixed"));
            QCOMPARE(names(core.getCurrentFileDetails().folderFileInfoList),expected);
        }
    }
    void tiesHaveDeterministicOrder_data()
    {
        QTest::addColumn<int>("mode");
        for (int mode : {0,1,3,4}) QTest::newRow(qPrintable(QString::number(mode))) << mode;
    }
    void tiesHaveDeterministicOrder()
    {
        QFETCH(int,mode); configure(mode);
        const QStringList forward{"image01.png","image1.png","image2.png","image02.png","image10.png"};
        auto reverse=forward; std::reverse(reverse.begin(),reverse.end());
        QTemporaryDir first(files.filePath("ties-XXXXXX")), second(files.filePath("ties-XXXXXX"));
        QVERIFY(first.isValid()); QVERIFY(second.isValid());
        makeImages(first.path(),forward); makeImages(second.path(),reverse);
        QVImageCore core; core.updateFolderInfo(first.path());
        const auto ordered=names(core.getCurrentFileDetails().folderFileInfoList);
        core.updateFolderInfo(second.path());
        QCOMPARE(names(core.getCurrentFileDetails().folderFileInfoList),ordered);
        QCOMPARE(ordered,QStringList({"image01.png","image1.png","image02.png","image2.png","image10.png"}));
        configure(mode,true); core.updateFolderInfo(second.path());
        auto descending=ordered; std::reverse(descending.begin(),descending.end());
        QCOMPARE(names(core.getCurrentFileDetails().folderFileInfoList),descending);
    }
    void sameCountChangesAreRefreshed()
    {
        configure(0);
        QTemporaryDir directory(files.filePath("changes-XXXXXX")); QVERIFY(directory.isValid());
        makeImages(directory.path(),{"image3.png","image1.png","image2.png"});
        QVImageCore core; core.updateFolderInfo(directory.path());
        QVERIFY(QFile::rename(directory.filePath("image3.png"),directory.filePath("image0.png")));
        core.updateFolderInfo(directory.path());
        QCOMPARE(names(core.getCurrentFileDetails().folderFileInfoList),QStringList({"image0.png","image1.png","image2.png"}));
        configure(3); core.updateFolderInfo(directory.path());
        QFile larger(directory.filePath("image2.png")); QVERIFY(larger.open(QIODevice::Append));
        QCOMPARE(larger.write(QByteArray(1000,'x')),1000); larger.close();
        core.updateFolderInfo(directory.path());
        QCOMPARE(core.getCurrentFileDetails().folderFileInfoList.first().fileName,QString("image2.png"));
        configure(1); core.updateFolderInfo(directory.path());
        QFile newer(directory.filePath("image0.png")); QVERIFY(newer.open(QIODevice::ReadWrite));
        QVERIFY(newer.setFileTime(QDateTime::fromMSecsSinceEpoch(1900000000000),QFileDevice::FileModificationTime)); newer.close();
        core.updateFolderInfo(directory.path());
        QCOMPARE(core.getCurrentFileDetails().folderFileInfoList.first().fileName,QString("image0.png"));
    }
    void randomRefreshKeepsSurvivingOrder()
    {
        configure(5);
        QTemporaryDir directory(files.filePath("random-XXXXXX")); QVERIFY(directory.isValid());
        makeImages(directory.path(),{"_a.png","C.png","b.png","A.png","d.png"});
        QVImageCore core; core.updateFolderInfo(directory.path());
        auto previous=names(core.getCurrentFileDetails().folderFileInfoList);
        const auto removed=previous.takeAt(2); QVERIFY(QFile::remove(directory.filePath(removed)));
        makeImages(directory.path(),{"new.png"}); // Same count, different membership.
        core.updateFolderInfo(directory.path());
        QCOMPARE(names(core.getCurrentFileDetails().folderFileInfoList),previous+QStringList{"new.png"});
        makeImages(directory.path(),{"extra.png"});
        previous=names(core.getCurrentFileDetails().folderFileInfoList);
        core.updateFolderInfo(directory.path());
        QCOMPARE(names(core.getCurrentFileDetails().folderFileInfoList),previous+QStringList{"extra.png"});
    }
    void arrowKeysAfterIdle_data()
    {
        QTest::addColumn<int>("preloading");
        QTest::newRow("no-cache") << 0; QTest::newRow("preload-neighbours") << 1;
    }
    void arrowKeysAfterIdle()
    {
        QFETCH(int,preloading); configure(0,false,preloading);
        QTemporaryDir directory(files.filePath("arrows-XXXXXX")); QVERIFY(directory.isValid());
        makeImages(directory.path(),{"z10.png","a10.PNG","_2.png","a2.png","A2.png","_1.PNG","a02.png","Z1.png"});
        QImage large(4096,4096,QImage::Format_RGB888); large.fill(QColor(90,130,170));
        QVERIFY(large.save(directory.filePath("b50MB.bmp")));
        QVERIFY(QFileInfo(directory.filePath("b50MB.bmp")).size()>50000000); large={};
        window=new MainWindow; window->resize(1000,650); window->show();
        auto *view=window->findChild<QVGraphicsView*>(); QVERIFY(view);
        window->openFile(directory.path());
        QTRY_VERIFY_WITH_TIMEOUT(view->getCurrentFileDetails().isPixmapLoaded,10000);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(window));
        view->setFocus();
        const auto ordered=names(view->getCurrentFileDetails().folderFileInfoList);
        QCOMPARE(ordered.size(),9);
        QCOMPARE(view->getCurrentFileDetails().fileInfo.fileName(),ordered.first());
        for (int i=1;i<ordered.size();++i) {
            QTest::keyClick(window,Qt::Key_Right);
            QTRY_COMPARE_WITH_TIMEOUT(view->getCurrentFileDetails().fileInfo.fileName(),ordered[i],10000);
            QVERIFY(view->getCurrentFileDetails().isPixmapLoaded);
        }
        QTest::qWait(3100); // Real idle refresh path in goToFile().
        for (int i=ordered.size()-2;i>=0;--i) {
            QTest::keyClick(window,Qt::Key_Left);
            QTRY_COMPARE_WITH_TIMEOUT(view->getCurrentFileDetails().fileInfo.fileName(),ordered[i],10000);
            QCOMPARE(view->getCurrentFileDetails().loadedIndexInFolder,i);
        }
        QTest::keyClick(window,Qt::Key_Left);
        QTRY_COMPARE_WITH_TIMEOUT(view->getCurrentFileDetails().fileInfo.fileName(),ordered.last(),10000);
        QTest::keyClick(window,Qt::Key_Right);
        QTRY_COMPARE_WITH_TIMEOUT(view->getCurrentFileDetails().fileInfo.fileName(),ordered.first(),10000);
    }
};

int main(int argc,char **argv)
{
    QTemporaryDir settings; qputenv("XDG_CONFIG_HOME",settings.path().toUtf8());
    QCoreApplication::setOrganizationName("qViewSR-tests");
    QCoreApplication::setApplicationName("navigation-tests");
    QLocale::setDefault(QLocale(QLocale::Japanese,QLocale::Japan));
    QSettings config; config.setValue("firstlaunch",true);
    config.setValue("sr/worker",settings.filePath("no-worker")); config.sync();
    QVApplication app(argc,argv); NavigationTests tests;
    return QTest::qExec(&tests,argc,argv);
}

#include "tst_navigationtests.moc"
