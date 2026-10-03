// SPDX-License-Identifier: GPL-3.0-or-later
#include "vector_controller.h"
#include "cancellable_save_file.h"
#include "memory_budget.h"
#include "generated_svg_renderer.h"
#include "vector_editor.h"
#include "mainwindow.h"
#include "qvgraphicsview.h"
#include "sr/color_pipeline.h"
#include "sr/sr_controller.h"

#include <QApplication>
#include <QCheckBox>
#include <QButtonGroup>
#include <QGridLayout>
#include <QToolButton>
#include <QColorSpace>
#include <QColorDialog>
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
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
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
    std::shared_ptr<EditDocument> editable;
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
Rendered previewDocument(const QByteArray& svg, QSize size, double previewScale) {
    Rendered output;
    output.svg = svg;
    output.error = svgMemoryError(quint64(svg.size()));
    if (!output.error.isEmpty()) return output;
    const double proxyScale = qMin(previewScale, qMin(
        std::sqrt(double(MaxProxyPixels) / (qint64(size.width()) * size.height())),
        double(MaxProxySide) / qMax(size.width(), size.height())));
    const QSize proxySize(qMax(1, int(std::floor(size.width() * proxyScale))),
                          qMax(1, int(std::floor(size.height() * proxyScale))));
    output.image = rasterizeSvg(svg, proxySize, &output.error);
    return output;
}

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
    return previewDocument(output.svg, size, previewScale);
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
    Sr::Controller* sr;
    Editor* editor;
    QCheckBox *editMode, *editShowLines, *editShowFill;
    QButtonGroup* editTools;
    QPushButton *editSelect, *editPen, *editEraser, *editFinish;
    QLabel *editRadiusLabel, *editColorSample, *explanation, *generationNote;
    QVector<QToolButton*> paletteButtons;
    QComboBox* editLayer;
    QDoubleSpinBox *brushRadius, *editLineWidth;
    QPushButton *editColor, *editDelete, *editUndo, *editRedo, *editDiscard;
    QWidget *editControls, *generationControls;
    QColor selectedEditColor = Qt::black;
    QByteArray editBaseSvg, editInitialSvg;
    QImage editBaseImage;
    bool edited = false, startingEditor = false;
    QPointer<QDialog> panel;
    QComboBox *strength, *detail, *color, *background, *lineMode, *maskGap, *inputSource;
    QDoubleSpinBox *width, *scale, *previewScale, *minLineLength, *joinDistance, *shapeTolerance;
    QSlider *opacity, *suppression, *branchStrength;
    QCheckBox *showFill, *grayFill, *showResult, *correctLines, *cleanLines;
    QPushButton *run, *stop, *svgSave, *pngSave;
    QLabel *status, *inputStatus;
    QTimer debounce, watchdog;
    QPointer<QProcess> process;
    QByteArray stdoutBytes, stderrBytes, fillSvg, linesSvg, composedSvg;
    QSize sourceSize;
    QImage image;
    QString correctionSummary, resultInputDescription;
    qint64 inputCacheKey = 0;
    QByteArray inputProfileIcc;
    std::shared_ptr<QTemporaryDir> workspace;
    std::shared_ptr<std::atomic_bool> preparationCancelled;
    quint64 epoch = 0, shapeRevision = 0, renderRevision = 0;
    bool preparing = false, rendering = false, rerun = false, renderPending = false;
    bool generated = false, showing = false, fullscreen = false, restorePanel = false, externalBusy = false, saving = false, loading = false;

    Private(Controller* controller, MainWindow* parent, QVGraphicsView* graphics, Sr::Controller* superResolution)
        : owner(controller), window(parent), view(graphics), sr(superResolution) {
        panel = new QDialog(parent, Qt::Tool);
        panel->setObjectName(QStringLiteral("vectorPanel"));
        panel->setWindowTitle(QStringLiteral("ベクター変換"));
        panel->setModal(false);
        panel->setAttribute(Qt::WA_DeleteOnClose, false);
        panel->installEventFilter(owner);
        auto* layout = new QVBoxLayout(panel);
        explanation = new QLabel(QStringLiteral("塗りと主線を別々にベクター化します。入力を下で選びます。既定はノイズ低減を含む超解像結果です。"), panel);
        explanation->setWordWrap(true);
        layout->addWidget(explanation);
        inputStatus = new QLabel(panel); inputStatus->setObjectName("vectorInputStatus");
        inputStatus->setWordWrap(true); layout->addWidget(inputStatus);
        auto* scroll = new QScrollArea(panel);
        scroll->setObjectName("vectorParameters");
        scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
        scroll->setMinimumHeight(280);
        auto* parameters = new QWidget(scroll);
        auto* parameterLayout = new QVBoxLayout(parameters);
        parameterLayout->setContentsMargins(0, 0, 0, 0);
        generationControls = new QWidget(parameters); generationControls->setObjectName("vectorGenerationControls");
        auto* form = new QFormLayout(generationControls);
        parameterLayout->addWidget(generationControls);
        scroll->setWidget(parameters);
        layout->addWidget(scroll, 1);
        auto combo = [this, form](const QString& name, const QString& text,
                                  const QList<QPair<QString, QString>>& items) {
            auto* control = new QComboBox(panel);
            control->setObjectName(name);
            for (const auto& item : items) control->addItem(item.first, item.second);
            form->addRow(text, control);
            return control;
        };
        inputSource = combo("vectorInputSource", QStringLiteral("変換元の画像"),
                            {{QStringLiteral("超解像結果を優先（なければ元画像）"), "auto"},
                             {QStringLiteral("加工前の元画像"), "original"}});
        inputSource->setToolTip(QStringLiteral("最新の超解像結果を実寸で使います。元画像との表示切替や画面の拡大率には影響されません。ノイズ低減はSR設定で指定して超解像を実行してください。"));
        lineMode = combo("vectorLineMode", QStringLiteral("主線の抽出方式"),
                         {{QStringLiteral("暗い主線（線画・イラスト）"), "dark"},
                          {QStringLiteral("色の境界（ロゴ・色面の輪郭）"), "color"}});
        lineMode->setToolTip(QStringLiteral("色の境界は明るさが近い色同士も検出します。太い線は両側の輪郭になるため、線の中心を残す場合は「暗い主線」を使います。"));
        strength = combo("vectorStrength", QStringLiteral("主線の抽出強度"),
                         {{QStringLiteral("弱：濃い線を中心に"), "weak"}, {QStringLiteral("標準"), "balanced"}, {QStringLiteral("強：薄い線も拾う"), "strong"}});
        maskGap = combo("vectorMaskGap", QStringLiteral("線抽出の隙間補正"),
                        {{QStringLiteral("OFF（元の抽出）"), "0"}, {QStringLiteral("1px（小さな隙間）"), "1"},
                         {QStringLiteral("2px（強め）"), "2"}});
        maskGap->setToolTip(QStringLiteral("暗い主線の中にできた細い隙間を埋め、梯子状の分岐を抑えます。近接した別の線や小さい文字の穴も結合するため、必要な画像で1pxから試してください。色の境界には適用しません。"));
        detail = combo("vectorDetail", QStringLiteral("曲線の細かさ"),
                       {{QStringLiteral("細部優先（0.3px）"), "fine"}, {QStringLiteral("標準（1px）"), "balanced"}, {QStringLiteral("簡略化（3px）"), "simple"}});
        correctLines = new QCheckBox(QStringLiteral("主線を補正（直線・円・楕円・滑らかな曲線）"), panel);
        correctLines->setObjectName("vectorCorrectLines");
        correctLines->setToolTip(QStringLiteral("元の形からのずれを抑えながら、主線を直線・円・楕円や少ないベジエ曲線で近似します。オフにすると補正前の線に戻ります。塗りには適用しません。"));
        form->addRow(correctLines);
        auto pixels = [this, form](const QString& name, const QString& label, double minimum, double maximum, double step) {
            auto* spin = new QDoubleSpinBox(panel);
            spin->setObjectName(name); spin->setRange(minimum, maximum); spin->setSingleStep(step); spin->setDecimals(2);
            spin->setSuffix(QStringLiteral(" px")); form->addRow(label, spin); return spin;
        };
        shapeTolerance = pixels("vectorShapeTolerance", QStringLiteral("円・楕円の許容誤差"), .25, 100, .5);
        shapeTolerance->setToolTip(QStringLiteral("入力画像のピクセル単位。値を上げると揺らいだ閉輪郭も円・楕円に近似しやすくなります。大きすぎる値は形状を変えるため、表示を比較してください。直線・曲線の補正誤差とは独立しています。"));
        cleanLines = new QCheckBox(QStringLiteral("主線を整理（短いノイズ除去・途切れ接続）"), panel);
        cleanLines->setObjectName("vectorCleanLines");
        cleanLines->setToolTip(QStringLiteral("短い孤立線や、滑らかな長い主線に直交する短い横枝を除き、方向の揃った近い端点を接続します。小さな文字や意図した線も消える場合があるため、ON/OFFで比較してください。"));
        form->addRow(cleanLines);
        minLineLength = pixels("vectorMinLineLength", QStringLiteral("短い線の基準長"), 0, 1000, 1);
        minLineLength->setToolTip(QStringLiteral("入力画像のピクセル単位。孤立線はこの長さ未満を除去し、横枝の長さ判定にも使用します。0で短線の除去を無効にし、途切れの接続だけを行います。"));
        joinDistance = pixels("vectorJoinDistance", QStringLiteral("途切れの接続距離"), 0, 200, 1);
        joinDistance->setToolTip(QStringLiteral("この距離以内で、接線方向が揃い、接続先が一つに定まる端点だけをつなぎます。0で接続を無効にします。"));
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
        branchStrength = percentageSlider("vectorBranchStrength", QStringLiteral("短い横枝の除去"), 1);
        branchStrength->setToolTip(QStringLiteral("長い主線にほぼ直交する短い枝を除去する強さ。0で無効にします。長い枝、角、閉じた輪郭は保持します。"));
        width = pixels("vectorWidth", QStringLiteral("線幅（入力画像基準）"), .3, 3.0, .1);
        width->setDecimals(1);
        opacity = percentageSlider("vectorOpacity", QStringLiteral("主線の濃さ"), 1);
        suppression = percentageSlider("vectorSuppression", QStringLiteral("塗り側の元線を弱める量"), 5);
        color = combo("vectorColor", QStringLiteral("主線の色"),
                      {{QStringLiteral("透明（主線を非表示）"), "transparent"}, {QStringLiteral("白"), "white"},
                       {QStringLiteral("黒"), "black"}, {QStringLiteral("入力画像から推定"), "source"}});
        background = combo("vectorBackground", QStringLiteral("背景の色"),
                           {{QStringLiteral("透明"), "transparent"}, {QStringLiteral("白"), "white"}, {QStringLiteral("黒"), "black"}});
        showFill = new QCheckBox(QStringLiteral("塗りを表示"), panel); showFill->setObjectName("vectorShowFill");
        grayFill = new QCheckBox(QStringLiteral("塗りをグレースケール化"), panel); grayFill->setObjectName("vectorGrayFill");
        form->addRow(showFill); form->addRow(grayFill);
        previewScale = new QDoubleSpinBox(panel);
        previewScale->setObjectName("vectorPreviewScale"); previewScale->setRange(.01, 8); previewScale->setSingleStep(.1);
        previewScale->setSuffix(QStringLiteral(" 倍")); form->addRow(QStringLiteral("受け渡し画像の倍率"), previewScale);
        previewScale->setToolTip(QStringLiteral("Ctrl＋ドラッグで渡す画像の倍率です。画面のプレビューは倍率に関係なくSVGから描画します。"));
        editor = new Editor(view);
        editor->setPanelWidget(panel);
        editMode = new QCheckBox(QStringLiteral("編集モード（Space＋ドラッグで画像移動）"), panel);
        editMode->setObjectName("vectorEditMode"); layout->insertWidget(2, editMode);
        editControls = new QWidget(parameters); editControls->setObjectName("vectorEditControls");
        parameterLayout->addWidget(editControls);
        auto* edits = new QFormLayout(editControls); edits->setContentsMargins(0, 0, 0, 0);
        editLayer = new QComboBox(editControls); editLayer->setObjectName("vectorEditLayer");
        editLayer->addItem(QStringLiteral("主線（ベジエ編集）"), "lines");
        editLayer->addItem(QStringLiteral("色面（ドラッグで境界を変形）"), "fill");
        edits->addRow(QStringLiteral("編集するレイヤー"), editLayer);
        auto* visibility = new QHBoxLayout;
        editShowLines = new QCheckBox(QStringLiteral("主線を表示"), editControls); editShowLines->setObjectName("vectorEditShowLines");
        editShowFill = new QCheckBox(QStringLiteral("色面を表示"), editControls); editShowFill->setObjectName("vectorEditShowFill");
        visibility->addWidget(editShowLines); visibility->addWidget(editShowFill);
        edits->addRow(QStringLiteral("レイヤーの表示"), visibility);
        auto* toolbox = new QHBoxLayout;
        editTools = new QButtonGroup(editControls); editTools->setExclusive(true);
        auto toolButton = [&](const QString& text, const char* name, Editor::Tool tool) {
            auto* button = new QPushButton(text, editControls); button->setObjectName(name); button->setCheckable(true);
            button->setStyleSheet("QPushButton:checked { background: #d5e9ff; color: #103f71; border: 2px solid #3484e4; border-radius: 3px; padding: 4px; }");
            editTools->addButton(button, int(tool)); toolbox->addWidget(button); return button;
        };
        editSelect = toolButton(QStringLiteral("選択・変形"), "vectorToolSelect", Editor::Tool::Select);
        editPen = toolButton(QStringLiteral("ペン"), "vectorToolPen", Editor::Tool::Pen);
        editEraser = toolButton(QStringLiteral("消しゴム"), "vectorToolEraser", Editor::Tool::Eraser);
        editSelect->setChecked(true);
        editPen->setToolTip(QStringLiteral("クリックで節点を追加、ドラッグでハンドルを調整。Enter／ダブルクリックで確定、Escで中止します。"));
        editEraser->setToolTip(QStringLiteral("選択中のレイヤーで、触れたパス全体を削除します。非表示の図形は対象外です。"));
        edits->addRow(QStringLiteral("ツールボックス"), toolbox);
        editFinish = new QPushButton(QStringLiteral("線を確定（Enter）"), editControls); editFinish->setObjectName("vectorPenFinish");
        edits->addRow(editFinish);
        auto* palette = new QWidget(editControls); palette->setObjectName("vectorPalette");
        auto* paletteLayout = new QGridLayout(palette); paletteLayout->setContentsMargins(0, 0, 0, 0);
        const QStringList colors{"#000000", "#ffffff", "#808080", "#804020", "#e53935", "#fb8c00",
                                 "#fdd835", "#43a047", "#00acc1", "#1e88e5", "#8e24aa", "#ec407a"};
        for (int i=0; i<colors.size(); ++i) {
            const QColor swatch(colors[i]);
            auto* button = new QToolButton(palette); button->setObjectName(QStringLiteral("vectorPalette%1").arg(i));
            button->setFixedSize(32, 28); button->setCheckable(true); button->setProperty("color", swatch);
            button->setToolTip(colors[i]); button->setAccessibleName(QStringLiteral("色 %1").arg(colors[i]));
            button->setStyleSheet(QStringLiteral("QToolButton { background: %1; border: 1px solid #888; border-radius: 3px; } QToolButton:checked { border: 3px solid #3484e4; }").arg(colors[i]));
            paletteLayout->addWidget(button, i/6, i%6); paletteButtons.append(button);
            QObject::connect(button, &QToolButton::clicked, owner, [this, swatch] { chooseEditColor(swatch); });
        }
        edits->addRow(QStringLiteral("色パレット"), palette);
        editColorSample = new QLabel(editControls); editColorSample->setObjectName("vectorEditColorSample");
        editColorSample->setAlignment(Qt::AlignCenter); editColorSample->setMinimumHeight(24);
        edits->addRow(QStringLiteral("描画／変更する色"), editColorSample);
        brushRadius = new QDoubleSpinBox(editControls); brushRadius->setObjectName("vectorBrushRadius");
        brushRadius->setRange(1, 10000); brushRadius->setValue(40); brushRadius->setSuffix(" px");
        editRadiusLabel = new QLabel(QStringLiteral("境界変形の範囲"), editControls);
        edits->addRow(editRadiusLabel, brushRadius);
        editLineWidth = new QDoubleSpinBox(editControls); editLineWidth->setObjectName("vectorEditLineWidth");
        editLineWidth->setRange(.1, 1000); editLineWidth->setValue(2.6); editLineWidth->setSuffix(" px");
        edits->addRow(QStringLiteral("追加する線の幅"), editLineWidth);
        auto* editButtons = new QHBoxLayout;
        auto button = [&](const QString& text, const char* name) {
            auto* b = new QPushButton(text, editControls); b->setObjectName(name); editButtons->addWidget(b); return b;
        };
        editColor = button(QStringLiteral("その他の色…"), "vectorEditColor");
        editDelete = button(QStringLiteral("削除"), "vectorEditDelete");
        editUndo = button(QStringLiteral("戻す"), "vectorEditUndo");
        editRedo = button(QStringLiteral("やり直す"), "vectorEditRedo");
        edits->addRow(editButtons);
        auto* help = new QLabel(QStringLiteral("ペン: 節点を順にクリック。ドラッグで曲線にし、Enterまたはダブルクリックで確定します。Escで未確定の線を中止。\n選択・変形: 節点やハンドル、色面の境界をドラッグ。消しゴム: 選択中のレイヤーのパス全体を削除。\nSpace＋ドラッグで画像移動、Ctrl＋Zで取り消し。非表示のレイヤーはPNGにも出力しません。"), editControls);
        help->setWordWrap(true); edits->addRow(help);
        editDiscard = new QPushButton(QStringLiteral("手編集を破棄して生成結果へ戻す"), panel);
        editDiscard->setObjectName("vectorEditDiscard"); parameterLayout->addWidget(editDiscard); parameterLayout->addStretch();
        generationNote = new QLabel(QStringLiteral("透明背景は塗りの白い部分を消しません。主線を透明にしても塗り側の元線は残ります。"), panel);
        generationNote->setWordWrap(true); layout->addWidget(generationNote);
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
        panel->setMinimumWidth(460);
        panel->resize(500, 720);
        loadSettings();
        debounce.setSingleShot(true); debounce.setInterval(350);
        QObject::connect(&debounce, &QTimer::timeout, owner, &Controller::generate);
        watchdog.setSingleShot(true); watchdog.setInterval(180000);
        QObject::connect(&watchdog, &QTimer::timeout, owner, [this] {
            owner->cancel(); report(QStringLiteral("ベクター変換が制限時間を超えたため中止しました。"));
        });
        for (auto* control : {strength, detail, lineMode, maskGap, inputSource})
            QObject::connect(control, &QComboBox::currentIndexChanged, owner, [this] { shapeChanged(); });
        QObject::connect(suppression, &QSlider::valueChanged, owner, [this] { shapeChanged(); });
        QObject::connect(correctLines, &QCheckBox::toggled, owner, [this] { shapeChanged(); });
        QObject::connect(cleanLines, &QCheckBox::toggled, owner, [this] { shapeChanged(); });
        QObject::connect(branchStrength, &QSlider::valueChanged, owner, [this] { shapeChanged(); });
        for (auto* control : {minLineLength, joinDistance, shapeTolerance})
            QObject::connect(control, &QDoubleSpinBox::valueChanged, owner, [this] { shapeChanged(); });
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
        QObject::connect(editMode, &QCheckBox::toggled, owner, &Controller::setEditing);
        QObject::connect(editLayer, &QComboBox::currentIndexChanged, owner, [this] {
            editor->setLayer(editLayer->currentData() == "fill" ? EditDocument::Layer::Fill : EditDocument::Layer::Lines);
            updateControls(false);
        });
        QObject::connect(editTools, &QButtonGroup::idClicked, owner, [this](int id) { editor->setTool(Editor::Tool(id)); });
        QObject::connect(editFinish, &QPushButton::clicked, owner, [this] { editor->finishPen(); view->setFocus(); });
        QObject::connect(editShowLines, &QCheckBox::toggled, owner, [this](bool visible) { editor->setLayerVisible(EditDocument::Layer::Lines, visible); });
        QObject::connect(editShowFill, &QCheckBox::toggled, owner, [this](bool visible) { editor->setLayerVisible(EditDocument::Layer::Fill, visible); });
        QObject::connect(brushRadius, &QDoubleSpinBox::valueChanged, editor, &Editor::setBrushRadius);
        QObject::connect(editLineWidth, &QDoubleSpinBox::valueChanged, editor, &Editor::setLineWidth);
        QObject::connect(editColor, &QPushButton::clicked, owner, [this] {
            const auto chosen = QColorDialog::getColor(selectedEditColor, panel, QStringLiteral("選択した図形／追加する線の色"));
            if (chosen.isValid()) chooseEditColor(chosen);
        });
        QObject::connect(editDelete, &QPushButton::clicked, editor, &Editor::deleteSelection);
        QObject::connect(editUndo, &QPushButton::clicked, editor, &Editor::undo);
        QObject::connect(editRedo, &QPushButton::clicked, editor, &Editor::redo);
        QObject::connect(editDiscard, &QPushButton::clicked, owner, [this] {
            if (QMessageBox::question(panel, QStringLiteral("手編集を破棄"), QStringLiteral("主線・色面への手編集を破棄し、編集前の生成結果へ戻しますか？"),
                                      QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes) owner->discardEdits();
        });
        QObject::connect(editor, &Editor::changed, owner, [this](const QByteArray& svg) {
            edited = svg != editInitialSvg; composedSvg = svg; queueRender(); updateControls(false);
        });
        QObject::connect(editor, &Editor::stateChanged, owner, [this] { updateControls(false); });
        QObject::connect(editor, &Editor::message, owner, [this](const QString& text) { setStatus(text); });
        QObject::connect(&view->getImageCore(), &QVImageCore::sourceChanging, owner, [this] { loading = true; invalidate(); });
        QObject::connect(view, &QVGraphicsView::fileChanged, owner, [this] { loading = false; updateControls(); });
        QObject::connect(view, &QVGraphicsView::vectorRenderingFailed, owner, [this](const QString& error) { report(error); });
        QObject::connect(sr, &Sr::Controller::stateChanged, owner, [this] { updateInputStatus(); });
        updateControls();
    }

    ~Private() {
        editor->end();
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
        selected(inputSource, "inputSource", "auto");
        selected(lineMode, "lineMode", "dark");
        selected(maskGap, "maskGap", "0");
        selected(color, "color", "source"); selected(background, "background", "white");
        width->setValue(settings.value("vector/width", 2.6).toDouble());
        opacity->setValue(settings.value("vector/opacity", 85).toInt());
        suppression->setValue(qRound(settings.value("vector/suppression", 30).toDouble() / 5));
        showFill->setChecked(settings.value("vector/showFill", false).toBool());
        grayFill->setChecked(settings.value("vector/grayFill", false).toBool());
        correctLines->setChecked(settings.value("vector/correctLines", false).toBool());
        cleanLines->setChecked(settings.value("vector/cleanLines", false).toBool());
        minLineLength->setValue(settings.value("vector/minLineLength", 6.).toDouble());
        joinDistance->setValue(settings.value("vector/joinDistance", 4.).toDouble());
        branchStrength->setValue(settings.value("vector/branchStrength", 50).toInt());
        shapeTolerance->setValue(settings.value("vector/shapeTolerance", 2.).toDouble());
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
        settings.setValue("vector/inputSource", inputSource->currentData());
        settings.setValue("vector/lineMode", lineMode->currentData());
        settings.setValue("vector/maskGap", maskGap->currentData());
        settings.setValue("vector/cleanLines", cleanLines->isChecked());
        settings.setValue("vector/minLineLength", minLineLength->value());
        settings.setValue("vector/joinDistance", joinDistance->value());
        settings.setValue("vector/branchStrength", branchStrength->value());
        settings.setValue("vector/shapeTolerance", shapeTolerance->value());
    }

    struct Input {
        QImage image;
        Sr::Profile profile;
        QString description;
    };

    Input selectedInput() const {
        if (inputSource->currentData().toString() == "auto" && sr->hasResult() && !sr->hasAnimation())
            return {sr->resultImage(), {Sr::srgbProfile(), QStringLiteral("sRGB"), {}, false},
                    QStringLiteral("超解像結果（%1回・元画像比%2倍）").arg(sr->resultPasses()).arg(sr->resultScale(), 0, 'g', 6)};
        return {view->getImageCore().getSourceImage(), view->getImageCore().getSourceProfile(), QStringLiteral("加工前の元画像")};
    }

    void updateInputStatus() {
        const auto input = selectedInput();
        inputStatus->setText(input.image.isNull() ? QStringLiteral("入力: 静止画を開いてください。")
            : QStringLiteral("次の変換入力: %1 · %2×%3px").arg(input.description).arg(input.image.width()).arg(input.image.height()));
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
    bool busy() const { return workerBusy() || rendering || saving || debounce.isActive() || rerun || editor->isBusy(); }
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
        if (editor->hasPendingPen()) { if (error) *error = QStringLiteral("描画中の線をEnterで確定するか、Escで中止してから保存してください。"); return {}; }
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

    void chooseEditColor(const QColor& chosen) {
        selectedEditColor = chosen;
        editor->setColor(chosen);
        updateControls(false);
    }

    void updateControls(bool updateMessage = true) {
        const QString unavailable = eligibilityError();
        const bool manual = edited || editor->active() || startingEditor;
        const bool canGenerate = !externalBusy && !manual;
        run->setEnabled(unavailable.isEmpty() && !workerBusy() && !saving && !manual);
        stop->setEnabled(workerBusy() || rendering || debounce.isActive());
        showResult->setEnabled(!image.isNull() && !externalBusy);
        svgSave->setEnabled(!composedSvg.isEmpty() && !busy() && !editor->hasPendingPen());
        pngSave->setEnabled(!composedSvg.isEmpty() && !busy() && !editor->hasPendingPen());
        for (auto* control : {strength, detail, color, background, lineMode, inputSource}) control->setEnabled(canGenerate);
        maskGap->setEnabled(canGenerate && lineMode->currentData().toString() == "dark");
        showFill->setEnabled(canGenerate); previewScale->setEnabled(!externalBusy && !startingEditor); scale->setEnabled(!externalBusy);
        correctLines->setEnabled(canGenerate);
        cleanLines->setEnabled(canGenerate);
        shapeTolerance->setEnabled(canGenerate && correctLines->isChecked());
        minLineLength->setEnabled(canGenerate && cleanLines->isChecked());
        joinDistance->setEnabled(canGenerate && cleanLines->isChecked());
        branchStrength->parentWidget()->setEnabled(canGenerate && cleanLines->isChecked() && minLineLength->value() > 0);
        suppression->parentWidget()->setEnabled(canGenerate && showFill->isChecked()); grayFill->setEnabled(canGenerate && showFill->isChecked());
        const bool linesVisible = color->currentData().toString() != "transparent";
        width->setEnabled(canGenerate && linesVisible); opacity->parentWidget()->setEnabled(canGenerate && linesVisible);
        editMode->setEnabled(!externalBusy && !startingEditor && !editor->isBusy() && ((!image.isNull() && !busy()) || editor->active()));
        { QSignalBlocker block(editMode); editMode->setChecked(editor->active() || startingEditor); }
        const bool editingPanel = editor->active() || startingEditor;
        editControls->setVisible(editingPanel);
        generationControls->setVisible(!editingPanel);
        explanation->setVisible(!editingPanel); inputStatus->setVisible(!editingPanel); generationNote->setVisible(!editingPanel);
        run->setVisible(!editingPanel); stop->setVisible(!editingPanel);
        panel->setWindowTitle(editingPanel ? QStringLiteral("ベクター編集") : QStringLiteral("ベクター変換"));
        if (editor->active()) {
            QSignalBlocker block(editLayer);
            editLayer->setCurrentIndex(editLayer->findData(editor->layer() == EditDocument::Layer::Lines ? "lines" : "fill"));
        }
        const bool lineLayer = editLayer->currentData() == "lines";
        const bool erasing = editor->tool() == Editor::Tool::Eraser;
        editPen->setEnabled(lineLayer); editLineWidth->setEnabled(lineLayer && !erasing);
        brushRadius->setEnabled(erasing || !lineLayer);
        editRadiusLabel->setText(erasing ? QStringLiteral("消しゴムの半径") : QStringLiteral("境界変形の範囲"));
        if (auto* button = editTools->button(int(editor->tool()))) button->setChecked(true);
        editFinish->setVisible(editor->tool() == Editor::Tool::Pen);
        editFinish->setEnabled(editor->hasPendingPen());
        if (editor->active() && !editor->isBusy()) {
            QSignalBlocker linesBlock(editShowLines), fillBlock(editShowFill);
            editShowLines->setChecked(editor->layerVisible(EditDocument::Layer::Lines));
            editShowFill->setChecked(editor->layerVisible(EditDocument::Layer::Fill));
        }
        for (auto* button : paletteButtons) button->setChecked(button->property("color").value<QColor>() == selectedEditColor);
        editColorSample->setText(selectedEditColor.name());
        editColorSample->setStyleSheet(QStringLiteral("background: %1; color: %2; border: 1px solid #888; border-radius: 3px;")
            .arg(selectedEditColor.name(), selectedEditColor.lightnessF() > .5 ? "black" : "white"));
        editDelete->setEnabled(editor->selectedPath() >= 0);
        editControls->setEnabled(editor->active() && !editor->isBusy());
        editUndo->setEnabled((editor->document().canUndo() || editor->hasPendingPen()) && !editor->isBusy()); editRedo->setEnabled(editor->document().canRedo() && !editor->isBusy());
        editDiscard->setVisible(edited); editDiscard->setEnabled(edited && !busy() && !externalBusy);
        if (updateMessage && !unavailable.isEmpty()) setStatus(unavailable);
        else if (updateMessage && image.isNull() && !busy()) setStatus(QStringLiteral("「生成・更新」で変換を開始します。設定の変更は生成後に自動反映します。"));
        updateInputStatus();
        emit owner->stateChanged();
    }

    void shapeChanged() {
        if (edited || editor->active() || startingEditor) { updateControls(false); return; }
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
        editor->reset(); edited = false; startingEditor = false;
        editBaseSvg.clear(); editInitialSvg.clear(); editBaseImage = {};
        fillSvg.clear(); linesSvg.clear(); composedSvg.clear(); image = {}; sourceSize = {};
        workspace.reset(); generated = false; showing = false;
        inputCacheKey = 0; inputProfileIcc.clear();
        correctionSummary.clear(); resultInputDescription.clear();
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

    void beginEditing() {
        if (editor->active() || startingEditor) return;
        if (busy() || externalBusy || image.isNull()) { updateControls(false); return; }
        const auto memoryError = EditDocument::memoryError(
            qMax(quint64(composedSvg.size()), quint64(fillSvg.size()) + quint64(linesSvg.size())), availableMemoryBytes());
        if (!memoryError.isEmpty()) { report(memoryError); return; }
        if (!edited) { editBaseSvg = composedSvg; editBaseImage = image; }
        const auto editEpoch = epoch;
        auto look = appearance(); look.showFill = true;
        if (look.color == "transparent") look.color = "source";
        if (look.opacity == 0) look.opacity = 100;
        const auto fill = fillSvg, lines = linesSvg, svg = composedSvg;
        const auto size = sourceSize;
        const auto requestedScale = previewScale->value();
        const bool existing = edited;
        startingEditor = true; rendering = true;
        setStatus(QStringLiteral("編集用の主線・色面を準備しています…"));
        updateControls(false);
        auto* watcher = new QFutureWatcher<Rendered>(owner);
        QObject::connect(watcher, &QFutureWatcher<Rendered>::finished, owner, [this, watcher, editEpoch] {
            auto result = watcher->result(); watcher->deleteLater();
            rendering = false; startingEditor = false;
            if (epoch != editEpoch || externalBusy) { updateControls(false); return; }
            QString error = result.error;
            if (error.isEmpty() && !editor->beginPrepared(result.svg, result.editable, &error)) {
                if (error.isEmpty()) error = QStringLiteral("編集用SVGを読み込めません。");
            }
            if (!error.isEmpty()) { report(error); updateControls(false); return; }
            if (!edited) editInitialSvg = editor->svg();
            composedSvg = result.svg; image = result.image;
            editor->setLayer(editLayer->currentData() == "fill" ? EditDocument::Layer::Fill : EditDocument::Layer::Lines);
            editor->setBrushRadius(brushRadius->value());
            editor->setLineWidth(editLineWidth->value());
            owner->setShowingResult(true);
            window->cancelSlideshow();
            updateControls(false);
            if (auto* scroll = panel->findChild<QScrollArea*>("vectorParameters")) scroll->verticalScrollBar()->setValue(0);
            setStatus(QStringLiteral("編集モード: パレットとツールで編集できます。Space＋ドラッグで移動、左右キーのファイル移動は停止中です。"));
            view->setFocus(); emit owner->resultReady();
        });
        watcher->setFuture(QtConcurrent::run([fill, lines, svg, size, look, requestedScale, existing] {
            auto result = existing ? previewDocument(svg, size, requestedScale) : compose(fill, lines, size, look, requestedScale);
            if (result.error.isEmpty()) {
                result.editable = std::make_shared<EditDocument>();
                if (!result.editable->load(result.svg, &result.error)) result.editable.reset();
            }
            return result;
        }));
    }

    void endEditing() {
        if (startingEditor) { owner->cancel(); startingEditor = false; }
        if (!editor->active()) return;
        editor->end();
        // Leaving a session without edits restores the original layer visibility.
        if (!edited && !editBaseSvg.isEmpty()) {
            ++renderRevision; renderPending = false;
            composedSvg = editBaseSvg; image = editBaseImage;
            editBaseSvg.clear(); editInitialSvg.clear(); editBaseImage = {};
            if (showing) emit owner->previewReady(image, composedSvg, previewScale->value());
        }
        updateControls(false);
        setStatus(edited ? QStringLiteral("編集結果を保持しています。SVG／PNGで保存できます。再生成するには手編集を破棄してください。")
                         : QStringLiteral("編集モードを終了しました。"));
    }

    void startRender() {
        const auto renderEpoch = epoch, revision = renderRevision;
        const auto fill = fillSvg, lines = linesSvg;
        const auto size = sourceSize;
        const auto inputDescription = resultInputDescription;
        const auto look = appearance();
        const bool manual = edited || editor->active();
        const auto editedSvg = composedSvg;
        const double requestedScale = previewScale->value();
        rendering = true;
        auto* watcher = new QFutureWatcher<Rendered>(owner);
        QObject::connect(watcher, &QFutureWatcher<Rendered>::finished, owner, [this, watcher, renderEpoch, revision, inputDescription] {
            auto result = watcher->result(); watcher->deleteLater(); rendering = false;
            if (renderEpoch == epoch && revision == renderRevision && !externalBusy) {
                if (!result.error.isEmpty()) report(result.error);
                else {
                    image = result.image; composedSvg = result.svg;
                    if (showResult->isChecked()) { showing = true; emit owner->previewReady(image, composedSvg, previewScale->value()); }
                    if (editor->active())
                        setStatus(QStringLiteral("編集結果: %1×%2px。表示中のレイヤーをPNGに保存します。\nSVGは非表示の図形も含めて保持します。Space＋ドラッグで画像移動。")
                                  .arg(sourceSize.width()).arg(sourceSize.height()));
                    else setStatus(QStringLiteral("SVG: %1×%2px。拡大時もベクターから描画します。\n変換に使った画像: %3\n%4")
                                   .arg(sourceSize.width()).arg(sourceSize.height()).arg(inputDescription, correctionSummary));
                    emit owner->resultReady();
                }
            }
            if (renderPending && !fillSvg.isEmpty() && !externalBusy) { renderPending = false; startRender(); }
            updateControls(false);
        });
        watcher->setFuture(QtConcurrent::run([fill, lines, size, look, requestedScale, manual, editedSvg] {
            return manual ? previewDocument(editedSvg, size, requestedScale) : compose(fill, lines, size, look, requestedScale);
        }));
        updateControls(false);
    }

    void startJob() {
        if (edited || editor->active() || startingEditor) {
            report(QStringLiteral("手編集を保持しています。再生成するには編集モードを終了し、手編集を破棄してください。")); return;
        }
        const QString unavailable = eligibilityError();
        if (!unavailable.isEmpty()) { report(unavailable); return; }
        if (workerBusy()) { rerun = true; return; }
        // Use full-resolution sRGB SR pixels, never the monitor-converted view
        // or our own rasterized vector preview. QImage copies retain this input
        // while asynchronous preparation is running.
        const auto input = selectedInput();
        const auto source = input.image;
        const auto profile = input.profile;
        const auto memoryError = preparationMemoryError(source.size());
        if (!memoryError.isEmpty()) { report(memoryError); return; }
        const QString root = repositoryRoot();
        const QString python = qEnvironmentVariable("QVIEWSR_VECTOR_PYTHON", root + "/.local/vector-probe-venv/bin/python");
        const QString script = qEnvironmentVariable("QVIEWSR_VECTOR_WORKER", root + "/tools/vector/vector_worker.py");
        if (!QFileInfo(python).isExecutable() || !QFileInfo(script).isFile()) {
            report(QStringLiteral("ベクター変換の実行環境が見つかりません。セットアップ手順を確認してください。")); return;
        }
        const bool changedInput = inputCacheKey != source.cacheKey() || inputProfileIcc != profile.icc;
        if (changedInput) {
            // A new SR pass can have the same dimensions. Never reuse its
            // predecessor's input.png, and do not publish an older render.
            ++epoch; ++renderRevision; renderPending = false;
            workspace.reset();
            inputCacheKey = source.cacheKey(); inputProfileIcc = profile.icc;
        }
        if (!workspace) workspace = std::make_shared<QTemporaryDir>(QDir::tempPath() + "/qviewsr-vector-XXXXXX");
        if (!workspace->isValid()) { report(QStringLiteral("作業用フォルダーを作成できません。")); return; }
        generated = true; rerun = false; preparing = true;
        const auto cancelled = std::make_shared<std::atomic_bool>(false);
        preparationCancelled = cancelled;
        if (image.isNull() || changedInput) { QSignalBlocker block(showResult); showResult->setChecked(true); }
        const auto jobEpoch = epoch, revision = shapeRevision;
        const auto folder = workspace;
        const QSize size = source.size();
        const QString inputDescription = input.description;
        const QString selectedStrength = strength->currentData().toString();
        const QString selectedDetail = detail->currentData().toString();
        const int selectedSuppression = suppression->value() * 5;
        const bool selectedCorrection = correctLines->isChecked();
        QStringList selectedLineOptions{"--line-mode", lineMode->currentData().toString(),
            "--mask-gap", maskGap->currentData().toString(),
            "--shape-tolerance", QString::number(shapeTolerance->value())};
        if (cleanLines->isChecked()) selectedLineOptions << "--clean-lines"
            << "--min-line-length" << QString::number(minLineLength->value())
            << "--join-distance" << QString::number(joinDistance->value())
            << "--branch-strength" << QString::number(branchStrength->value());
        setStatus(QStringLiteral("入力画像の色と作業ファイルを準備しています…"));
        updateControls(false);
        auto* watcher = new QFutureWatcher<QString>(owner);
        QObject::connect(watcher, &QFutureWatcher<QString>::finished, owner,
                         [this, watcher, folder, cancelled, jobEpoch, revision, size, inputDescription, python, script, selectedStrength, selectedDetail, selectedSuppression, selectedCorrection, selectedLineOptions] {
            const QString error = watcher->result(); watcher->deleteLater(); preparing = false;
            if (preparationCancelled == cancelled) preparationCancelled.reset();
            if (jobEpoch != epoch || externalBusy) { continuePending(); return; }
            if (!error.isEmpty()) { report(error); continuePending(); return; }
            if (revision != shapeRevision) { rerun = true; continuePending(); return; }
            launch(folder, jobEpoch, revision, size, inputDescription, python, script, selectedStrength, selectedDetail, selectedSuppression, selectedCorrection, selectedLineOptions);
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

    void launch(const std::shared_ptr<QTemporaryDir>& folder, quint64 jobEpoch, quint64 revision, QSize size, const QString& inputDescription,
                const QString& python, const QString& script, const QString& selectedStrength,
                const QString& selectedDetail, int selectedSuppression, bool selectedCorrection, const QStringList& selectedLineOptions) {
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
                         [this, child, folder, jobEpoch, revision, size, inputDescription, selectedCorrection](int code, QProcess::ExitStatus exitStatus) {
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
            editor->reset();
            fillSvg = fill; linesSvg = lines; sourceSize = size; resultInputDescription = inputDescription;
            const auto stats = result.value("correction_stats").toObject();
            correctionSummary = selectedCorrection
                ? QStringLiteral("主線補正ON: 直線 %1、円 %2、楕円 %3、曲線の簡略化 %4。")
                    .arg(stats.value("lines").toInt()).arg(stats.value("circles").toInt())
                    .arg(stats.value("ellipses").toInt()).arg(stats.value("simplified_curves").toInt())
                : QStringLiteral("形状補正OFF。");
            const auto cleaned = result.value("cleanup_stats").toObject();
            if (result.value("clean_lines").toBool()) correctionSummary += QStringLiteral("\n主線整理ON: 孤立線 %1、横枝 %2を除去、%3か所を接続。")
                .arg(cleaned.value("isolated_removed").toInt()).arg(cleaned.value("spurs_removed").toInt())
                .arg(cleaned.value("joins").toInt());
            queueRender(); continuePending();
        });
        child->setProgram(python);
        QStringList arguments{script, "--input", folder->filePath("input.png"), "--cache", folder->filePath("cache"),
                              "--strength", selectedStrength, "--detail", selectedDetail, "--suppression", QString::number(selectedSuppression)};
        if (selectedCorrection) arguments << "--correct-lines";
        arguments << selectedLineOptions;
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

Controller::Controller(MainWindow* window, QVGraphicsView* view, Sr::Controller* sr) : QObject(window) {
    setObjectName(QStringLiteral("vectorController"));
    d = std::make_unique<Private>(this, window, view, sr);
}
Controller::~Controller() = default;
bool Controller::hasResult() const { return !d->image.isNull(); }
bool Controller::showingResult() const { return d->showing && hasResult(); }
bool Controller::isBusy() const { return d->busy(); }
bool Controller::isEditing() const { return d->editor->active(); }
bool Controller::hasEdits() const { return d->edited; }
QImage Controller::resultImage() const { return d->image; }

void Controller::showPanel() {
    if (d->fullscreen) { d->restorePanel = true; return; }
    bool visibleOnScreen = false;
    for (auto* screen : QGuiApplication::screens())
        visibleOnScreen |= screen->availableGeometry().intersects(d->panel->frameGeometry());
    if (!visibleOnScreen) d->panel->move(d->window->frameGeometry().topRight() - QPoint(d->panel->width(), 0));
    d->panel->show(); d->panel->raise(); d->panel->activateWindow(); d->updateControls();
    if (isEditing()) {
        if (auto* scroll = d->panel->findChild<QScrollArea*>("vectorParameters")) scroll->verticalScrollBar()->setValue(0);
    }
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
    if (!showing) d->endEditing();
    { QSignalBlocker block(d->showResult); d->showResult->setChecked(showing); }
    const bool changed = d->showing != showing; d->showing = showing;
    if (showing) emit previewReady(d->image, d->composedSvg, d->previewScale->value());
    else if (changed) emit originalRequested();
    emit stateChanged();
}

void Controller::setEditing(bool editing) {
    if (editing) d->beginEditing(); else d->endEditing();
}

void Controller::discardEdits() {
    if (d->busy() || d->externalBusy) return;
    d->endEditing();
    if (!d->edited) return;
    d->edited = false;
    d->editor->reset();
    d->composedSvg = d->editBaseSvg; d->image = d->editBaseImage;
    d->editBaseSvg.clear(); d->editInitialSvg.clear(); d->editBaseImage = {};
    ++d->renderRevision; d->renderPending = false;
    if (d->showing) emit previewReady(d->image, d->composedSvg, d->previewScale->value());
    d->setStatus(QStringLiteral("手編集を破棄し、編集前の生成結果へ戻しました。"));
    d->updateControls(false);
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
