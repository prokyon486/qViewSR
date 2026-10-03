// SPDX-License-Identifier: GPL-3.0-or-later
#include "vector_controller.h"
#include "cancellable_save_file.h"
#include "memory_budget.h"
#include "generated_svg_renderer.h"
#include "mainwindow.h"
#include "qvgraphicsview.h"
#include "sr/color_pipeline.h"

#include <QApplication>
#include <QCheckBox>
#include <QColorSpace>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDomDocument>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QImageWriter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSvgRenderer>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <QVector>
#include <QtConcurrent/QtConcurrentRun>
#include <cmath>

namespace Vector {
namespace {
constexpr qint64 MaxOutputPixels = 64000000;
constexpr qint64 MaxProxyPixels = 16000000;
constexpr int MaxProxySide = 8192;
const QString SvgNamespace = QStringLiteral("http://www.w3.org/2000/svg");

struct Appearance {
    double width = 2.6;
    int opacity = 85;
    QString color = QStringLiteral("source");
    QString background = QStringLiteral("white");
    bool showFill = false;
    bool grayFill = false;
};

struct Rendered {
    QByteArray svg;
    QImage image;
    QString error;
};

QString repositoryRoot() {
    const QString override = qEnvironmentVariable("QVIEWSR_VECTOR_ROOT");
    if (!override.isEmpty()) return override;
    QDir dir(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 7; ++i) {
        if (QFileInfo::exists(dir.filePath("tools/vector/vector_worker.py"))) return dir.absolutePath();
        if (!dir.cdUp()) break;
    }
    return QCoreApplication::applicationDirPath();
}

}

bool rasterSize(QSize source, double scale, QSize* size, QString* error) {
    if (!std::isfinite(scale) || scale < 0.01 || scale > 8.0 || source.isEmpty()) {
        if (error) *error = QStringLiteral("画像化の倍率は0.01〜8倍で指定してください。");
        return false;
    }
    const qint64 width = qRound64(source.width() * scale);
    const qint64 height = qRound64(source.height() * scale);
    if (width <= 0 || height <= 0 || width > 32768 || height > 32768 || width * height > MaxOutputPixels) {
        if (error) *error = QStringLiteral("出力画像が上限（64メガピクセル・一辺32768px）を超えます。保存／受け渡し画像の倍率を下げてください。SVG保存はこの制限を受けません。");
        return false;
    }
    *size = QSize(int(width), int(height));
    return true;
}

QImage rasterizeSvg(const QByteArray& svg, QSize size, QString* error) {
    const auto memoryError = svgMemoryError(quint64(svg.size()));
    if (!memoryError.isEmpty()) { if (error) *error = memoryError; return {}; }
    GeneratedSvgRenderer renderer(svg);
    if (!renderer.isValid()) {
        if (error) *error = renderer.errorString();
        return {};
    }
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        if (error) *error = QStringLiteral("描画用のメモリーを確保できません。");
        return {};
    }
    image.fill(Qt::transparent);
    image.setColorSpace(QColorSpace::SRgb);
    const auto box = renderer.viewBoxF();
    const auto transform = QTransform::fromTranslate(-box.left(), -box.top())
        * QTransform::fromScale(size.width()/box.width(), size.height()/box.height());
    if (!renderer.render(image, transform, error)) return {};
    return image;
}

