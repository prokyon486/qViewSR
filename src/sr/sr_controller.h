// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QObject>
#include <QImage>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QPointer>
#include <QTimer>
#include <QStringList>
#include <memory>
class MainWindow;
class QVGraphicsView;
class QAction;
class QLabel;
class QProgressBar;
class QToolBar;
class QProcess;

namespace Sr {
struct Configuration {
    QString runtimeRoot, workerPath, modelPath;
    QString devices = QStringLiteral("all");
    QString displayProfile; // empty: auto; "sRGB": forced sRGB; otherwise ICC path
    int memoryMiB = 2048;
    int halo = 16;
    int denoise = 0;
    static Configuration load();
    void save() const;
    QProcessEnvironment environment() const;
};

class Controller : public QObject {
    Q_OBJECT
public:
    Controller(MainWindow* window, QVGraphicsView* view);
    ~Controller() override;
    bool isBusy() const { return bool(job_); }
    bool hasResult() const { return !result_.isNull(); }
    bool showingSr() const { return showingSr_; }
    QImage resultImage() const { return result_; }
    QString statusText() const;
    bool backendReady() const { return backendReady_; }
    qint64 backendPid() const;
    Configuration configuration() const { return configuration_; }
    void setConfiguration(const Configuration& configuration);
    bool saveResult(const QString& path, const QByteArray& format, QString* error);
public slots:
    void start();
    void cancel();
    void toggle();
    void saveAs();
    void showSettings();
    void setFullscreen(bool fullscreen);
signals:
    void stateChanged();
    void resultReady();
    void failed(const QString& message);
    void backendInitialized();
private:
    struct Job;
    void setStatus(QString text);
    void invalidate();
    void sourceLoaded();
    void updateActions();
    void display();
    QByteArray displayIcc(QString* description = nullptr) const;
    void launch(const std::shared_ptr<Job>& job);
    void ensureWorker();
    void stopWorker();
    void restartWorker();
    void readEvents();
    void dispatch();
    void acceptResult(const std::shared_ptr<Job>& job);
    void finish(std::shared_ptr<Job> job, const QString& error = {});
    MainWindow* window_;
    QVGraphicsView* view_;
    QToolBar* toolbar_;
    QAction *run_, *cancel_, *toggle_, *save_, *settings_;
    QLabel* status_;
    QProgressBar* progress_;
    Configuration configuration_;
    std::shared_ptr<Job> job_;
    QImage result_;
    bool showingSr_ = false, sourceReady_ = false, saving_ = false;
    quint64 generation_ = 0;
    QString resultSummary_;
    QTimer watchdog_;
    QTimer backendWatchdog_;
    QPointer<QProcess> worker_;
    QByteArray workerBuffer_, workerLog_;
    QString sessionId_;
    bool backendReady_ = false, stoppingWorker_ = false;
    QStringList backendWarnings_, backendDevices_;
};
}
