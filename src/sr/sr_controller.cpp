// SPDX-License-Identifier: GPL-3.0-or-later
#include "sr_controller.h"
#include "color_pipeline.h"
#include "gif_export.h"
#include "mainwindow.h"
#include "qvgraphicsview.h"
#include <QAction>
#include <QCheckBox>
#include <QColorDialog>
#include <QColorSpace>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDateTime>
#include <QElapsedTimer>
#include <QMap>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QProcess>
#include <QProgressBar>
#include <QPromise>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QToolBar>
#include <QUuid>
#include <QVBoxLayout>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>
#include <atomic>
#include <cmath>

namespace Sr {
struct AnimationFrames {
    QVector<QImage> frames;
    QVector<int> delays;
    int loops=0; // Qt convention: 0 plays once, -1 repeats forever.
    quint64 bytes() const { quint64 n=0; for(const auto& frame:frames) n+=quint64(frame.sizeInBytes()); return n; }
};
namespace {
quint64 animationEstimate(const AnimationFrames& input,double scale,quint64 retained) {
    quint64 output=0,maxPixels=0;
    for(const auto& frame:input.frames) {
        const quint64 pixels=quint64(frame.width())*frame.height();
        maxPixels=qMax(maxPixels,pixels);
        output+=quint64(qRound(frame.width()*scale))*qRound(frame.height()*scale)*8;
    }
    return retained+input.bytes()*2+output+maxPixels*320+4*192ULL*1024*1024;
}
QString animationMemoryError(quint64 required,quint64 budget) {
    return QStringLiteral("GIFメモリー不足 · 見積り %1 MiB / 上限 %2 MiB\nSR設定のアニメーション用メモリー上限を増やしてください。")
        .arg((required+1024*1024-1)/(1024*1024)).arg(budget/(1024*1024));
}

QString scaleText(double scale) { return QString::number(scale,'g',6); }
double validScale(double scale) { return std::isfinite(scale)?qBound(1.25,scale,4.0):4.0; }
// Long text must not enlarge the toolbar's size hint or push the label into
// its overflow menu. Expand into all space left after the controls instead.
class StatusLabel : public QLabel {
public:
    using QLabel::QLabel;
    QSize sizeHint() const override { return {150,height()}; }
    QSize minimumSizeHint() const override { return {0,height()}; }
};
QString repositoryRoot() {
    QDir dir(QCoreApplication::applicationDirPath());
    for(int i=0;i<5;++i) {
        if(QFileInfo::exists(dir.filePath("docs/sr/source-lock.json"))) return dir.absolutePath();
        if(!dir.cdUp()) break;
    }
    return QCoreApplication::applicationDirPath();
}
QByteArray hash(const QString& path) {
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash digest(QCryptographicHash::Sha256);
    if(!digest.addData(&file)) return {};
    return digest.result().toHex();
}
QString readFailure(const QString& message) { return QStringLiteral("超解像: ") + message; }
bool transparent(const QImage& image) {
    if(!image.hasAlphaChannel()) return false;
    const auto rgba=image.convertToFormat(QImage::Format_RGBA8888);
    for(int y=0;y<rgba.height();++y) {
        const auto* row=rgba.constScanLine(y);
        for(int x=0;x<rgba.width();++x) if(row[x*4+3]!=255) return true;
    }
    return false;
}
bool writeImage(QImage image,const QString& path,QByteArray format,const QColor& background,QString* error) {
    format=format.toLower();
    if(format=="jpg") format="jpeg";
    if(format!="png" && format!="jpeg") { if(error)*error=QStringLiteral("PNG/JPEGを選択してください"); return false; }
    if(format=="jpeg") {
        QImage flattened(image.size(),QImage::Format_RGB32);
        if(flattened.isNull()) { if(error)*error=QStringLiteral("保存用メモリーを確保できません"); return false; }
        flattened.fill(background);
        QPainter painter(&flattened); painter.drawImage(0,0,image); painter.end(); image=flattened;
    }
    image.setColorSpace(QColorSpace::SRgb);
    QSaveFile file(path);
    if(!file.open(QIODevice::WriteOnly)) { if(error)*error=file.errorString(); return false; }
    QImageWriter writer(&file,format);
    if(format=="jpeg") writer.setQuality(95);
    if(!writer.write(image)) { if(error)*error=writer.errorString(); file.cancelWriting(); return false; }
    if(!file.commit()) { if(error)*error=file.errorString(); return false; }
    return true;
}
}

Configuration Configuration::load() {
    Configuration result;
    QSettings settings;
    const auto root=repositoryRoot();
    QString assets=QDir(root).filePath(".local");
    if(!QFileInfo::exists(assets+"/models/1032-fp16")) assets=QDir(root).filePath("../.local");
    result.runtimeRoot=settings.value("sr/runtime",assets+"/openvino-2020.3.355/l_openvino_toolkit_runtime_ubuntu18_p_2020.3.355").toString();
    result.workerPath=settings.value("sr/worker",root+"/build/worker/ncs-sr-worker").toString();
    result.modelPath=settings.value("sr/model",assets+"/models/1032-fp16/single-image-super-resolution-1032.xml").toString();
    result.devices=settings.value("sr/devices","all").toString();
    result.displayProfile=settings.value("sr/displayProfile").toString();
    result.memoryMiB=qBound(1024,settings.value("sr/memoryMiB",2048).toInt(),16384);
    result.animationMemoryMiB=qBound(1024,settings.value("sr/animationMemoryMiB",8192).toInt(),65536);
    result.halo=qBound(0,settings.value("sr/halo",16).toInt(),64);
    result.denoise=qBound(0,settings.value("sr/denoise",0).toInt(),15);
    result.scale=validScale(settings.value("sr/scale",4.0).toDouble());
    return result;
}
void Configuration::save() const {
    QSettings s;
    s.setValue("sr/runtime",runtimeRoot); s.setValue("sr/worker",workerPath); s.setValue("sr/model",modelPath);
    s.setValue("sr/devices",devices); s.setValue("sr/displayProfile",displayProfile);
    s.setValue("sr/memoryMiB",memoryMiB); s.setValue("sr/animationMemoryMiB",animationMemoryMiB); s.setValue("sr/halo",halo); s.setValue("sr/denoise",denoise); s.setValue("sr/scale",scale);
}
QProcessEnvironment Configuration::environment() const {
    auto env=QProcessEnvironment::systemEnvironment();
    const auto ie=runtimeRoot+"/deployment_tools/inference_engine";
    env.insert("LD_LIBRARY_PATH",ie+"/lib/intel64:"+ie+"/external/tbb/lib:"+runtimeRoot+"/deployment_tools/ngraph/lib:"+runtimeRoot+"/opencv/lib");
    return env;
}
struct Controller::Job {
    std::shared_ptr<QTemporaryDir> directory=std::make_shared<QTemporaryDir>();
    QString id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    quint64 generation;
    Configuration configuration;
    QSize size;
    double cumulativeScale=1.0;
    QStringList steps;
    QString inputDescription;
    QString inputHash, sourceKey, error;
    QImage alpha;
    Profile profile;
    std::shared_ptr<const AnimationFrames> inputs, originals;
    std::shared_ptr<AnimationFrames> outputs=std::make_shared<AnimationFrames>();
    bool animated=false;
    int frame=0;
    quint64 budget=0,retainedBytes=0;
    QMap<QString,int> deviceTiles;
    double wallMs=0;
    QByteArray log;
    QJsonObject request;
    bool sent=false;
    QJsonObject completion;
    QStringList warnings;
    std::atomic<bool> cancelled{false};
};

Controller::Controller(MainWindow* window,QVGraphicsView* view)
    : QObject(window),window_(window),view_(view),configuration_(Configuration::load()) {
    setObjectName("srController");
    toolbar_=new QToolBar(QStringLiteral("超解像"),window); toolbar_->setObjectName("srToolbar");
    toolbar_->setMovable(false); toolbar_->setFloatable(false);
    window->addToolBar(Qt::TopToolBarArea,toolbar_);
    auto action=[&](const QString& text,const char* name,const QKeySequence& shortcut) {
        auto* a=toolbar_->addAction(text); a->setObjectName(name); a->setShortcut(shortcut);
        window->addAction(a); return a;
    };
    scale_=new QDoubleSpinBox(toolbar_); scale_->setObjectName("srScale");
    scale_->setRange(1.25,4.0); scale_->setDecimals(2); scale_->setSingleStep(0.25);
    scale_->setPrefix(QStringLiteral("倍率 ")); scale_->setSuffix(QStringLiteral(" 倍"));
    scale_->setValue(configuration_.scale);
    scale_->setToolTip(QStringLiteral("今回の倍率（1.25〜4）。モデルは4倍で推論し、指定サイズへ縮小します。"));
    toolbar_->addWidget(scale_);
    run_=action(QStringLiteral("元画像を超解像"),"srRun",QKeySequence("Ctrl+U"));
    repeat_=action(QStringLiteral("SRを重ねる"),"srRepeat",QKeySequence("Ctrl+Shift+U"));
    run_->setToolTip(QStringLiteral("無加工の元画像から指定倍率で超解像します（Ctrl+U）"));
    repeat_->setToolTip(QStringLiteral("前回のSR結果をさらに指定倍率で超解像します（Ctrl+Shift+U）。元画像は保持します。"));
    toggle_=action(QStringLiteral("SRを表示"),"srToggle",QKeySequence("Ctrl+Space"));
    save_=action(QStringLiteral("SRを保存…"),"srSave",QKeySequence("Ctrl+Shift+S"));
    cancel_=action(QStringLiteral("中止"),"srCancel",QKeySequence("Ctrl+."));
    settings_=action(QStringLiteral("SR設定…"),"srSettings",{});
    toolbar_->addSeparator();
    progress_=new QProgressBar(toolbar_); progress_->setObjectName("srProgress"); progress_->setMaximumWidth(110); progress_->hide();
    toolbar_->addWidget(progress_);
    status_=new StatusLabel(QStringLiteral("画像を開いてください"),toolbar_); status_->setObjectName("srStatus");
    status_->setMargin(6); status_->setMinimumWidth(0);
    status_->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
    status_->setFixedHeight(status_->fontMetrics().height()+12);
    status_->setTextFormat(Qt::PlainText); status_->setWordWrap(false); status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    toolbar_->addWidget(status_);
    connect(run_,&QAction::triggered,this,&Controller::start);
    connect(repeat_,&QAction::triggered,this,&Controller::startAgain);
    connect(scale_,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this](double value) {
        configuration_.scale=value; configuration_.save();
    });
    connect(cancel_,&QAction::triggered,this,&Controller::cancel);
    connect(toggle_,&QAction::triggered,this,&Controller::toggle);
    connect(save_,&QAction::triggered,this,&Controller::saveAs);
    connect(settings_,&QAction::triggered,this,&Controller::showSettings);
    connect(&view_->getImageCore(),&QVImageCore::sourceChanging,this,&Controller::invalidate);
    connect(view_,&QVGraphicsView::fileChanged,this,&Controller::sourceLoaded);
    watchdog_.setSingleShot(true);
    connect(&watchdog_,&QTimer::timeout,this,[this] {
        if(!job_) return;
        job_->error=QStringLiteral("処理が120秒間進みませんでした。接続とデバイス選択を確認してください。");
        const auto job=job_; stopWorker(); finish(job,job->error);
    });
    backendWatchdog_.setSingleShot(true);
    connect(&backendWatchdog_,&QTimer::timeout,this,[this] {
        const auto job=job_; stopWorker();
        const auto error=QStringLiteral("デバイスの準備が120秒以内に完了しませんでした");
        if(job) finish(job,error); else { setStatus(error); emit failed(error); }
    });
    QTimer::singleShot(0,this,[this] {
        ensureWorker();
        if(window_->windowHandle()) connect(window_->windowHandle(),&QWindow::screenChanged,this,[this]{display();});
    });
    animationTimer_.setSingleShot(true); animationTimer_.setTimerType(Qt::PreciseTimer);
    connect(&animationTimer_,&QTimer::timeout,this,&Controller::advanceAnimation);
    updateActions();
}
Controller::~Controller() {
    animationTimer_.stop(); watchdog_.stop();
    if(job_) job_->cancelled=true;
    if(exportCancelled_) *exportCancelled_=true;
    stopWorker();
}
void Controller::setStatus(QString text) {
    status_->setToolTip(text);
    text.replace("\r\n","\n"); text.replace('\r','\n');
    status_->setText(text.section('\n',0,0).replace('\t',' '));
}
void Controller::setFullscreen(bool fullscreen) { toolbar_->setVisible(!fullscreen); }
QString Controller::statusText() const { return status_->text(); }
qint64 Controller::backendPid() const { return worker_?worker_->processId():0; }
void Controller::setConfiguration(const Configuration& config) {
    if(isBusy()) return;
    const bool changed=config.runtimeRoot!=configuration_.runtimeRoot || config.workerPath!=configuration_.workerPath ||
        config.modelPath!=configuration_.modelPath || config.devices!=configuration_.devices;
    configuration_=config; configuration_.scale=validScale(config.scale);
    { const QSignalBlocker blocker(scale_); scale_->setValue(configuration_.scale); }
    configuration_.save(); display();
    if(changed) restartWorker(); else if(!worker_) ensureWorker();
}
void Controller::invalidate() {
    animationTimer_.stop(); animationPlaying_=false; animationIndex_=0; animationLoopsDone_=0; animationEnded_=false;
    originalAnimation_.reset(); resultAnimation_.reset(); originalDisplayFrames_.clear(); resultDisplayFrames_.clear(); animationDisplayProfile_.clear();
    ++generation_; sourceReady_=false; result_={}; showingSr_=false; resultSummary_.clear();
    resultScale_=1.0; resultSteps_.clear();
    cancel(); updateActions();
}
void Controller::sourceLoaded() {
    sourceReady_=!view_->getImageCore().getSourceImage().isNull();
    if(!job_) {
        setStatus(sourceReady_?QStringLiteral("元画像 · ")+view_->getImageCore().getSourceProfile().description:
                         QStringLiteral("画像を開いてください"));
        if(sourceReady_ && !view_->getImageCore().getSourceProfile().error.isEmpty()) setStatus(view_->getImageCore().getSourceProfile().error);
    }
    if(sourceReady_ && !view_->getCurrentFileDetails().isMovieLoaded && view_->getImageCore().getSourceProfile().error.isEmpty()) display();
    updateActions();
}
void Controller::updateActions() {
    const bool valid=sourceReady_ && view_->getImageCore().getSourceProfile().error.isEmpty();
    run_->setEnabled(valid && !isBusy()); repeat_->setEnabled(valid && !result_.isNull() && !isBusy());
    scale_->setEnabled(!isBusy()); settings_->setEnabled(!isBusy());
    cancel_->setEnabled((job_ && !job_->cancelled) || (exportCancelled_ && !*exportCancelled_));
    toggle_->setEnabled(!result_.isNull()); save_->setEnabled(!result_.isNull() && !isBusy());
    save_->setText(QStringLiteral("SRを保存…"));
    save_->setToolTip(hasAnimation()?QStringLiteral("GIF: 全フレームを保存。PNG/JPEG: 現在のフレームを保存。GIFは256色・透明/不透明に変換します。"):QStringLiteral("sRGB ICC付きでPNG/JPEGに保存します"));
    toggle_->setText(showingSr_?QStringLiteral("元画像を表示"):QStringLiteral("SRを表示"));
    emit stateChanged();
}
QByteArray Controller::displayIcc(QString* description) const {
    if(configuration_.displayProfile=="sRGB") { if(description)*description="sRGB"; return srgbProfile(); }
    if(!configuration_.displayProfile.isEmpty()) {
        QFile file(configuration_.displayProfile);
        if(file.size()>16*1024*1024 || !file.open(QIODevice::ReadOnly)) return {};
        if(description)*description=QFileInfo(file).fileName();
        return file.readAll();
    }
    const auto space=view_->getImageCore().detectDisplayColorSpace();
    if(description)*description=space.isValid()?QStringLiteral("自動画面ICC"):QStringLiteral("sRGB（画面ICC未取得）");
    return space.isValid()?space.iccProfile():srgbProfile();
}
void Controller::display(bool updateStatus) {
    if(!sourceReady_) return;
    QString description,error;
    const auto destination=displayIcc(&description);
    if(destination.isEmpty()) { setStatus(QStringLiteral("画面ICCを読み込めません。SR設定を確認してください。")); return; }
    const auto& source=hasAnimation()?(showingSr_?resultAnimation_->frames[animationIndex_]:originalAnimation_->frames[animationIndex_]):
        (showingSr_?result_:view_->getImageCore().getSourceImage());
    const Profile profile=showingSr_?Profile{srgbProfile(),QStringLiteral("sRGB"),{},false}:view_->getImageCore().getSourceProfile();
    QImage image;
    if(hasAnimation()) {
        if(animationDisplayProfile_!=destination) {
            originalDisplayFrames_.fill(QImage(),animationFrameCount()); resultDisplayFrames_.fill(QImage(),animationFrameCount());
            animationDisplayProfile_=destination;
        }
        auto& cache=showingSr_?resultDisplayFrames_:originalDisplayFrames_;
        if(cache[animationIndex_].isNull()) cache[animationIndex_]=convert(source,profile,destination,&error);
        image=cache[animationIndex_];
        result_=resultAnimation_->frames[animationIndex_];
    } else image=convert(source,profile,destination,&error);
    if(image.isNull()) { animationTimer_.stop(); animationPlaying_=false; setStatus(readFailure(error)); return; }
    view_->setDisplayImagePreservingView(image);
    if(updateStatus && !isBusy()) {
        QString summary=showingSr_?QStringLiteral("SR ×%1（%2回） · %3×%4").arg(scaleText(resultScale_)).arg(resultSteps_.size()).arg(result_.width()).arg(result_.height()):
                              QStringLiteral("元画像 · ")+view_->getImageCore().getSourceProfile().description;
        if(hasAnimation()) summary+=QStringLiteral(" · GIF %1フレーム · %2").arg(animationFrameCount()).arg(animationPlaying_?QStringLiteral("再生中"):QStringLiteral("一時停止"));
        setStatus(summary);
        status_->setToolTip(QStringLiteral("入力: %1\n表示: %2\nSR保存: sRGB\n%3")
            .arg(view_->getImageCore().getSourceProfile().description,description,resultSummary_));
    }
}
void Controller::start() { startJob(false); }
void Controller::startAgain() { startJob(true); }
void Controller::startJob(bool fromResult) {
    if(fromResult && result_.isNull()) return;
    if(isBusy() || !sourceReady_ || !view_->getImageCore().getSourceProfile().error.isEmpty()) return;
    const auto config=configuration_;
    if(!QFileInfo(config.workerPath).isExecutable() || !QFileInfo::exists(config.modelPath) ||
       !QFileInfo::exists(config.runtimeRoot+"/deployment_tools/inference_engine/lib/intel64/libmyriadPlugin.so")) {
        setStatus(QStringLiteral("推論プログラム / モデル / ランタイムが見つかりません。SR設定を確認してください。"));
        emit failed(status_->text()); return;
    }
    const auto path=view_->getCurrentFileDetails().fileInfo.absoluteFilePath();
    const bool animated=hasAnimation() || (view_->getCurrentFileDetails().isMovieLoaded && QImageReader::imageFormat(path)=="gif");
    if(!animated) {
        if(view_->getCurrentFileDetails().isMovieLoaded && view_->getLoadedMovie().state()==QMovie::Running) window_->pause();
        view_->getImageCore().freezeAnimationForSr(); display();
    }
    auto job=std::make_shared<Job>();
    job->generation=generation_; job->configuration=config; job->animated=animated;
    job->budget=quint64(animated?config.animationMemoryMiB:config.memoryMiB)*1024*1024;
    job->retainedBytes=retainedAnimationBytes()+quint64(result_.sizeInBytes())+quint64(view_->getImageCore().getSourceImage().sizeInBytes());
    job->cumulativeScale=(fromResult?resultScale_:1.0)*config.scale;
    job->steps=fromResult?resultSteps_:QStringList{}; job->steps<<scaleText(config.scale);
    job->inputDescription=fromResult?QStringLiteral("前回のSR（sRGB）"):QStringLiteral("元画像");
    job->profile=fromResult?Profile{srgbProfile(),QStringLiteral("sRGB"),{},false}:view_->getImageCore().getSourceProfile();
    if(!job->directory->isValid()) { setStatus(QStringLiteral("一時フォルダーを作成できません")); return; }
    if(hasAnimation()) {
        job->inputs=fromResult?resultAnimation_:originalAnimation_; job->originals=originalAnimation_;
    } else if(!animated) {
        auto input=std::make_shared<AnimationFrames>(); input->frames<< (fromResult?result_:view_->getImageCore().getSourceImage());
        job->inputs=input;
    }
    job_=job; progress_->setRange(0,0); progress_->show(); updateActions();
    if(job->inputs) { prepareFrame(job); return; }
    setStatus(QStringLiteral("GIF全フレームを読み込み中…"));
    auto* watcher=new QFutureWatcher<QString>(this);
    connect(watcher,&QFutureWatcher<QString>::finished,this,[this,job,watcher] {
        const auto error=watcher->result(); watcher->deleteLater();
        if(job!=job_) return;
        if(job->cancelled || job->generation!=generation_) { finish(job); return; }
        if(!error.isEmpty()) { finish(job,error); return; }
        prepareFrame(job);
    });
    watcher->setFuture(QtConcurrent::run([path,job] {
        const QFileInfo before(path); const auto revision=before.lastModified(); const auto bytes=before.size();
        QImageReader reader(path); reader.setAutoTransform(true);
        auto input=std::make_shared<AnimationFrames>();
        const int count=reader.imageCount();
        while(reader.canRead()) {
            if(job->cancelled) return QString();
            const QSize size=reader.size();
            if(!size.isValid() || size.width()>100000 || size.height()>100000) return QStringLiteral("GIFのフレーム寸法が不正です");
            const quint64 pixels=quint64(size.width())*size.height();
            const quint64 predicted=job->retainedBytes+input->bytes()*2+pixels*8+
                quint64(qRound(size.width()*job->configuration.scale))*qRound(size.height()*job->configuration.scale)*8*qMax(count,int(input->frames.size())+1)+
                pixels*320+4*192ULL*1024*1024;
            if(predicted>job->budget) return animationMemoryError(predicted,job->budget);
            auto image=reader.read();
            if(image.isNull()) return QStringLiteral("GIFフレームを読み込めません: ")+reader.errorString();
            if(!input->frames.isEmpty() && image.size()!=input->frames.first().size()) return QStringLiteral("GIFの合成後フレーム寸法が一致しません");
            input->frames<<image; input->delays<<qMax(1,reader.nextImageDelay());
        }
        if(input->frames.isEmpty() || (count>0 && count!=input->frames.size())) return QStringLiteral("GIFを最後のフレームまで読み込めませんでした");
        input->loops=reader.loopCount();
        const QFileInfo after(path);
        if(after.size()!=bytes || after.lastModified()!=revision) return QStringLiteral("GIFが読込中に変更されました。読み直してください");
        job->inputs=input; job->originals=input;
        return QString();
    }));
}
void Controller::prepareFrame(const std::shared_ptr<Job>& job) {
    if(job!=job_) return;
    if(job->cancelled || job->generation!=generation_) { finish(job); return; }
    const auto source=job->inputs->frames[job->frame]; job->size=source.size();
    const quint64 pixels=quint64(source.width())*source.height();
    const quint64 required=job->animated?animationEstimate(*job->inputs,job->configuration.scale,job->retainedBytes):
        pixels*320+job->retainedBytes+4*192ULL*1024*1024;
    if(required>job->budget) { finish(job,job->animated?animationMemoryError(required,job->budget):
        QStringLiteral("SRメモリー不足 · 見積り %1 MiB / 上限 %2 MiB\nSR設定で上限を変更してください。")
        .arg((required+1024*1024-1)/(1024*1024)).arg(job->configuration.memoryMiB)); return; }
    job->id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    job->sent=false; job->request={}; job->completion={}; job->alpha={};
    setStatus(job->animated?QStringLiteral("フレーム %1/%2 · sRGB入力を準備中…").arg(job->frame+1).arg(job->inputs->frames.size()):QStringLiteral("sRGB入力を準備中…"));
    auto* watcher=new QFutureWatcher<QString>(this);
    connect(watcher,&QFutureWatcher<QString>::finished,this,[this,job,watcher] {
        const auto error=watcher->result(); watcher->deleteLater();
        if(job!=job_) return;
        if(job->cancelled || job->generation!=generation_) { finish(job); return; }
        if(!error.isEmpty()) { finish(job,error); return; }
        launch(job);
    });
    watcher->setFuture(QtConcurrent::run([source,job] {
        // The worker refuses to overwrite files. The preceding frame is already
        // decoded into memory, so keep disk use bounded by reusing its output path.
        const auto output=job->directory->filePath("output.png");
        if(QFileInfo::exists(output) && !QFile::remove(output)) return QStringLiteral("前フレームの一時出力を削除できません");
        QString error;
        auto input=toSrgb(source,job->profile,&error);
        if(input.isNull() || job->cancelled) return error;
        if(transparent(input)) {
            job->alpha=input.convertToFormat(QImage::Format_Alpha8);
            for(int y=0;y<input.height();++y) {
                uchar* row=input.scanLine(y);
                for(int x=0;x<input.width();++x) if(row[x*4+3]==0) row[x*4]=row[x*4+1]=row[x*4+2]=0;
            }
        }
        const auto path=job->directory->filePath("input.png");
        if(!input.convertToFormat(QImage::Format_RGB888).save(path,"PNG")) return QStringLiteral("SR入力PNGを保存できません");
        job->inputHash=QString::fromLatin1(hash(path));
        job->sourceKey=QString::fromLatin1(QCryptographicHash::hash(job->inputHash.toUtf8()+job->profile.icc,QCryptographicHash::Sha256).toHex());
        return QString();
    }));
}