namespace {
void grayElement(QDomElement element) {
    for (const QString& attribute : {QStringLiteral("fill"), QStringLiteral("stroke"), QStringLiteral("stop-color")}) {
        if (!element.hasAttribute(attribute)) continue;
        const QColor color(element.attribute(attribute));
        if (!color.isValid()) continue; // "none" and paint servers are preserved.
        const int gray = qRound(.2126 * color.red() + .7152 * color.green() + .0722 * color.blue());
        const QColor converted(gray, gray, gray, color.alpha());
        element.setAttribute(attribute, converted.name(color.alpha() == 255 ? QColor::HexRgb : QColor::HexArgb));
    }
    for (auto child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) grayElement(child);
}

Rendered compose(const QByteArray& fillBytes, const QByteArray& linesBytes, QSize size, const Appearance& look, double previewScale) {
    Rendered output;
    output.error = svgMemoryError(quint64(fillBytes.size()) + quint64(linesBytes.size()));
    if (!output.error.isEmpty()) return output;
    QDomDocument fillDocument, linesDocument, document;
    if (!fillDocument.setContent(fillBytes) || !linesDocument.setContent(linesBytes)) {
        output.error = QStringLiteral("生成されたベクターデータを読み込めません。");
        return output;
    }
    auto root = document.createElementNS(SvgNamespace, QStringLiteral("svg"));
    root.setAttribute(QStringLiteral("width"), size.width());
    root.setAttribute(QStringLiteral("height"), size.height());
    root.setAttribute(QStringLiteral("viewBox"), QStringLiteral("0 0 %1 %2").arg(size.width()).arg(size.height()));
    document.appendChild(root);
    if (look.background != QStringLiteral("transparent")) {
        auto background = document.createElement(QStringLiteral("rect"));
        background.setAttribute(QStringLiteral("id"), QStringLiteral("background"));
        background.setAttribute(QStringLiteral("width"), size.width());
        background.setAttribute(QStringLiteral("height"), size.height());
        background.setAttribute(QStringLiteral("fill"), look.background == QStringLiteral("black") ? "#000000" : "#ffffff");
        root.appendChild(background);
    }
    if (look.showFill) {
        auto fill = document.createElement(QStringLiteral("g"));
        fill.setAttribute(QStringLiteral("id"), QStringLiteral("fill"));
        root.appendChild(fill);
        for (auto child = fillDocument.documentElement().firstChild(); !child.isNull(); child = child.nextSibling())
            fill.appendChild(document.importNode(child, true));
        if (look.grayFill) grayElement(fill);
    }
    if (look.color != QStringLiteral("transparent") && look.opacity > 0) {
        auto lines = document.createElement(QStringLiteral("g"));
        lines.setAttribute(QStringLiteral("id"), QStringLiteral("lines"));
        lines.setAttribute(QStringLiteral("fill"), QStringLiteral("none"));
        lines.setAttribute(QStringLiteral("stroke-width"), QString::number(look.width));
        lines.setAttribute(QStringLiteral("stroke-linecap"), QStringLiteral("round"));
        lines.setAttribute(QStringLiteral("stroke-linejoin"), QStringLiteral("round"));
        lines.setAttribute(QStringLiteral("opacity"), QString::number(look.opacity / 100.0));
        root.appendChild(lines);
        // The worker returns source-colored paths in source-image coordinates.
        const auto paths = linesDocument.elementsByTagName(QStringLiteral("path"));
        // QDomNodeList is live: importing and changing destination nodes can
        // invalidate its generation even across documents. Snapshot references
        // before any mutation, preserving the descendant document order while
        // avoiding a complete source-tree scan for every imported path.
        const int pathCount = paths.size();
        QVector<QDomNode> pathNodes;
        pathNodes.reserve(pathCount);
        for (int i = 0; i < pathCount; ++i) pathNodes.append(paths.item(i));
        for (const auto& sourcePath : pathNodes) {
            auto path = document.importNode(sourcePath, true).toElement();
            path.setAttribute(QStringLiteral("fill"), QStringLiteral("none"));
            path.removeAttribute(QStringLiteral("stroke-width"));
            if (look.color != QStringLiteral("source"))
                path.setAttribute(QStringLiteral("stroke"), look.color == QStringLiteral("white") ? "#ffffff" : "#000000");
            lines.appendChild(path);
        }
    }
    output.svg = document.toByteArray(-1);
    output.error = svgMemoryError(quint64(output.svg.size()));
    if (!output.error.isEmpty()) { output.svg.clear(); return output; }
    // This bitmap only supplies viewer geometry and a bounded fallback. The SVG
    // retains full input coordinates; exports are rendered separately on demand.
    const double proxyScale = qMin(previewScale, qMin(
        std::sqrt(double(MaxProxyPixels) / (qint64(size.width()) * size.height())),
        double(MaxProxySide) / qMax(size.width(), size.height())));
    const QSize proxySize(qMax(1, int(std::floor(size.width() * proxyScale))),
                          qMax(1, int(std::floor(size.height() * proxyScale))));
    output.image = rasterizeSvg(output.svg, proxySize, &output.error);
    return output;
}

bool writePng(const QImage& image, const QString& path, QString* error,
              const std::shared_ptr<std::atomic_bool>& cancelled = {}) {
    CancellableSaveFile file(path, cancelled);
    if (file.cancellationRequested()) {
        if (error) *error = QStringLiteral("入力画像の準備を中止しました。");
        return false;
    }
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    QImageWriter writer(&file, "png");
    if (!writer.write(image)) {
        if (error) *error = file.cancellationRequested()
            ? QStringLiteral("入力画像の準備を中止しました。") : writer.errorString();
        return false;
    }
    if (!file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

}

QByteArray readSvgAsset(const QString& requestedPath, const QString& cacheDirectory, QString* error) {
    const QFileInfo info(QDir(cacheDirectory).absoluteFilePath(requestedPath));
    const auto canonical = info.canonicalFilePath();
    const auto cacheCanonical = QFileInfo(cacheDirectory).canonicalFilePath();
    if (canonical.isEmpty() || cacheCanonical.isEmpty() || !info.isFile() ||
        !canonical.startsWith(cacheCanonical + QDir::separator()) || info.size() <= 0) {
        if (error) *error = QStringLiteral("ワーカーが不正なSVG出力を返しました。");
        return {};
    }
    QFile file(canonical);
    if (!file.open(QIODevice::ReadOnly)) { if (error) *error = file.errorString(); return {}; }
    const qint64 expected = file.size();
    const auto memoryError = expected > 0 ? svgMemoryError(quint64(expected))
        : QStringLiteral("ワーカーが空のSVG出力を返しました。");
    if (!memoryError.isEmpty()) { if (error) *error = memoryError; return {}; }
    // A changed cache must not turn readAll() into an unbounded allocation.
    const auto bytes = file.read(expected + 1);
    if (file.error() != QFileDevice::NoError) { if (error) *error = file.errorString(); return {}; }
    if (bytes.size() != expected || file.size() != expected) {
        if (error) *error = QStringLiteral("読み込み中にSVG出力のサイズが変わりました。もう一度生成してください。");
        return {};
    }
    return bytes;
}

struct Controller::Private {
    Controller* owner;
    MainWindow* window;
    QVGraphicsView* view;
    QPointer<QDialog> panel;
    QComboBox *strength, *detail, *color, *background;
    QDoubleSpinBox *width, *scale, *previewScale;
    QSlider *opacity, *suppression;
    QCheckBox *showFill, *grayFill, *showResult, *correctLines;
    QPushButton *run, *stop, *svgSave, *pngSave;
    QLabel* status;
    QTimer debounce, watchdog;
    QPointer<QProcess> process;
    QByteArray stdoutBytes, stderrBytes, fillSvg, linesSvg, composedSvg;
    QSize sourceSize;
    QImage image;
    QString correctionSummary;
    std::shared_ptr<QTemporaryDir> workspace;
    std::shared_ptr<std::atomic_bool> preparationCancelled;
    quint64 epoch = 0, shapeRevision = 0, renderRevision = 0;
    bool preparing = false, rendering = false, rerun = false, renderPending = false;
    bool generated = false, showing = false, fullscreen = false, restorePanel = false, externalBusy = false, saving = false, loading = false;

    Private(Controller* controller, MainWindow* parent, QVGraphicsView* graphics)
        : owner(controller), window(parent), view(graphics) {
        panel = new QDialog(parent, Qt::Tool);
        panel->setObjectName(QStringLiteral("vectorPanel"));
        panel->setWindowTitle(QStringLiteral("ベクター変換"));
        panel->setModal(false);
        panel->setAttribute(Qt::WA_DeleteOnClose, false);
        panel->installEventFilter(owner);
        auto* layout = new QVBoxLayout(panel);
        auto* explanation = new QLabel(QStringLiteral("塗りと主線を別々にベクター化します。静止画の元画像が入力です。"), panel);
        explanation->setWordWrap(true);
        layout->addWidget(explanation);
        auto* form = new QFormLayout;
        layout->addLayout(form);
        auto combo = [this, form](const QString& name, const QString& text,
                                  const QList<QPair<QString, QString>>& items) {
            auto* control = new QComboBox(panel);
            control->setObjectName(name);
            for (const auto& item : items) control->addItem(item.first, item.second);
            form->addRow(text, control);
            return control;
        };
        strength = combo("vectorStrength", QStringLiteral("主線の抽出強度"),
                         {{QStringLiteral("弱：濃い線を中心に"), "weak"}, {QStringLiteral("標準"), "balanced"}, {QStringLiteral("強：薄い線も拾う"), "strong"}});
        detail = combo("vectorDetail", QStringLiteral("曲線の細かさ"),
                       {{QStringLiteral("細部優先（0.3px）"), "fine"}, {QStringLiteral("標準（1px）"), "balanced"}, {QStringLiteral("簡略化（3px）"), "simple"}});
        correctLines = new QCheckBox(QStringLiteral("主線を補正（直線・円・楕円・滑らかな曲線）"), panel);
        correctLines->setObjectName("vectorCorrectLines");
        correctLines->setToolTip(QStringLiteral("元の形からのずれを抑えながら、主線を直線・円・楕円や少ないベジエ曲線で近似します。オフにすると補正前の線に戻ります。塗りには適用しません。"));
        form->addRow(correctLines);
        width = new QDoubleSpinBox(panel);
        width->setObjectName("vectorWidth"); width->setRange(.3, 3.0); width->setSingleStep(.1); width->setDecimals(1);
        width->setSuffix(QStringLiteral(" px")); form->addRow(QStringLiteral("線幅（元画像基準）"), width);
        auto percentageSlider = [this, form](const QString& name, const QString& text, int step) {
            auto* row = new QWidget(panel);
            auto* controls = new QHBoxLayout(row);
            controls->setContentsMargins(0, 0, 0, 0);
            auto* slider = new QSlider(Qt::Horizontal, row);
            slider->setObjectName(name);
            slider->setAccessibleName(text);
            slider->setRange(0, 100 / step);
            slider->setPageStep(10 / step);
            slider->setMinimumWidth(120);
            auto* value = new QLabel(QStringLiteral("0 %"), row);
            value->setObjectName(name + QStringLiteral("Value"));
            value->setMinimumWidth(value->fontMetrics().horizontalAdvance(QStringLiteral("100 %")));
            value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            value->setBuddy(slider);
            QObject::connect(slider, &QSlider::valueChanged, value, [value, step](int position) {
                value->setText(QStringLiteral("%1 %").arg(position * step));
            });
            controls->addWidget(slider, 1);
            controls->addWidget(value);
            form->addRow(text, row);
            return slider;
        };
        opacity = percentageSlider("vectorOpacity", QStringLiteral("主線の濃さ"), 1);
        suppression = percentageSlider("vectorSuppression", QStringLiteral("塗り側の元線を弱める量"), 5);
        color = combo("vectorColor", QStringLiteral("主線の色"),
                      {{QStringLiteral("透明（主線を非表示）"), "transparent"}, {QStringLiteral("白"), "white"},
                       {QStringLiteral("黒"), "black"}, {QStringLiteral("元画像から推定"), "source"}});
        background = combo("vectorBackground", QStringLiteral("背景の色"),
                           {{QStringLiteral("透明"), "transparent"}, {QStringLiteral("白"), "white"}, {QStringLiteral("黒"), "black"}});
        showFill = new QCheckBox(QStringLiteral("塗りを表示"), panel); showFill->setObjectName("vectorShowFill");
        grayFill = new QCheckBox(QStringLiteral("塗りをグレースケール化"), panel); grayFill->setObjectName("vectorGrayFill");
        form->addRow(showFill); form->addRow(grayFill);
        previewScale = new QDoubleSpinBox(panel);
        previewScale->setObjectName("vectorPreviewScale"); previewScale->setRange(.01, 8); previewScale->setSingleStep(.1);
        previewScale->setSuffix(QStringLiteral(" 倍")); form->addRow(QStringLiteral("受け渡し画像の倍率"), previewScale);
        previewScale->setToolTip(QStringLiteral("Ctrl＋ドラッグで渡す画像の倍率です。画面のプレビューは倍率に関係なくSVGから描画します。"));
        auto* note = new QLabel(QStringLiteral("透明背景は塗りの白い部分を消しません。主線を透明にしても塗り側の元線は残ります。"), panel);
        note->setWordWrap(true); layout->addWidget(note);
        showResult = new QCheckBox(QStringLiteral("変換結果を表示（オフで元画像）"), panel);
        showResult->setObjectName("vectorShowResult"); layout->addWidget(showResult);
        auto* actions = new QHBoxLayout;
        run = new QPushButton(QStringLiteral("生成・更新"), panel); run->setObjectName("vectorGenerate");
        stop = new QPushButton(QStringLiteral("中止"), panel); stop->setObjectName("vectorCancel");
        actions->addWidget(run); actions->addWidget(stop); layout->addLayout(actions);
        auto* exports = new QHBoxLayout;
        svgSave = new QPushButton(QStringLiteral("SVG保存"), panel); svgSave->setObjectName("vectorSaveSvg");
        pngSave = new QPushButton(QStringLiteral("PNG保存"), panel); pngSave->setObjectName("vectorSavePng");
        scale = new QDoubleSpinBox(panel); scale->setObjectName("vectorExportScale"); scale->setRange(.01, 8); scale->setSingleStep(.1); scale->setValue(4); scale->setSuffix(QStringLiteral(" 倍"));
        scale->setPrefix(QStringLiteral("保存 "));
        scale->setAccessibleName(QStringLiteral("PNG保存倍率"));
        scale->setToolTip(QStringLiteral("PNG保存時の倍率。プレビューとは独立してSVGから描画します。"));
        exports->addWidget(svgSave); exports->addWidget(pngSave); exports->addWidget(scale); layout->addLayout(exports);
        status = new QLabel(panel); status->setObjectName("vectorStatus"); status->setWordWrap(true); status->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(status);
        panel->setMinimumWidth(390);
        loadSettings();
        debounce.setSingleShot(true); debounce.setInterval(350);
        QObject::connect(&debounce, &QTimer::timeout, owner, &Controller::generate);
        watchdog.setSingleShot(true); watchdog.setInterval(180000);
        QObject::connect(&watchdog, &QTimer::timeout, owner, [this] {
            owner->cancel(); report(QStringLiteral("ベクター変換が制限時間を超えたため中止しました。"));
        });
        for (auto* control : {strength, detail})
            QObject::connect(control, &QComboBox::currentIndexChanged, owner, [this] { shapeChanged(); });
        QObject::connect(suppression, &QSlider::valueChanged, owner, [this] { shapeChanged(); });
        QObject::connect(correctLines, &QCheckBox::toggled, owner, [this] { shapeChanged(); });
        for (auto* control : {color, background})
            QObject::connect(control, &QComboBox::currentIndexChanged, owner, [this] { appearanceChanged(); });
        QObject::connect(width, &QDoubleSpinBox::valueChanged, owner, [this] { appearanceChanged(); });
        QObject::connect(opacity, &QSlider::valueChanged, owner, [this] { appearanceChanged(); });
        QObject::connect(previewScale, &QDoubleSpinBox::valueChanged, owner, [this] { appearanceChanged(); });
        for (auto* control : {showFill, grayFill})
            QObject::connect(control, &QCheckBox::toggled, owner, [this] { appearanceChanged(); });
        QObject::connect(showResult, &QCheckBox::toggled, owner, &Controller::setShowingResult);
        QObject::connect(run, &QPushButton::clicked, owner, &Controller::generate);
        QObject::connect(stop, &QPushButton::clicked, owner, &Controller::cancel);
        QObject::connect(svgSave, &QPushButton::clicked, owner, [this] {
            const QString path = QFileDialog::getSaveFileName(panel, QStringLiteral("SVGを保存"), suggestedName("svg"), QStringLiteral("SVG画像 (*.svg)"));
            if (path.isEmpty()) return;
            QString error;
            if (!owner->saveSvg(path, &error)) report(error);
            else setStatus(QStringLiteral("SVGを保存しました。"));
        });
        QObject::connect(pngSave, &QPushButton::clicked, owner, [this] { exportPng(); });
        QObject::connect(&view->getImageCore(), &QVImageCore::sourceChanging, owner, [this] { loading = true; invalidate(); });
        QObject::connect(view, &QVGraphicsView::fileChanged, owner, [this] { loading = false; updateControls(); });
        QObject::connect(view, &QVGraphicsView::vectorRenderingFailed, owner, [this](const QString& error) { report(error); });
        updateControls();
    }

    ~Private() {
        ++epoch;
        if (preparationCancelled) preparationCancelled->store(true, std::memory_order_relaxed);
        if (process) { process->disconnect(owner); process->kill(); process->waitForFinished(2000); }
        saveSettings();
        if (panel) { panel->removeEventFilter(owner); delete panel; }
    }

    Appearance appearance() const {
        return {width->value(), opacity->value(), color->currentData().toString(), background->currentData().toString(), showFill->isChecked(), grayFill->isChecked()};
    }

    void loadSettings() {
        QSettings settings;
        auto selected = [&settings](QComboBox* control, const QString& key, const QString& fallback) {
            int index = control->findData(settings.value("vector/" + key, fallback));
            if (index < 0) index = control->findData(fallback);
            control->setCurrentIndex(index);
        };
        selected(strength, "strength", "strong"); selected(detail, "detail", "balanced");
        selected(color, "color", "source"); selected(background, "background", "white");
        width->setValue(settings.value("vector/width", 2.6).toDouble());
        opacity->setValue(settings.value("vector/opacity", 85).toInt());
        suppression->setValue(qRound(settings.value("vector/suppression", 30).toDouble() / 5));
        showFill->setChecked(settings.value("vector/showFill", false).toBool());
        grayFill->setChecked(settings.value("vector/grayFill", false).toBool());
        correctLines->setChecked(settings.value("vector/correctLines", false).toBool());
        scale->setValue(settings.value("vector/exportScale", 4.0).toDouble());
        previewScale->setValue(settings.value("vector/previewScale", 4.0).toDouble());
        panel->restoreGeometry(settings.value("vector/panelGeometry").toByteArray());
    }

    void saveSettings() const {
        if (!panel) return;
        QSettings settings;
        settings.setValue("vector/panelGeometry", panel->saveGeometry());
        settings.setValue("vector/strength", strength->currentData()); settings.setValue("vector/detail", detail->currentData());
        settings.setValue("vector/width", width->value()); settings.setValue("vector/opacity", opacity->value());
        settings.setValue("vector/suppression", suppression->value() * 5); settings.setValue("vector/color", color->currentData());
        settings.setValue("vector/background", background->currentData()); settings.setValue("vector/showFill", showFill->isChecked());
        settings.setValue("vector/grayFill", grayFill->isChecked()); settings.setValue("vector/exportScale", scale->value());
        settings.setValue("vector/previewScale", previewScale->value());
        settings.setValue("vector/correctLines", correctLines->isChecked());
    }

    QString eligibilityError() const {
        const auto& file = view->getCurrentFileDetails();
        if (file.isModelDocument) return QStringLiteral("3Dモデルのベクター変換には対応していません。静止画を開いてください。");
        if (file.isMovieLoaded || view->getImageCore().isAnimationFrozenForSr())
            return QStringLiteral("アニメーションのベクター変換には対応していません。静止画を開いてください。");
        if (!file.isPixmapLoaded || loading || view->getImageCore().getSourceImage().isNull())
            return QStringLiteral("変換する静止画を開いてください。");
        if (externalBusy) return QStringLiteral("超解像処理中はベクター変換を操作できません。");
        return {};
    }

    bool workerBusy() const { return preparing || process; }
    bool busy() const { return workerBusy() || rendering || saving || debounce.isActive() || rerun; }
    void setStatus(const QString& message) { status->setText(message); status->setToolTip(message); }
    void report(const QString& message) { setStatus(message); emit owner->failed(message); updateControls(false); }
    QString suggestedName(const QString& extension) const {
        const QFileInfo file = view->getCurrentFileDetails().fileInfo;
        return file.absolutePath() + '/' + file.completeBaseName() + "-vector." + extension;
    }

    bool maySaveTo(const QString& path, QString* error) const {
        const QFileInfo output(path), original(view->getCurrentFileDetails().fileInfo);
        if (QDir::cleanPath(output.absoluteFilePath()) == QDir::cleanPath(original.absoluteFilePath()) ||
            (!output.canonicalFilePath().isEmpty() && output.canonicalFilePath() == original.canonicalFilePath())) {
            if (error) *error = QStringLiteral("元画像は上書きできません。別のファイル名で保存してください。");
            return false;
        }
        return true;
    }

    QByteArray exportDocument(QSize* outputSize, QString* error) const {
        if (composedSvg.isEmpty()) { if (error) *error = QStringLiteral("保存する変換結果がありません。"); return {}; }
        const auto memoryError = svgMemoryError(quint64(composedSvg.size()));
        if (!memoryError.isEmpty()) { if (error) *error = memoryError; return {}; }
        QDomDocument document;
        if (!document.setContent(composedSvg)) { if (error) *error = QStringLiteral("保存するSVGが不正です。"); return {}; }
        const int rotation = view->getImageCore().getCurrentRotation();
        const bool mirror = view->transform().m11() < 0, flip = view->transform().m22() < 0;
        QTransform transform = QImage::trueMatrix(QTransform().rotate(rotation), sourceSize.width(), sourceSize.height());
        const QRectF bounds = transform.mapRect(QRectF(QPointF(0, 0), QSizeF(sourceSize)));
        *outputSize = QSize(qRound(bounds.width()), qRound(bounds.height()));
        transform *= QTransform(mirror ? -1 : 1, 0, 0, flip ? -1 : 1,
                                mirror ? outputSize->width() : 0, flip ? outputSize->height() : 0);
        auto root = document.documentElement();
        root.setAttribute("width", outputSize->width()); root.setAttribute("height", outputSize->height());
        root.setAttribute("viewBox", QStringLiteral("0 0 %1 %2").arg(outputSize->width()).arg(outputSize->height()));
        if (!transform.isIdentity()) {
            auto transformed = document.createElement("g");
            transformed.setAttribute("transform", QStringLiteral("matrix(%1 %2 %3 %4 %5 %6)")
                                     .arg(transform.m11(), 0, 'g', 12).arg(transform.m12(), 0, 'g', 12)
                                     .arg(transform.m21(), 0, 'g', 12).arg(transform.m22(), 0, 'g', 12)
                                     .arg(transform.dx(), 0, 'g', 12).arg(transform.dy(), 0, 'g', 12));
            while (!root.firstChild().isNull()) transformed.appendChild(root.firstChild());
            root.appendChild(transformed);
        }
        const auto bytes = document.toByteArray(-1);
        const auto outputError = svgMemoryError(quint64(bytes.size()));
        if (!outputError.isEmpty()) { if (error) *error = outputError; return {}; }
        return bytes;
    }

    void updateControls(bool updateMessage = true) {
        const QString unavailable = eligibilityError();
        run->setEnabled(unavailable.isEmpty() && !workerBusy() && !saving);
        stop->setEnabled(workerBusy() || rendering || debounce.isActive());
        showResult->setEnabled(!image.isNull() && !externalBusy);
        svgSave->setEnabled(!composedSvg.isEmpty() && !busy());
        pngSave->setEnabled(!composedSvg.isEmpty() && !busy());
        for (auto* control : {strength, detail, color, background}) control->setEnabled(!externalBusy);
        showFill->setEnabled(!externalBusy); previewScale->setEnabled(!externalBusy); scale->setEnabled(!externalBusy);
        correctLines->setEnabled(!externalBusy);
        suppression->parentWidget()->setEnabled(!externalBusy && showFill->isChecked()); grayFill->setEnabled(!externalBusy && showFill->isChecked());
        const bool linesVisible = color->currentData().toString() != "transparent";
        width->setEnabled(!externalBusy && linesVisible); opacity->parentWidget()->setEnabled(!externalBusy && linesVisible);
        if (updateMessage && !unavailable.isEmpty()) setStatus(unavailable);
        else if (updateMessage && image.isNull() && !busy()) setStatus(QStringLiteral("「生成・更新」で変換を開始します。設定の変更は生成後に自動反映します。"));
        emit owner->stateChanged();
    }

    void shapeChanged() {
        ++shapeRevision;
        saveSettings();
        if (generated && !externalBusy) debounce.start();
        updateControls(false);
    }

    void appearanceChanged() {
        saveSettings();
        updateControls(false);
        if (!fillSvg.isEmpty() && !externalBusy) queueRender();
    }

    void invalidate() {
        owner->cancel();
        fillSvg.clear(); linesSvg.clear(); composedSvg.clear(); image = {}; sourceSize = {};
        workspace.reset(); generated = false; showing = false;
        correctionSummary.clear();
        { QSignalBlocker block(showResult); showResult->setChecked(false); }
        updateControls();
    }

    void queueRender() {
        ++renderRevision;
        if (fillSvg.isEmpty() || linesSvg.isEmpty()) return;
        if (rendering) { renderPending = true; return; }
        renderPending = false;
        startRender();
    }

    void startRender() {
        const auto renderEpoch = epoch, revision = renderRevision;
        const auto fill = fillSvg, lines = linesSvg;
        const auto size = sourceSize;
        const auto look = appearance();
        const double requestedScale = previewScale->value();
        rendering = true;
        auto* watcher = new QFutureWatcher<Rendered>(owner);
        QObject::connect(watcher, &QFutureWatcher<Rendered>::finished, owner, [this, watcher, renderEpoch, revision] {
            auto result = watcher->result(); watcher->deleteLater(); rendering = false;
            if (renderEpoch == epoch && revision == renderRevision && !externalBusy) {
                if (!result.error.isEmpty()) report(result.error);
                else {
                    image = result.image; composedSvg = result.svg;
                    if (showResult->isChecked()) { showing = true; emit owner->previewReady(image, composedSvg, previewScale->value()); }
                    setStatus(QStringLiteral("SVG: %1×%2px。拡大時もベクターから描画します。\n%3")
                              .arg(sourceSize.width()).arg(sourceSize.height()).arg(correctionSummary));
                    emit owner->resultReady();
                }
            }
            if (renderPending && !fillSvg.isEmpty() && !externalBusy) { renderPending = false; startRender(); }
            updateControls(false);
        });
        watcher->setFuture(QtConcurrent::run([fill, lines, size, look, requestedScale] { return compose(fill, lines, size, look, requestedScale); }));
        updateControls(false);
    }

    void startJob() {
        const QString unavailable = eligibilityError();
        if (!unavailable.isEmpty()) { report(unavailable); return; }
        if (workerBusy()) { rerun = true; return; }
        const auto memoryError = preparationMemoryError(view->getImageCore().getSourceImage().size());
        if (!memoryError.isEmpty()) { report(memoryError); return; }
        const QString root = repositoryRoot();
        const QString python = qEnvironmentVariable("QVIEWSR_VECTOR_PYTHON", root + "/.local/vector-probe-venv/bin/python");
        const QString script = qEnvironmentVariable("QVIEWSR_VECTOR_WORKER", root + "/tools/vector/vector_worker.py");
        if (!QFileInfo(python).isExecutable() || !QFileInfo(script).isFile()) {
            report(QStringLiteral("ベクター変換の実行環境が見つかりません。セットアップ手順を確認してください。")); return;
        }
        if (!workspace) workspace = std::make_shared<QTemporaryDir>(QDir::tempPath() + "/qviewsr-vector-XXXXXX");
        if (!workspace->isValid()) { report(QStringLiteral("作業用フォルダーを作成できません。")); return; }
        generated = true; rerun = false; preparing = true;
        const auto cancelled = std::make_shared<std::atomic_bool>(false);
        preparationCancelled = cancelled;
        if (image.isNull()) { QSignalBlocker block(showResult); showResult->setChecked(true); }
        const auto jobEpoch = epoch, revision = shapeRevision;
        const auto folder = workspace;
        const auto source = view->getImageCore().getSourceImage();
        const auto profile = view->getImageCore().getSourceProfile();
        const QSize size = source.size();
        const QString selectedStrength = strength->currentData().toString();
        const QString selectedDetail = detail->currentData().toString();
        const int selectedSuppression = suppression->value() * 5;
        const bool selectedCorrection = correctLines->isChecked();
        setStatus(QStringLiteral("入力画像の色と作業ファイルを準備しています…"));
        updateControls(false);
        auto* watcher = new QFutureWatcher<QString>(owner);
        QObject::connect(watcher, &QFutureWatcher<QString>::finished, owner,
                         [this, watcher, folder, cancelled, jobEpoch, revision, size, python, script, selectedStrength, selectedDetail, selectedSuppression, selectedCorrection] {
            const QString error = watcher->result(); watcher->deleteLater(); preparing = false;
            if (preparationCancelled == cancelled) preparationCancelled.reset();
            if (jobEpoch != epoch || externalBusy) { continuePending(); return; }
            if (!error.isEmpty()) { report(error); continuePending(); return; }
            if (revision != shapeRevision) { rerun = true; continuePending(); return; }
            launch(folder, jobEpoch, revision, size, python, script, selectedStrength, selectedDetail, selectedSuppression, selectedCorrection);
        });
        watcher->setFuture(QtConcurrent::run([source, profile, folder, cancelled] {
            const auto stopped = [&] { return cancelled->load(std::memory_order_relaxed); };
            const auto cancelledError = QStringLiteral("入力画像の準備を中止しました。");
            if (stopped()) return cancelledError;
            const QString input = folder->filePath("input.png");
            if (QFileInfo::exists(input)) return QString();
            const auto unavailable = preparationMemoryError(source.size());
            if (!unavailable.isEmpty()) return unavailable;
            if (stopped()) return cancelledError;
            QString error;
            const QImage srgb = Sr::toSrgb(source, profile, &error);
            if (stopped()) return cancelledError;
            if (srgb.isNull()) return error.isEmpty() ? QStringLiteral("入力画像をsRGBへ変換できません。") : error;
            if (!writePng(srgb, input, &error, cancelled)) return error;
            return QString();
        }));
    }

    void continuePending() {
        updateControls(false);
        if (rerun && !externalBusy) { rerun = false; QTimer::singleShot(0, owner, &Controller::generate); }
    }

    void launch(const std::shared_ptr<QTemporaryDir>& folder, quint64 jobEpoch, quint64 revision, QSize size,
                const QString& python, const QString& script, const QString& selectedStrength,
                const QString& selectedDetail, int selectedSuppression, bool selectedCorrection) {
        auto* child = new QProcess(owner); process = child;
        stdoutBytes.clear(); stderrBytes.clear();
        auto environment = QProcessEnvironment::systemEnvironment(); environment.remove("LD_LIBRARY_PATH");
        child->setProcessEnvironment(environment);
        child->setWorkingDirectory(repositoryRoot());
        QObject::connect(child, &QProcess::readyReadStandardOutput, owner, [this, child] {
            const QByteArray bytes = child->readAllStandardOutput();
            if (process == child) stdoutBytes = (stdoutBytes + bytes).right(1024 * 1024);
        });
        QObject::connect(child, &QProcess::readyReadStandardError, owner, [this, child] {
            const QByteArray bytes = child->readAllStandardError();
            if (process == child) {
                stderrBytes = (stderrBytes + bytes).right(16384);
                watchdog.start();
                const QString progress = QString::fromUtf8(bytes).trimmed();
                if (!progress.isEmpty()) setStatus(progress.right(2000));
            }
        });
        QObject::connect(child, &QProcess::errorOccurred, owner, [this, child, jobEpoch](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart || process != child) return;
            process = nullptr; watchdog.stop(); child->deleteLater();
            if (jobEpoch == epoch) report(QStringLiteral("ベクターワーカーを起動できません: ") + child->errorString());
            continuePending();
        });
        QObject::connect(child, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), owner,
                         [this, child, folder, jobEpoch, revision, size, selectedCorrection](int code, QProcess::ExitStatus exitStatus) {
            if (process != child) { child->deleteLater(); return; }
            stdoutBytes += child->readAllStandardOutput(); stderrBytes += child->readAllStandardError();
            process = nullptr; watchdog.stop(); child->deleteLater();
            if (jobEpoch != epoch || externalBusy) { continuePending(); return; }
            if (revision != shapeRevision) { rerun = true; continuePending(); return; }
            if (code != 0 || exitStatus != QProcess::NormalExit) {
                report(QStringLiteral("ベクター変換に失敗しました。\n") + QString::fromUtf8(stderrBytes).trimmed().left(2000));
                continuePending(); return;
            }
            QJsonObject result;
            for (const auto& line : stdoutBytes.split('\n')) {
                const auto object = QJsonDocument::fromJson(line).object();
                if (object.contains("fill_svg") && object.contains("lines_svg")) result = object;
            }
            if (result.value("width").toInt() != size.width() || result.value("height").toInt() != size.height()) {
                report(QStringLiteral("ワーカーから受け取った画像サイズが入力と一致しません。")); continuePending(); return;
            }
            QString error;
            const auto fill = readSvgAsset(result.value("fill_svg").toString(), folder->filePath("cache"), &error);
            if (fill.isEmpty()) { report(error); continuePending(); return; }
            const auto lines = readSvgAsset(result.value("lines_svg").toString(), folder->filePath("cache"), &error);
            if (fill.isEmpty() || lines.isEmpty()) { report(error); continuePending(); return; }
            fillSvg = fill; linesSvg = lines; sourceSize = size;
            const auto stats = result.value("correction_stats").toObject();
            correctionSummary = selectedCorrection
                ? QStringLiteral("主線補正ON: 直線 %1、円 %2、楕円 %3、曲線の簡略化 %4。")
                    .arg(stats.value("lines").toInt()).arg(stats.value("circles").toInt())
                    .arg(stats.value("ellipses").toInt()).arg(stats.value("simplified_curves").toInt())
                : QStringLiteral("主線補正OFF（抽出した元の線）。");
            queueRender(); continuePending();
        });
        child->setProgram(python);
        QStringList arguments{script, "--input", folder->filePath("input.png"), "--cache", folder->filePath("cache"),
                              "--strength", selectedStrength, "--detail", selectedDetail, "--suppression", QString::number(selectedSuppression)};
        if (selectedCorrection) arguments << "--correct-lines";
        child->setArguments(arguments);
        setStatus(QStringLiteral("塗りと主線をベクター化しています…"));
        const qint64 timeoutSeconds = qMin<qint64>(10800, 300 + qint64(size.width()) * size.height() / 10000);
        watchdog.setInterval(int(timeoutSeconds * 1000));
        watchdog.start(); child->start(); updateControls(false);
    }

