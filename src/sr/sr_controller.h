// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QObject>
#include <QImage>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QPointer>
#include <QTimer>
#include <QVector>
#include <QStringList>
#include <memory>
class MainWindow;
class QVGraphicsView;
class QAction;
class QLabel;
class QProgressBar;
class QToolBar;
class QProcess;
class QDoubleSpinBox;

namespace Sr {
struct AnimationFrames;
struct Configuration {
    QString runtimeRoot, workerPath, modelPath;
    QString devices = QStringLiteral("all");
    QString displayProfile; // empty: auto; "sRGB": forced sRGB; otherwise ICC path
    int memoryMiB = 2048;
    int animationMemoryMiB = 8192;
    int halo = 16;
    int denoise = 0;
    double scale = 4.0;
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
    double resultScale() const { return resultScale_; }
    int resultPasses() const { return resultSteps_.size(); }
    bool hasAnimation() const { return bool(resultAnimation_); }
    int animationFrameCount() const;
    int animationFrameIndex() const { return animationIndex_; }
    int animationDelay(int frame) const;
    int animationLoopCount() const;
    bool animationPlaying() const { return animationPlaying_; }
    int animationSpeed() const { return animationSpeed_; }
    void setAnimationPaused(bool paused);
    void stepAnimation();
    void setAnimationSpeed(int percent);
    QString statusText() const;
    bool backendReady() const { return backendReady_; }
    qint64 backendPid() const;
    Configuration configuration() const { return configuration_; }
    void setConfiguration(const Configuration& configuration);
    bool saveResult(const QString& path, const QByteArray& format, QString* error);
public slots:
    void start();
    void startAgain();
    void cancel();
    void toggle();
    void saveAs();
    void saveDisplayedFrameAs();
    void showSettings();
    void setFullscreen(bool fullscreen);
signals:
    void stateChanged();
    void resultReady();
    void failed(const QString& message);
    void backendInitialized();
    void animationFrameChanged(int frame);
private:
    struct Job;
    void setStatus(QString text);
    void saveFrameAs(bool originalFrame);
    void startJob(bool fromResult);
    void prepareFrame(const std::shared_ptr<Job>& job);
    void publishResult(const std::shared_ptr<Job>& job);
    void advanceAnimation();
    void scheduleAnimation(int renderMs = 0);
    quint64 retainedAnimationBytes() const;
    void invalidate();
    void sourceLoaded();
    void updateActions();
    void display(bool updateStatus = true);
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
    QAction *run_, *repeat_, *cancel_, *toggle_, *save_, *settings_;
    QDoubleSpinBox* scale_;
    QLabel* status_;
    QProgressBar* progress_;
    Configuration configuration_;
    std::shared_ptr<Job> job_;
    QImage result_;
    bool showingSr_ = false, sourceReady_ = false, saving_ = false;
    quint64 generation_ = 0;
    QString resultSummary_;
    double resultScale_ = 1.0;
    QStringList resultSteps_;
    std::shared_ptr<const AnimationFrames> originalAnimation_, resultAnimation_;
    QVector<QImage> originalDisplayFrames_, resultDisplayFrames_;
    QByteArray animationDisplayProfile_;
    QTimer animationTimer_;
    int animationIndex_ = 0, animationLoopsDone_ = 0, animationSpeed_ = 100;
    bool animationPlaying_ = false, animationEnded_ = false;
    QTimer watchdog_;
    QTimer backendWatchdog_;
    QPointer<QProcess> worker_;
    QByteArray workerBuffer_, workerLog_;
    QString sessionId_;
    bool backendReady_ = false, stoppingWorker_ = false;
    QStringList backendWarnings_, backendDevices_;
};
}