void Controller::stopWorker() {
    backendWatchdog_.stop(); backendReady_=false; stoppingWorker_=false;
    auto process=worker_; worker_=nullptr;
    if(process) {
        process->disconnect(this);
        process->write(QJsonDocument(QJsonObject{{"protocol_version",1},{"command","shutdown"},{"job_id",sessionId_}}).toJson(QJsonDocument::Compact)+"\n");
        process->closeWriteChannel();
        if(process->state()!=QProcess::NotRunning && !process->waitForFinished(10000)) {
            process->kill(); process->waitForFinished(2000);
        }
        process->deleteLater();
    }
    workerBuffer_.clear();
}
void Controller::restartWorker() {
    if(!worker_) { ensureWorker(); return; }
    if(stoppingWorker_) return;
    const auto process=worker_;
    process->disconnect(this); backendWatchdog_.stop(); backendReady_=false; stoppingWorker_=true;
    const auto restart=[this,process] {
        if(worker_!=process) return;
        worker_=nullptr; process->deleteLater();
        QTimer::singleShot(1000,this,[this]{stoppingWorker_=false; ensureWorker();});
    };
    if(process->state()==QProcess::NotRunning) { restart(); return; }
    connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,restart);
    setStatus(QStringLiteral("デバイスを解放して切り替えています…"));
    process->write(QJsonDocument(QJsonObject{{"protocol_version",1},{"command","shutdown"},{"job_id",sessionId_}}).toJson(QJsonDocument::Compact)+"\n");
    process->closeWriteChannel();
    // Native USB close/reset can take several seconds across four sticks. Give it
    // time to finish and keep the GUI responsive while awaiting the next session.
    QTimer::singleShot(10000,process,[process] { if(process && process->state()!=QProcess::NotRunning) process->kill(); });
}
void Controller::ensureWorker() {
    if(worker_ || stoppingWorker_) return;
    const auto config=configuration_;
    if(!QFileInfo(config.workerPath).isExecutable() || !QFileInfo::exists(config.modelPath) ||
       !QFileInfo::exists(config.runtimeRoot+"/deployment_tools/inference_engine/lib/intel64/libmyriadPlugin.so")) return;
    auto* process=new QProcess(this); worker_=process;
    process->setObjectName("srWorker");
    sessionId_=QUuid::createUuid().toString(QUuid::WithoutBraces);
    workerLog_.clear(); workerBuffer_.clear(); backendWarnings_.clear(); backendDevices_.clear(); backendReady_=false;
    process->setProcessEnvironment(config.environment());
    connect(process,&QProcess::started,this,[this,process,config] {
        QString runtimeDir=QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        if(runtimeDir.isEmpty()) runtimeDir=QDir::tempPath()+"/qviewsr-"+qEnvironmentVariable("USER");
        QDir().mkpath(runtimeDir);
        const QJsonObject init{{"protocol_version",1},{"command","configure"},{"job_id",sessionId_},
            {"model_xml",QFileInfo(config.modelPath).absoluteFilePath()},{"devices",config.devices},
            {"lock_file",runtimeDir+"/qviewsr-devices.lock"}};
        process->write(QJsonDocument(init).toJson(QJsonDocument::Compact)+"\n");
    });
    connect(process,&QProcess::readyReadStandardOutput,this,&Controller::readEvents);
    connect(process,&QProcess::readyReadStandardError,this,[this,process] {
        workerLog_+=process->readAllStandardError(); workerLog_=workerLog_.right(65536);
        if(job_) job_->log=workerLog_;
    });
    connect(process,&QProcess::errorOccurred,this,[this,process](QProcess::ProcessError error) {
        if(error==QProcess::FailedToStart) {
            const auto message=QStringLiteral("推論エンジンを起動できません: ")+process->errorString();
            const auto job=job_; stopWorker();
            if(job) finish(job,message); else { setStatus(message); emit failed(message); }
        }
    });
    connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this,process](int,QProcess::ExitStatus) {
        if(worker_!=process) return;
        readEvents();
        if(worker_!=process) return;
        backendWatchdog_.stop(); backendReady_=false; worker_=nullptr; process->deleteLater();
        const auto error=QStringLiteral("推論エンジンが終了しました。再実行すると初期化し直します。 ")+QString::fromUtf8(workerLog_.right(1000));
        if(job_) finish(job_,job_->cancelled?QString():error);
        else { setStatus(error); emit failed(error); }
    });
    setStatus(QStringLiteral("デバイスを初期化中…（次の画像から再利用）"));
    backendWatchdog_.start(120000); process->start(config.workerPath,{"--serve"});
}
void Controller::launch(const std::shared_ptr<Job>& job) {
    job->request={{"protocol_version",1},{"command","run"},{"job_id",job->id},{"source_key",job->sourceKey},
        {"input",job->directory->filePath("input.png")},{"output",job->directory->filePath("output.png")},
        {"model_xml",QFileInfo(job->configuration.modelPath).absoluteFilePath()},
        {"input_sha256",job->inputHash},{"width",job->size.width()},{"height",job->size.height()},
        {"halo",job->configuration.halo},{"denoise",job->configuration.denoise},{"devices",job->configuration.devices},
        {"max_memory_bytes",double(job->budget)}};
    ensureWorker(); dispatch();
}
void Controller::dispatch() {
    if(!job_ || job_->cancelled || job_->sent || job_->request.isEmpty()) return;
    if(stoppingWorker_) { setStatus(QStringLiteral("デバイスの切替完了を待っています…")); return; }
    if(!worker_) { finish(job_,QStringLiteral("推論エンジンを起動できません。SR設定を確認してください。")); return; }
    if(!backendReady_) { setStatus(QStringLiteral("デバイスの初期化完了を待っています…")); return; }
    job_->sent=true;
    worker_->write(QJsonDocument(job_->request).toJson(QJsonDocument::Compact)+"\n");
    watchdog_.start(120000);
    const auto frame=job_->animated?QStringLiteral("GIF %1/%2フレーム · ").arg(job_->frame+1).arg(job_->inputs->frames.size()):QString();
    setStatus(frame+QStringLiteral("準備済みデバイスで処理中…"));
}
void Controller::acceptResult(const std::shared_ptr<Job>& job) {
    watchdog_.stop();
    if(job->cancelled || job->generation!=generation_) { finish(job); return; }
    setStatus(QStringLiteral("SR画像を確認中…"));
    auto* watcher=new QFutureWatcher<QImage>(this);
    connect(watcher,&QFutureWatcher<QImage>::finished,this,[this,job,watcher] {
        auto result=watcher->result(); watcher->deleteLater();
        if(job!=job_) return;
        if(job->cancelled || job->generation!=generation_) { finish(job); return; }
        if(result.isNull()) { finish(job,job->error); return; }
        job->outputs->frames<<result;
        job->wallMs+=job->completion.value("wall_ms").toDouble();
        for(const auto& value:job->completion.value("devices").toArray()) {
            const auto device=value.toObject(); job->deviceTiles[device.value("id").toString()]+=device.value("tiles").toInt();
        }
        if(++job->frame<job->inputs->frames.size()) {
            QTimer::singleShot(0,this,[this,job]{prepareFrame(job);}); return;
        }
        publishResult(job);
    });
    watcher->setFuture(QtConcurrent::run([job] {
        auto fail=[&](const QString& error) { job->error=error; return QImage(); };
        const auto completion=job->completion;
        const QSize expected(job->size.width()*4,job->size.height()*4);
        if(completion.value("source_key").toString()!=job->sourceKey || completion.value("color_space").toString()!="sRGB" ||
           completion.value("width").toInt()!=expected.width() || completion.value("height").toInt()!=expected.height() || completion.value("denoise").toInt()!=job->configuration.denoise)
            return fail(QStringLiteral("推論エンジンの結果情報が一致しません"));
        const auto path=job->directory->filePath("output.png");
        if(hash(path)!=completion.value("output_sha256").toString().toLatin1()) return fail(QStringLiteral("出力のchecksumが一致しません"));
        QImageReader reader(path,"PNG");
        if(reader.size()!=expected) return fail(QStringLiteral("出力寸法が一致しません"));
        auto image=reader.read();
        if(image.isNull()) return fail(reader.errorString());
        if(!job->alpha.isNull()) image.setAlphaChannel(job->alpha.scaled(expected,Qt::IgnoreAspectRatio,Qt::SmoothTransformation));
        // The pinned network always predicts 4x. Resize that SR result (including
        // alpha) in premultiplied form to avoid fringes along transparent edges.
        const QSize target(qRound(job->size.width()*job->configuration.scale),qRound(job->size.height()*job->configuration.scale));
        if(target!=expected) image=image.convertToFormat(QImage::Format_ARGB32_Premultiplied).scaled(target,Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
        if(image.isNull()) return fail(QStringLiteral("指定倍率の画像用メモリーを確保できません"));
        image.setColorSpace(QColorSpace::SRgb); return image;
    }));
}
void Controller::publishResult(const std::shared_ptr<Job>& job) {
    result_=job->outputs->frames.first(); showingSr_=true; resultScale_=job->cumulativeScale; resultSteps_=job->steps;
    QStringList stats;
    for(auto i=job->deviceTiles.cbegin();i!=job->deviceTiles.cend();++i) stats<<QStringLiteral("%1: %2タイル").arg(i.key()).arg(i.value());
    resultSummary_=QStringLiteral("倍率: %1（元画像比 ×%2） / 入力: %3\n").arg(job->steps.join(" × "),scaleText(job->cumulativeScale),job->inputDescription)
        +stats.join(" / ")+QStringLiteral("\nノイズ低減: %1 / 処理: %2秒（初期化除く）").arg(job->configuration.denoise).arg(job->wallMs/1000,0,'f',2);
    if(job->animated) {
        animationTimer_.stop();
        if(!hasAnimation()) animationSpeed_=view_->getLoadedMovie().speed();
        view_->getImageCore().freezeAnimationForSr();
        job->outputs->delays=job->inputs->delays; job->outputs->loops=job->inputs->loops;
        originalAnimation_=job->originals; resultAnimation_=job->outputs;
        animationIndex_=0; animationLoopsDone_=0; animationPlaying_=true; animationEnded_=false;
        animationDisplayProfile_.clear(); originalDisplayFrames_.clear(); resultDisplayFrames_.clear();
        resultSummary_+=QStringLiteral("\nGIF: %1フレーム / 再生: %2 / GIF保存は全フレーム、PNG・JPEGは現在のフレーム").arg(animationFrameCount())
            .arg(animationLoopCount()<0?QStringLiteral("無限ループ"):QStringLiteral("%1回").arg(animationLoopCount()+1));
    }
    if(!job->warnings.isEmpty()) resultSummary_+="\n"+job->warnings.join("\n");
    finish(job); display(); if(hasAnimation()) scheduleAnimation(); emit resultReady();
}
int Controller::animationFrameCount() const { return hasAnimation()?resultAnimation_->frames.size():0; }
int Controller::animationDelay(int frame) const { return hasAnimation()?resultAnimation_->delays.value(frame):0; }
int Controller::animationLoopCount() const { return hasAnimation()?resultAnimation_->loops:0; }
quint64 Controller::retainedAnimationBytes() const {
    quint64 bytes=0;
    if(originalAnimation_) bytes+=originalAnimation_->bytes();
    if(resultAnimation_) bytes+=resultAnimation_->bytes();
    for(const auto& frame:originalDisplayFrames_) bytes+=quint64(frame.sizeInBytes());
    for(const auto& frame:resultDisplayFrames_) bytes+=quint64(frame.sizeInBytes());
    return bytes;
}
void Controller::scheduleAnimation(int renderMs) {
    animationTimer_.stop();
    if(hasAnimation() && animationPlaying_ && animationSpeed_>0)
        animationTimer_.start(qMax(1,int(qint64(animationDelay(animationIndex_))*100/animationSpeed_)-renderMs));
}
void Controller::advanceAnimation() {
    if(!hasAnimation() || !animationPlaying_) return;
    if(animationIndex_+1==animationFrameCount()) {
        if(animationLoopCount()>=0 && animationLoopsDone_>=animationLoopCount()) {
            animationPlaying_=false; animationEnded_=true; display(); updateActions(); return;
        }
        if(animationLoopCount()>=0) ++animationLoopsDone_;
    }
    animationIndex_=(animationIndex_+1)%animationFrameCount();
    QElapsedTimer rendering; rendering.start(); display(false);
    emit animationFrameChanged(animationIndex_); scheduleAnimation(int(rendering.elapsed()));
}
void Controller::setAnimationPaused(bool paused) {
    if(!hasAnimation()) return;
    if(!paused && animationEnded_) {
        animationIndex_=0; animationLoopsDone_=0; animationEnded_=false;
    }
    animationPlaying_=!paused; display(); scheduleAnimation(); updateActions();
}
void Controller::stepAnimation() {
    if(!hasAnimation()) return;
    animationPlaying_=false; animationTimer_.stop();
    animationIndex_=(animationIndex_+1)%animationFrameCount(); animationLoopsDone_=0; animationEnded_=false;
    display(); emit animationFrameChanged(animationIndex_); updateActions();
}
void Controller::setAnimationSpeed(int percent) {
    animationSpeed_=qBound(0,percent,1000); scheduleAnimation();
}
void Controller::readEvents() {
    if(!worker_) return;
    workerBuffer_+=worker_->readAllStandardOutput();
    auto protocolFailure=[this] {
        const auto job=job_; stopWorker();
        const auto error=QStringLiteral("推論エンジンの通信形式が不正です");
        if(job) finish(job,error); else { setStatus(error); emit failed(error); }
    };
    while(workerBuffer_.contains('\n')) {
        const auto newline=workerBuffer_.indexOf('\n');
        const auto line=workerBuffer_.left(newline); workerBuffer_.remove(0,newline+1);
        QJsonParseError error;
        const auto message=QJsonDocument::fromJson(line,&error).object();
        if(line.size()>65536 || error.error!=QJsonParseError::NoError || message.value("protocol_version").toInt()!=1) {
            protocolFailure(); return;
        }
        const auto id=message.value("job_id").toString(), event=message.value("event").toString();
        if(id==sessionId_) {
            if(event=="session_ready") {
                backendReady_=true; backendWatchdog_.stop();
                backendDevices_.clear();
                for(const auto& id:message.value("devices").toArray()) backendDevices_<<id.toString();
                if(!job_) setStatus(QStringLiteral("デバイス準備完了 · %1台").arg(message.value("devices").toArray().size()));
                emit backendInitialized(); dispatch();
            } else if(event=="device_error") {
                backendWarnings_<<message.value("device").toString()+": "+message.value("message").toString();
            } else if(event=="error") {
                const auto job=job_; stopWorker();
                const auto text=QStringLiteral("デバイスを準備できません: ")+message.value("message").toString()+"\n"+backendWarnings_.join("\n");
                if(job) finish(job,text); else { setStatus(text); emit failed(text); }
                return;
            }
            continue;
        }
        const auto job=job_;
        if(!job || id!=job->id) continue; // stale terminal events cannot affect the current image
        if(event=="error") {
            if(message.value("reset_session").toBool()) stopWorker();
            finish(job,job->cancelled?QString():QStringLiteral("超解像に失敗しました: ")+message.value("message").toString());
        }
        else if(event=="cancelled") { job->cancelled=true; finish(job); }
        else if(event=="completed") {
            if(!job->completion.isEmpty()) { protocolFailure(); return; }
            job->completion=message; acceptResult(job);
        } else if(!job->cancelled) {
            if(event=="device_error") job->warnings<<message.value("device").toString()+": "+message.value("message").toString();
            else if(event=="preprocessing") setStatus(QStringLiteral("ノイズを低減中…（強さ %1）").arg(message.value("denoise").toInt()));
            else if(event=="progress") {
                const int total=message.value("total").toInt(),done=message.value("completed").toInt();
                if(total<1 || done<0 || done>total) { protocolFailure(); return; }
                if(job->animated) {
                    progress_->setRange(0,1000); progress_->setValue(int(1000*(job->frame+double(done)/total)/job->inputs->frames.size()));
                    setStatus(QStringLiteral("GIF %1/%2フレーム · タイル %3/%4 · %5").arg(job->frame+1).arg(job->inputs->frames.size()).arg(done).arg(total).arg(message.value("device").toString()));
                } else {
                    progress_->setRange(0,total); progress_->setValue(done);
                    setStatus(QStringLiteral("処理中 %1/%2 · %3").arg(done).arg(total).arg(message.value("device").toString()));
                }
            }
            watchdog_.start(120000);
        }
    }
    if(workerBuffer_.size()>65536) protocolFailure();
}
void Controller::finish(std::shared_ptr<Job> job,const QString& error) {
    if(job!=job_) return;
    const bool cancelled=job->cancelled;
    job_.reset(); watchdog_.stop(); progress_->hide();
    if(!error.isEmpty()) { setStatus(readFailure(error)); status_->setToolTip(error+"\n"+QString::fromUtf8(job->log)); emit failed(error); }
    else if(cancelled) setStatus(sourceReady_?QStringLiteral("中止しました · ")+(showingSr_?QStringLiteral("前回のSRを保持"):QStringLiteral("元画像")):QStringLiteral("画像を読み込み中…"));
    updateActions();
}
void Controller::cancel() {
    if(exportCancelled_) { *exportCancelled_=true; setStatus(QStringLiteral("GIF保存を中止中…")); updateActions(); }
    if(!job_) return;
    const auto job=job_;
    job->cancelled=true; watchdog_.stop(); setStatus(QStringLiteral("中止中…")); updateActions();
    if(job->sent && worker_ && job->completion.isEmpty()) {
        worker_->write(QJsonDocument(QJsonObject{{"protocol_version",1},{"command","cancel"},{"job_id",job->id}}).toJson(QJsonDocument::Compact)+"\n");
        QTimer::singleShot(15000,this,[this,job] {
            if(job_==job) { stopWorker(); finish(job); }
        });
    } else if(!job->request.isEmpty() && !job->sent) finish(job);
    // Preparing/decoding futures retain their job and finish when the task exits.
}
void Controller::toggle() { if(result_.isNull()) return; showingSr_=!showingSr_; display(); updateActions(); }
bool Controller::saveResult(const QString& path,const QByteArray& format,QString* error) {
    if(result_.isNull()) { if(error)*error=QStringLiteral("保存するSR画像がありません"); return false; }
    const auto original=view_->getCurrentFileDetails().fileInfo;
    if(QFileInfo(path).absoluteFilePath()==original.absoluteFilePath() ||
       (!QFileInfo(path).canonicalFilePath().isEmpty() && QFileInfo(path).canonicalFilePath()==original.canonicalFilePath())) {
        if(error)*error=QStringLiteral("元画像への上書きはできません"); return false;
    }
    return writeImage(view_->getImageCore().matchCurrentRotation(result_),path,format,Qt::white,error);
}
bool Controller::saveAnimation(const QString& path,QString* error) {
    auto fail=[error](const QString& message) { if(error) *error=message; return false; };
    if(!hasAnimation() || isBusy()) return fail(QStringLiteral("保存できる超解像GIFがありません、または処理中です"));
    const auto original=view_->getCurrentFileDetails().fileInfo;
    const QFileInfo destination(path);
    if(destination.absoluteFilePath()==original.absoluteFilePath() ||
       (!destination.canonicalFilePath().isEmpty() && destination.canonicalFilePath()==original.canonicalFilePath()))
        return fail(QStringLiteral("元画像への上書きはできません"));
    // Capture immutable frames and rotation. The exporter must never read widgets
    // or the changing playback index from its background thread.
    const auto animation=resultAnimation_;
    const auto rotation=view_->getImageCore().getCurrentRotation();
    const auto generation=generation_;
    auto cancelled=std::make_shared<std::atomic_bool>(false); exportCancelled_=cancelled;
    auto* watcher=new QFutureWatcher<QString>(this);
    saving_=true; progress_->setRange(0,animation->frames.size()*2); progress_->setValue(0); progress_->show();
    updateActions(); setStatus(QStringLiteral("GIF保存用の色を調整中…"));
    connect(watcher,&QFutureWatcher<QString>::progressValueChanged,this,[this,generation,animation](int value) {
        if(generation!=generation_ || !exportCancelled_ || *exportCancelled_) return;
        progress_->setValue(value);
        const int count=animation->frames.size();
        setStatus(value<=count?QStringLiteral("GIF減色準備 %1/%2フレーム").arg(value).arg(count):
                              QStringLiteral("GIF保存中 %1/%2フレーム").arg(value-count).arg(count));
    });
    connect(watcher,&QFutureWatcher<QString>::finished,this,[this,watcher,cancelled,generation,path] {
        const auto error=watcher->result(); watcher->deleteLater();
        saving_=false; exportCancelled_.reset(); progress_->hide(); updateActions();
        if(error.isEmpty()) {
            if(generation==generation_) setStatus(QStringLiteral("GIFを保存しました: ")+QFileInfo(path).fileName());
            emit animationSaved(path);
        } else if(generation==generation_) {
            setStatus(error);
            if(!*cancelled) emit failed(error);
        }
    });
    watcher->setFuture(QtConcurrent::run([animation,rotation,path,cancelled](QPromise<QString>& promise) {
        promise.setProgressRange(0,animation->frames.size()*2);
        promise.addResult(writeGif(path,animation->frames,animation->delays,animation->loops,rotation,*cancelled,
            [&promise](int value) { promise.setProgressValue(value); }));
    }));
    return true;
}
void Controller::saveAs() { saveFrameAs(false,true); }
void Controller::saveDisplayedFrameAs() { saveFrameAs(hasAnimation() && !showingSr_); }
void Controller::saveFrameAs(bool originalFrame,bool offerAnimation) {
    if(result_.isNull() || isBusy()) return;
    offerAnimation=offerAnimation && hasAnimation();
    const auto original=view_->getCurrentFileDetails().fileInfo;
    if(hasAnimation()) setAnimationPaused(true);
    QString error;
    const auto frame=originalFrame?toSrgb(originalAnimation_->frames[animationIndex_],view_->getImageCore().getSourceProfile(),&error):result_;
    if(frame.isNull()) { setStatus(error); return; }
    const auto image=view_->getImageCore().matchCurrentRotation(frame);
    const auto suffix=originalFrame?QStringLiteral("_original"):"_sr"+scaleText(resultScale_).replace('.','p')+"x_"+QString::number(resultSteps_.size())+"pass";
    const auto title=offerAnimation?QStringLiteral("超解像GIFを保存（全フレーム・元の再生間隔）"):
        originalFrame?QStringLiteral("現在の元画像フレームを保存（sRGB ICC付き）"):QStringLiteral("SR画像を保存（sRGB ICC付き）");
    const auto filters=offerAnimation?QStringLiteral("GIFアニメーション（全フレーム） (*.gif);;PNG（現在のフレーム） (*.png);;JPEG（現在のフレーム） (*.jpg *.jpeg)"):QStringLiteral("PNG (*.png);;JPEG (*.jpg *.jpeg)");
    QFileDialog dialog(window_,title,original.absolutePath(),filters);
    dialog.setAcceptMode(QFileDialog::AcceptSave); dialog.setDefaultSuffix(offerAnimation?"gif":"png");
    dialog.selectFile(original.completeBaseName()+suffix+(hasAnimation() && !offerAnimation?QStringLiteral("_frame%1").arg(animationIndex_+1,5,10,QLatin1Char('0')):QString()));
    connect(&dialog,&QFileDialog::filterSelected,&dialog,[&dialog](const QString& filter) {
        dialog.setDefaultSuffix(filter.startsWith("GIF")?"gif":filter.startsWith("JPEG")?"jpg":"png");
    });
    if(dialog.exec()!=QDialog::Accepted || dialog.selectedFiles().isEmpty()) return;
    const auto path=dialog.selectedFiles().first();
    const auto selected=dialog.selectedNameFilter();
    const QByteArray format=selected.startsWith("GIF")?"gif":selected.startsWith("JPEG")?"jpeg":"png";
    if(format=="gif") {
        if(!saveAnimation(path,&error)) setStatus(error);
        return;
    }
    if(QFileInfo(path).absoluteFilePath()==original.absoluteFilePath() ||
       (!QFileInfo(path).canonicalFilePath().isEmpty() && QFileInfo(path).canonicalFilePath()==original.canonicalFilePath())) {
        setStatus(QStringLiteral("元画像への上書きはできません")); return;
    }
    QColor background=Qt::white;
    if(format=="jpeg" && transparent(image)) {
        background=QColorDialog::getColor(Qt::white,window_,QStringLiteral("JPEGの透明部分の背景色"));
        if(!background.isValid()) return;
    }
    const auto generation=generation_;
    auto* watcher=new QFutureWatcher<QString>(this);
    saving_=true; updateActions(); setStatus(QStringLiteral("画像を保存中…"));
    connect(watcher,&QFutureWatcher<QString>::finished,this,[this,watcher,generation,path] {
        const auto error=watcher->result(); watcher->deleteLater(); saving_=false; updateActions();
        if(generation==generation_) setStatus(error.isEmpty()?QStringLiteral("保存しました: ")+QFileInfo(path).fileName():error);
    });
    watcher->setFuture(QtConcurrent::run([image,path,format,background] {
        QString error; writeImage(image,path,format,background,&error); return error;
    }));
}
void Controller::showSettings() {
    if(isBusy()) return;
    QDialog dialog(window_); dialog.setWindowTitle(QStringLiteral("qViewSR 設定")); dialog.setMinimumWidth(700);
    auto* layout=new QVBoxLayout(&dialog); auto* form=new QFormLayout; layout->addLayout(form);
    auto field=[&](const QString& label,const QString& value,bool directory) {
        auto* row=new QWidget(&dialog); auto* line=new QHBoxLayout(row); line->setContentsMargins(0,0,0,0);
        auto* edit=new QLineEdit(value,row); auto* button=new QPushButton(QStringLiteral("参照…"),row); line->addWidget(edit); line->addWidget(button);
        connect(button,&QPushButton::clicked,&dialog,[&,edit,directory] {
            auto selected=directory?QFileDialog::getExistingDirectory(&dialog,QStringLiteral("フォルダー"),edit->text()):
                QFileDialog::getOpenFileName(&dialog,QStringLiteral("ファイル"),edit->text());
            if(!selected.isEmpty()) edit->setText(selected);
        });
        form->addRow(label,row); return edit;
    };
    auto* runtime=field(QStringLiteral("OpenVINOランタイム"),configuration_.runtimeRoot,true);
    auto* worker=field(QStringLiteral("推論プログラム"),configuration_.workerPath,false);
    auto* model=field(QStringLiteral("超解像モデル（XML）"),configuration_.modelPath,false);
    auto* devices=new QComboBox(&dialog); devices->setEditable(true);
    devices->addItem(QStringLiteral("全NCS / NCS2（最大4本）"),"all");
    devices->addItem(QStringLiteral("NCS2のみ"),"ncs2"); devices->addItem(QStringLiteral("初代NCSのみ"),"ncs");
    devices->addItem(QStringLiteral("CPU（比較用）"),"CPU");
    const int index=devices->findData(configuration_.devices);
    if(index>=0) devices->setCurrentIndex(index); else devices->setEditText(configuration_.devices);
    form->addRow(QStringLiteral("使用デバイス"),devices);
    auto* scan=new QPushButton(QStringLiteral("接続デバイスを確認"),&dialog); form->addRow({},scan);
    auto* scanResult=new QLabel(&dialog); scanResult->setObjectName("srDeviceList"); scanResult->setWordWrap(true); scanResult->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(scanResult);
    connect(scan,&QPushButton::clicked,&dialog,[&,scanResult] {
        const auto held=(backendReady_ && runtime->text()==configuration_.runtimeRoot && worker->text()==configuration_.workerPath)?backendDevices_:QStringList{};
        auto* process=new QProcess(&dialog); auto conf=configuration_; conf.runtimeRoot=runtime->text();
        process->setProcessEnvironment(conf.environment()); scan->setEnabled(false); scanResult->setText(QStringLiteral("確認中…"));
        connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),&dialog,[=](int,QProcess::ExitStatus) {
            const auto lines=process->readAllStandardOutput().split('\n'); QStringList ids=held;
            for(const auto& line:lines) {
                auto object=QJsonDocument::fromJson(line).object();
                for(const auto& id:object.value("devices").toArray()) if(!ids.contains(id.toString())) ids<<id.toString();
                if(object.value("event")=="error") ids<<object.value("message").toString();
            }
            scanResult->setText(ids.isEmpty()?QStringLiteral("NCSが見つかりません"):ids.join("\n")); scan->setEnabled(true); process->deleteLater();
        });
        connect(process,&QProcess::errorOccurred,&dialog,[=](QProcess::ProcessError) { scanResult->setText(process->errorString()); scan->setEnabled(true); });
        QTimer::singleShot(15000,process,[process]{if(process->state()!=QProcess::NotRunning)process->kill();});
        process->start(worker->text(),{"--list"});
    });
    auto* displayMode=new QComboBox(&dialog);
    displayMode->addItems({QStringLiteral("自動（X11画面ICC / 未取得ならsRGB）"),QStringLiteral("sRGB"),QStringLiteral("手動ICC")});
    displayMode->setCurrentIndex(configuration_.displayProfile.isEmpty()?0:configuration_.displayProfile=="sRGB"?1:2);
    form->addRow(QStringLiteral("表示プロファイル"),displayMode);
    auto* icc=field(QStringLiteral("手動ICCファイル"),configuration_.displayProfile=="sRGB"?QString():configuration_.displayProfile,false);
    auto* memory=new QSpinBox(&dialog); memory->setRange(1024,16384); memory->setSuffix(" MiB"); memory->setValue(configuration_.memoryMiB); form->addRow(QStringLiteral("SRメモリー上限"),memory);
    auto* animationMemory=new QSpinBox(&dialog); animationMemory->setRange(1024,65536); animationMemory->setSuffix(" MiB");
    animationMemory->setValue(configuration_.animationMemoryMiB); form->addRow(QStringLiteral("アニメーション用メモリー上限"),animationMemory);
    auto* halo=new QSpinBox(&dialog); halo->setRange(0,64); halo->setValue(configuration_.halo); form->addRow(QStringLiteral("タイル境界の余白（画素）"),halo);
    auto* noise=new QSpinBox(&dialog); noise->setObjectName("srDenoise");
    noise->setRange(0,15); noise->setSpecialValueText(QStringLiteral("なし")); noise->setValue(configuration_.denoise);
    noise->setToolTip(QStringLiteral("3: 弱 / 6: 標準 / 10: 強。JPEGのノイズをSR前に低減します。強くすると細部も滑らかになります。"));
    form->addRow(QStringLiteral("ノイズ低減（SR前）"),noise);
    auto* note=new QLabel(QStringLiteral("モデルは4倍で推論し、指定倍率に縮小してsRGBで保存します。画面ICCは保存画像に適用しません。\n個体を指定する場合はIDをカンマ区切りで入力できます。世代混在時の画質は比較検証中です。\nノイズ低減は0=なし、3=弱、6=標準、10=強。強くすると細部も滑らかになります。"),&dialog);
    note->setWordWrap(true); layout->addWidget(note);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog); layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept); connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if(dialog.exec()!=QDialog::Accepted) return;
    auto conf=configuration_;
    conf.runtimeRoot=runtime->text(); conf.workerPath=worker->text(); conf.modelPath=model->text();
    conf.devices=(devices->currentIndex()>=0 && devices->currentText()==devices->itemText(devices->currentIndex()))?devices->currentData().toString():devices->currentText().trimmed();
    conf.displayProfile=displayMode->currentIndex()==0?QString():displayMode->currentIndex()==1?QStringLiteral("sRGB"):icc->text();
    conf.memoryMiB=memory->value(); conf.animationMemoryMiB=animationMemory->value(); conf.halo=halo->value(); conf.denoise=noise->value(); setConfiguration(conf);
}
}