    void exportPng() {
        if (composedSvg.isEmpty() || busy()) return;
        QSize nativeSize, outputSize; QString error;
        const auto svg = exportDocument(&nativeSize, &error);
        if (svg.isEmpty() || !rasterSize(nativeSize, scale->value(), &outputSize, &error)) { report(error); return; }
        const QString path = QFileDialog::getSaveFileName(panel, QStringLiteral("PNGを保存"), suggestedName("png"), QStringLiteral("PNG画像 (*.png)"));
        if (path.isEmpty()) return;
        if (!maySaveTo(path, &error)) { report(error); return; }
        saving = true; saveSettings();
        setStatus(QStringLiteral("%1×%2pxのPNGを書き出しています…").arg(outputSize.width()).arg(outputSize.height())); updateControls(false);
        auto* watcher = new QFutureWatcher<QString>(owner);
        const auto saveEpoch = epoch;
        QObject::connect(watcher, &QFutureWatcher<QString>::finished, owner, [this, watcher, saveEpoch] {
            const auto error = watcher->result(); watcher->deleteLater(); saving = false;
            if (saveEpoch == epoch) {
                if (error.isEmpty()) setStatus(QStringLiteral("PNGを保存しました。")); else report(error);
            }
            updateControls(false);
        });
        watcher->setFuture(QtConcurrent::run([svg, outputSize, path] {
            QString error; const auto image = rasterizeSvg(svg, outputSize, &error);
            if (image.isNull()) return error;
            writePng(image, path, &error); return error;
        }));
    }
};

