// SPDX-License-Identifier: GPL-3.0-or-later
#include "sr_controller.h"
#include "color_pipeline.h"
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
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QToolBar>
#include <QUuid>
#include <QVBoxLayout>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>
#include <atomic>

namespace Sr {
namespace {
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
    result.halo=qBound(0,settings.value("sr/halo",16).toInt(),64);
    result.denoise=qBound(0,settings.value("sr/denoise",0).toInt(),15);
    return result;
}
void Configuration::save() const {
    QSettings s;
    s.setValue("sr/runtime",runtimeRoot); s.setValue("sr/worker",workerPath); s.setValue("sr/model",modelPath);
    s.setValue("sr/devices",devices); s.setValue("sr/displayProfile",displayProfile);
    s.setValue("sr/memoryMiB",memoryMiB); s.setValue("sr/halo",halo); s.setValue("sr/denoise",denoise);
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
    QString inputHash, sourceKey, error;
    QImage alpha;
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
    run_=action(QStringLiteral("超解像 ×4"),"srRun",QKeySequence("Ctrl+U"));
    toggle_=action(QStringLiteral("SRを表示"),"srToggle",QKeySequence("Ctrl+Space"));
    save_=action(QStringLiteral("SRを保存…"),"srSave",QKeySequence("Ctrl+Shift+S"));
    cancel_=action(QStringLiteral("中止"),"srCancel",QKeySequence("Ctrl+."));
    settings_=action(QStringLiteral("SR設定…"),"srSettings",{});
    toolbar_->addSeparator();
    progress_=new QProgressBar(toolbar_); progress_->setObjectName("srProgress"); progress_->setMaximumWidth(110); progress_->hide();
    toolbar_->addWidget(progress_);
    status_=new QLabel(QStringLiteral("画像を開いてください"),toolbar_); status_->setObjectName("srStatus");
    status_->setMargin(6); status_->setMinimumWidth(150);
    status_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
    status_->setFixedHeight(status_->fontMetrics().height()+12);
    status_->setTextFormat(Qt::PlainText); status_->setWordWrap(false); status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    toolbar_->addWidget(status_);
    connect(run_,&QAction::triggered,this,&Controller::start);
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
    updateActions();
}
Controller::~Controller() {
    watchdog_.stop();
    if(job_) job_->cancelled=true;
    stopWorker();
}
void Controller::setStatus(QString text) {
    status_->setToolTip(text);
    status_->setText(text.replace('\r',' ').replace('\n',' ').replace('\t',' '));
}
void Controller::setFullscreen(bool fullscreen) { toolbar_->setVisible(!fullscreen); }
QString Controller::statusText() const { return status_->text(); }
qint64 Controller::backendPid() const { return worker_?worker_->processId():0; }
void Controller::setConfiguration(const Configuration& config) {
    if(job_) return;
    const bool changed=config.runtimeRoot!=configuration_.runtimeRoot || config.workerPath!=configuration_.workerPath ||
        config.modelPath!=configuration_.modelPath || config.devices!=configuration_.devices;
    configuration_=config; configuration_.save(); display();
    if(changed) restartWorker(); else if(!worker_) ensureWorker();
}
void Controller::invalidate() {
    ++generation_; sourceReady_=false; result_={}; showingSr_=false; resultSummary_.clear();
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
    run_->setEnabled(valid && !job_); settings_->setEnabled(!job_);
    cancel_->setEnabled(bool(job_) && !job_->cancelled);
    toggle_->setEnabled(!result_.isNull()); save_->setEnabled(!result_.isNull() && !saving_);
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
void Controller::display() {
    if(!sourceReady_) return;
    QString description,error;
    const auto destination=displayIcc(&description);
    if(destination.isEmpty()) { setStatus(QStringLiteral("画面ICCを読み込めません。SR設定を確認してください。")); return; }
    const auto& source=showingSr_?result_:view_->getImageCore().getSourceImage();
    const Profile profile=showingSr_?Profile{srgbProfile(),QStringLiteral("sRGB"),{},false}:view_->getImageCore().getSourceProfile();
    auto image=convert(source,profile,destination,&error);
    if(image.isNull()) { setStatus(readFailure(error)); return; }
    view_->setDisplayImagePreservingView(image);
    if(!job_) setStatus(showingSr_?QStringLiteral("SR ×4 · %1×%2").arg(result_.width()).arg(result_.height()):
                              QStringLiteral("元画像 · ")+view_->getImageCore().getSourceProfile().description);
    status_->setToolTip(QStringLiteral("入力: %1\n表示: %2\nSR保存: sRGB\n%3")
        .arg(view_->getImageCore().getSourceProfile().description,description,resultSummary_));
}
void Controller::start() {
    if(job_ || !sourceReady_ || !view_->getImageCore().getSourceProfile().error.isEmpty()) return;
    const auto config=configuration_;
    if(!QFileInfo(config.workerPath).isExecutable() || !QFileInfo::exists(config.modelPath) ||
       !QFileInfo::exists(config.runtimeRoot+"/deployment_tools/inference_engine/lib/intel64/libmyriadPlugin.so")) {
        setStatus(QStringLiteral("推論プログラム / モデル / ランタイムが見つかりません。SR設定を確認してください。"));
        emit failed(status_->text()); return;
    }
    if(view_->getCurrentFileDetails().isMovieLoaded && view_->getLoadedMovie().state()==QMovie::Running) window_->pause();
    view_->getImageCore().freezeAnimationForSr();
    display();
    const auto source=view_->getImageCore().getSourceImage();
    const quint64 pixels=quint64(source.width())*source.height();
    if(pixels*240 + 4*192ULL*1024*1024 > quint64(config.memoryMiB)*1024*1024) {
        setStatus(QStringLiteral("SR用メモリー上限を超えます。小さい画像またはSR設定の上限を選んでください。"));
        emit failed(status_->text()); return;
    }
    if(showingSr_) { showingSr_=false; display(); }
    result_={}; resultSummary_.clear();
    auto job=std::make_shared<Job>();
    job->generation=generation_; job->configuration=config; job->size=source.size();
    if(!job->directory->isValid()) { setStatus(QStringLiteral("一時フォルダーを作成できません")); return; }
    job_=job; setStatus(QStringLiteral("sRGB入力を準備中…"));
    progress_->setRange(0,0); progress_->show(); updateActions();
    auto* watcher=new QFutureWatcher<QString>(this);
    const auto profile=view_->getImageCore().getSourceProfile();
    connect(watcher,&QFutureWatcher<QString>::finished,this,[this,job,watcher] {
        const auto error=watcher->result(); watcher->deleteLater();
        if(job!=job_) return;
        if(job->cancelled || job->generation!=generation_) { finish(job); return; }
        if(!error.isEmpty()) { finish(job,error); return; }
        launch(job);
    });
    watcher->setFuture(QtConcurrent::run([source,profile,job] {
        QString error;
        auto input=toSrgb(source,profile,&error);
        if(input.isNull() || job->cancelled) return error;
        if(transparent(input)) {
            job->alpha=input.convertToFormat(QImage::Format_Alpha8);
            // Fully transparent pixels have no visible colour; normalise hidden RGB.
            for(int y=0;y<input.height();++y) {
                uchar* row=input.scanLine(y);
                for(int x=0;x<input.width();++x) if(row[x*4+3]==0) row[x*4]=row[x*4+1]=row[x*4+2]=0;
            }
        }
        const auto path=job->directory->filePath("input.png");
        if(!input.convertToFormat(QImage::Format_RGB888).save(path,"PNG")) return QStringLiteral("SR入力PNGを保存できません");
        job->inputHash=QString::fromLatin1(hash(path));
        job->sourceKey=QString::fromLatin1(QCryptographicHash::hash(job->inputHash.toUtf8()+profile.icc,QCryptographicHash::Sha256).toHex());
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
        {"max_memory_bytes",double(job->configuration.memoryMiB)*1024*1024}};
    ensureWorker(); dispatch();
}
void Controller::dispatch() {
    if(!job_ || job_->cancelled || job_->sent || job_->request.isEmpty()) return;
    if(stoppingWorker_) { setStatus(QStringLiteral("デバイスの切替完了を待っています…")); return; }
    if(!worker_) { finish(job_,QStringLiteral("推論エンジンを起動できません。SR設定を確認してください。")); return; }
    if(!backendReady_) { setStatus(QStringLiteral("デバイスの初期化完了を待っています…")); return; }
    job_->sent=true;
    worker_->write(QJsonDocument(job_->request).toJson(QJsonDocument::Compact)+"\n");
    watchdog_.start(120000); setStatus(QStringLiteral("準備済みデバイスで処理中…"));
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
        result_=result; showingSr_=true;
        QStringList stats;
        for(const auto& value:job->completion.value("devices").toArray()) {
            auto device=value.toObject(); stats<<QStringLiteral("%1: %2タイル").arg(device.value("id").toString()).arg(device.value("tiles").toInt());
        }
        resultSummary_=stats.join(" / ")+QStringLiteral("\nノイズ低減: %1 / 処理: %2秒（初期化除く）")
            .arg(job->configuration.denoise).arg(job->completion.value("wall_ms").toDouble()/1000,0,'f',2);
        if(!job->warnings.isEmpty()) resultSummary_+="\n"+job->warnings.join("\n");
        finish(job); display(); emit resultReady();
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
        image.setColorSpace(QColorSpace::SRgb); return image;
    }));
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
                progress_->setRange(0,total); progress_->setValue(done);
                setStatus(QStringLiteral("処理中 %1/%2 · %3").arg(done).arg(total).arg(message.value("device").toString()));
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
    else if(cancelled) setStatus(sourceReady_?QStringLiteral("中止しました · 元画像"):QStringLiteral("画像を読み込み中…"));
    updateActions();
}
void Controller::cancel() {
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
void Controller::saveAs() {
    if(result_.isNull() || saving_) return;
    const auto original=view_->getCurrentFileDetails().fileInfo;
    QString selected;
    QString path=QFileDialog::getSaveFileName(window_,QStringLiteral("SR画像を保存（sRGB ICC付き）"),
        original.absolutePath()+"/"+original.completeBaseName()+"_sr4x.png",QStringLiteral("PNG (*.png);;JPEG (*.jpg *.jpeg)"),&selected);
    if(path.isEmpty()) return;
    QByteArray format=selected.startsWith("JPEG")?QByteArray("jpeg"):QByteArray("png");
    if(QFileInfo(path).suffix().isEmpty()) path+=format=="jpeg"?".jpg":".png";
    if(QFileInfo(path).absoluteFilePath()==original.absoluteFilePath() ||
       (!QFileInfo(path).canonicalFilePath().isEmpty() && QFileInfo(path).canonicalFilePath()==original.canonicalFilePath())) {
        setStatus(QStringLiteral("元画像への上書きはできません")); return;
    }
    QColor background=Qt::white;
    if(format=="jpeg" && transparent(result_)) {
        background=QColorDialog::getColor(Qt::white,window_,QStringLiteral("JPEGの透明部分の背景色"));
        if(!background.isValid()) return;
    }
    const auto image=view_->getImageCore().matchCurrentRotation(result_);
    const auto generation=generation_;
    auto* watcher=new QFutureWatcher<QString>(this);
    saving_=true; updateActions(); setStatus(QStringLiteral("SR画像を保存中…"));
    connect(watcher,&QFutureWatcher<QString>::finished,this,[this,watcher,generation,path] {
        const auto error=watcher->result(); watcher->deleteLater(); saving_=false; updateActions();
        if(generation==generation_) setStatus(error.isEmpty()?QStringLiteral("保存しました: ")+QFileInfo(path).fileName():error);
    });
    watcher->setFuture(QtConcurrent::run([image,path,format,background] {
        QString error; writeImage(image,path,format,background,&error); return error;
    }));
}
void Controller::showSettings() {
    if(job_) return;
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
    auto* halo=new QSpinBox(&dialog); halo->setRange(0,64); halo->setValue(configuration_.halo); form->addRow(QStringLiteral("タイル境界の余白（画素）"),halo);
    auto* noise=new QSpinBox(&dialog); noise->setObjectName("srDenoise");
    noise->setRange(0,15); noise->setSpecialValueText(QStringLiteral("なし")); noise->setValue(configuration_.denoise);
    noise->setToolTip(QStringLiteral("3: 弱 / 6: 標準 / 10: 強。JPEGのノイズをSR前に低減します。強くすると細部も滑らかになります。"));
    form->addRow(QStringLiteral("ノイズ低減（SR前）"),noise);
    auto* note=new QLabel(QStringLiteral("SRは×4・sRGBで処理/保存します。画面ICCは保存画像に適用しません。\n個体を指定する場合はIDをカンマ区切りで入力できます。世代混在時の画質は比較検証中です。\nノイズ低減は0=なし、3=弱、6=標準、10=強。強くすると細部も滑らかになります。"),&dialog);
    note->setWordWrap(true); layout->addWidget(note);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog); layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept); connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if(dialog.exec()!=QDialog::Accepted) return;
    auto conf=configuration_;
    conf.runtimeRoot=runtime->text(); conf.workerPath=worker->text(); conf.modelPath=model->text();
    conf.devices=(devices->currentIndex()>=0 && devices->currentText()==devices->itemText(devices->currentIndex()))?devices->currentData().toString():devices->currentText().trimmed();
    conf.displayProfile=displayMode->currentIndex()==0?QString():displayMode->currentIndex()==1?QStringLiteral("sRGB"):icc->text();
    conf.memoryMiB=memory->value(); conf.halo=halo->value(); conf.denoise=noise->value(); setConfiguration(conf);
}
}