Controller::Controller(MainWindow* window, QVGraphicsView* view) : QObject(window) {
    setObjectName(QStringLiteral("vectorController"));
    d = std::make_unique<Private>(this, window, view);
}
Controller::~Controller() = default;
bool Controller::hasResult() const { return !d->image.isNull(); }
bool Controller::showingResult() const { return d->showing && hasResult(); }
bool Controller::isBusy() const { return d->busy(); }
QImage Controller::resultImage() const { return d->image; }

void Controller::showPanel() {
    if (d->fullscreen) { d->restorePanel = true; return; }
    bool visibleOnScreen = false;
    for (auto* screen : QGuiApplication::screens())
        visibleOnScreen |= screen->availableGeometry().intersects(d->panel->frameGeometry());
    if (!visibleOnScreen) d->panel->move(d->window->frameGeometry().topRight() - QPoint(d->panel->width(), 0));
    d->panel->show(); d->panel->raise(); d->panel->activateWindow(); d->updateControls();
}

void Controller::setFullscreen(bool fullscreen) {
    if (fullscreen == d->fullscreen) return;
    d->fullscreen = fullscreen;
    if (fullscreen) { d->restorePanel = d->panel->isVisible(); d->panel->hide(); }
    else if (d->restorePanel) { d->restorePanel = false; showPanel(); }
}

void Controller::generate() { d->debounce.stop(); d->startJob(); }

void Controller::cancel() {
    ++d->epoch; ++d->renderRevision;
    if (d->preparationCancelled) d->preparationCancelled->store(true, std::memory_order_relaxed);
    d->debounce.stop(); d->watchdog.stop(); d->rerun = false; d->renderPending = false;
    if (d->process) d->process->kill();
    d->setStatus(QStringLiteral("変換を中止しました。直前の変換結果は保持しています。"));
    d->updateControls(false);
}

void Controller::deactivate() {
    cancel();
    setShowingResult(false);
}

void Controller::setExternalBusy(bool busy) {
    if (busy == d->externalBusy) return;
    d->externalBusy = busy;
    if (busy) deactivate();
    d->updateControls();
}

void Controller::setShowingResult(bool showing) {
    showing = showing && hasResult() && !d->externalBusy;
    { QSignalBlocker block(d->showResult); d->showResult->setChecked(showing); }
    const bool changed = d->showing != showing; d->showing = showing;
    if (showing) emit previewReady(d->image, d->composedSvg, d->previewScale->value());
    else if (changed) emit originalRequested();
    emit stateChanged();
}

bool Controller::saveSvg(const QString& path, QString* error) {
    if (d->busy()) { if (error) *error = QStringLiteral("処理中です。完了後に保存してください。"); return false; }
    if (!d->maySaveTo(path, error)) return false;
    QSize size;
    const auto svg = d->exportDocument(&size, error);
    if (svg.isEmpty()) return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(svg) != svg.size() || !file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

bool Controller::savePng(const QString& path, double scale, QString* error) {
    if (d->busy()) { if (error) *error = QStringLiteral("処理中です。完了後に保存してください。"); return false; }
    if (!d->maySaveTo(path, error)) return false;
    QSize nativeSize, size;
    const auto svg = d->exportDocument(&nativeSize, error);
    if (svg.isEmpty() || !rasterSize(nativeSize, scale, &size, error)) return false;
    const auto image = rasterizeSvg(svg, size, error);
    return !image.isNull() && writePng(image, path, error);
}

bool Controller::eventFilter(QObject* object, QEvent* event) {
    if (d && object == d->panel && (event->type() == QEvent::Move || event->type() == QEvent::Resize || event->type() == QEvent::Hide || event->type() == QEvent::Close))
        d->saveSettings();
    return QObject::eventFilter(object, event);
}
}
