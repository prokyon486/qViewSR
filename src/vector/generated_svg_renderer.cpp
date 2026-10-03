// SPDX-License-Identifier: GPL-3.0-or-later
#include "generated_svg_renderer.h"
#include "memory_budget.h"

#include <QLibrary>
#include <QDomDocument>
#include <QPainter>
#include <QRegularExpression>
#include <QSvgRenderer>
#include <QXmlStreamReader>
#include <cmath>
#include <limits>
#include <vector>

namespace Vector {
namespace {
// Public C ABI declarations keep the optional runtime independent of development
// headers and link-time packages. GLib uses guint32/int/char* for GError.
struct NativeError { quint32 domain; int code; char* message; };
struct NativeRectangle { double x, y, width, height; };
struct NativeMatrix { double xx, yx, xy, yy, x0, y0; };

struct NativeApi {
    QLibrary rsvg, cairo, gobject, glib, gio;
    void* (*newStream)(const void*, qintptr, void (*)(void*)) = nullptr;
    void* (*newHandle)(void*, void*, int, void*, NativeError**) = nullptr;
    int (*renderDocument)(void*, void*, const NativeRectangle*, NativeError**) = nullptr;
    void (*unref)(void*) = nullptr;
    void (*freeError)(NativeError*) = nullptr;
    void* (*newSurface)(unsigned char*, int, int, int, int) = nullptr;
    int (*surfaceStatus)(void*) = nullptr;
    void (*flushSurface)(void*) = nullptr;
    void (*destroySurface)(void*) = nullptr;
    void* (*newContext)(void*) = nullptr;
    int (*contextStatus)(void*) = nullptr;
    void (*setMatrix)(void*, const NativeMatrix*) = nullptr;
    void (*destroyContext)(void*) = nullptr;
    const char* (*statusText)(int) = nullptr;
    bool available = false;

    NativeApi() {
#ifdef Q_OS_LINUX
        // GObject registers global types. Keep the libraries resident after the
        // first load, including when one of the required symbols is absent.
        auto load = [](QLibrary& library, const QString& name, int version) {
            library.setFileNameAndVersion(name, version);
            library.setLoadHints(QLibrary::PreventUnloadHint);
            return library.load();
        };
        if (!load(rsvg, QStringLiteral("rsvg-2"), 2)
            || !load(cairo, QStringLiteral("cairo"), 2)
            || !load(gobject, QStringLiteral("gobject-2.0"), 0)
            || !load(glib, QStringLiteral("glib-2.0"), 0)
            || !load(gio, QStringLiteral("gio-2.0"), 0)) return;
#define RESOLVE(member, library, name) member = reinterpret_cast<decltype(member)>(library.resolve(name)); if (!member) return
        RESOLVE(newStream, gio, "g_memory_input_stream_new_from_data");
        RESOLVE(newHandle, rsvg, "rsvg_handle_new_from_stream_sync");
        RESOLVE(renderDocument, rsvg, "rsvg_handle_render_document");
        RESOLVE(unref, gobject, "g_object_unref");
        RESOLVE(freeError, glib, "g_error_free");
        RESOLVE(newSurface, cairo, "cairo_image_surface_create_for_data");
        RESOLVE(surfaceStatus, cairo, "cairo_surface_status");
        RESOLVE(flushSurface, cairo, "cairo_surface_flush");
        RESOLVE(destroySurface, cairo, "cairo_surface_destroy");
        RESOLVE(newContext, cairo, "cairo_create");
        RESOLVE(contextStatus, cairo, "cairo_status");
        RESOLVE(setMatrix, cairo, "cairo_set_matrix");
        RESOLVE(destroyContext, cairo, "cairo_destroy");
        RESOLVE(statusText, cairo, "cairo_status_to_string");
#undef RESOLVE
        available = true;
#endif
    }
};

NativeApi& nativeApi() { static NativeApi api; return api; }

struct NativeObject {
    void* value;
    void (*release)(void*);
    ~NativeObject() { if (value) release(value); }
};

QString takeError(NativeApi& api, NativeError* error, const QString& fallback) {
    if (!error) return fallback;
    const auto message = error->message ? QString::fromUtf8(error->message) : fallback;
    api.freeError(error);
    return message;
}

bool finiteBox(const QRectF& box) {
    return !box.isEmpty() && std::isfinite(box.x()) && std::isfinite(box.y())
        && std::isfinite(box.width()) && std::isfinite(box.height());
}

struct Metadata { bool extended = false, groupOpacity = false; QRectF box; QString error, subsetError; };

bool needsExtendedPath(QStringView path) {
    return path.size() > 65536 || (path.size() > 16384
        && (path.contains(QLatin1Char('A')) || path.contains(QLatin1Char('a'))
            || path.contains(QLatin1Char('T')) || path.contains(QLatin1Char('t'))));
}

Metadata inspect(const QByteArray& svg) {
    Metadata result;
    QXmlStreamReader xml(svg);
    bool rootSeen = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isDTD()) {
            result.error = QStringLiteral("生成SVGに予期しないDOCTYPEがあります。");
            return result;
        }
        if (xml.isEntityReference()) {
            result.error = QStringLiteral("生成SVGに予期しない実体参照があります。");
            return result;
        }
        if (!xml.isStartElement()) continue;
        if (xml.name() != QStringLiteral("svg") && xml.name() != QStringLiteral("g")
            && xml.name() != QStringLiteral("path") && xml.name() != QStringLiteral("rect"))
            result.subsetError = QStringLiteral("大きい生成SVGに未対応の要素があります: ") + xml.name().toString();
        for (const auto& attribute : xml.attributes()) {
            const auto name = attribute.name().toString().toLower();
            const auto value = attribute.value();
            if (name == QStringLiteral("href") || name == QStringLiteral("style")
                || name == QStringLiteral("base") || name.startsWith(QStringLiteral("on"))
                || value.contains(QStringLiteral("url("), Qt::CaseInsensitive)
                || value.contains(QStringLiteral("data:"), Qt::CaseInsensitive))
                result.subsetError = QStringLiteral("大きい生成SVGに予期しない参照・スタイルがあります。");
        }
        // librsvg 2.58 clips intermediate group-opacity surfaces under some
        // rotation/reflection matrices. Generated translucent lines stay in Qt;
        // never silently accept that unsupported combination in a native layer.
        if ((xml.name() == QStringLiteral("svg") || xml.name() == QStringLiteral("g"))
            && xml.attributes().hasAttribute(QStringLiteral("opacity"))) {
            bool ok = false;
            const auto opacity = xml.attributes().value(QStringLiteral("opacity")).toDouble(&ok);
            result.groupOpacity |= !ok || opacity != 1.;
        }
        if (!rootSeen) {
            rootSeen = true;
            if (xml.name() != QStringLiteral("svg")) {
                result.error = QStringLiteral("生成されたSVGが不正です。");
                return result;
            }
            const auto values = xml.attributes().value(QStringLiteral("viewBox")).toString()
                .split(QRegularExpression(QStringLiteral("[\\s,]+")), Qt::SkipEmptyParts);
            if (values.size() == 4) {
                double coordinates[4]; bool valid = true;
                for (int i=0; i<4; ++i) { bool ok; coordinates[i] = values[i].toDouble(&ok); valid &= ok; }
                if (valid) result.box = QRectF(coordinates[0], coordinates[1], coordinates[2], coordinates[3]);
            }
        }
        if (xml.name() == QStringLiteral("path")) {
            const auto path = xml.attributes().value(QStringLiteral("d"));
            // Qt 6.4 truncates at 32767 QPainterPath elements. Generated paths
            // use M/L/C/Z: 64K characters is conservative even for compact H/V
            // syntax. Arcs can expand into several cubics, so use a lower bound.
            if (needsExtendedPath(path)) result.extended = true;
        }
    }
    if (xml.hasError()) result.error = QStringLiteral("生成されたSVGを解析できません: ") + xml.errorString();
    return result;
}

QList<QDomElement> childrenOf(const QDomElement& element) {
    QList<QDomElement> nodes;
    for (auto child=element.firstChildElement(); !child.isNull(); child=child.nextSiblingElement()) nodes.append(child);
    return nodes;
}

QList<QByteArray> splitGeneratedLayers(const QByteArray& svg) {
    QDomDocument source;
    if (!source.setContent(svg)) return {svg};
    const auto sourceRoot = source.documentElement();
    const auto rootAttributes = sourceRoot.attributes();
    for (int i=0; i<rootAttributes.size(); ++i) {
        const auto name = rootAttributes.item(i).nodeName();
        if (name!=QStringLiteral("width") && name!=QStringLiteral("height")
            && name!=QStringLiteral("viewBox") && name!=QStringLiteral("xmlns")
            && name!=QStringLiteral("version") && name!=QStringLiteral("preserveAspectRatio"))
            return {svg};
    }
    auto container = sourceRoot;
    QList<QDomElement> ancestors;
    auto children = childrenOf(container);
    // Export wraps the generated layers in a transform-only group. Replicating
    // opacity, filters or composition operators across layers would change
    // overlaps, so only this known neutral wrapper may be traversed.
    while (children.size()==1 && children[0].tagName()==QStringLiteral("g")
        && children[0].attributes().size()==1 && children[0].hasAttribute(QStringLiteral("transform"))) {
        container = children[0]; ancestors.append(container); children = childrenOf(container);
    }
    if (children.isEmpty()) return {svg};
    for (const auto& child : children) {
        const auto id = child.attribute(QStringLiteral("id"));
        if (!((id==QStringLiteral("background") && child.tagName()==QStringLiteral("rect"))
            || ((id==QStringLiteral("fill") || id==QStringLiteral("lines")) && child.tagName()==QStringLiteral("g"))))
            return {svg};
    }
    QList<QByteArray> result;
    auto append = [&](const QList<QDomElement>& nodes, const QDomElement& fill = QDomElement()) {
        if (nodes.isEmpty()) return;
        QDomDocument document;
        auto parent = document.importNode(sourceRoot, false);
        document.appendChild(parent);
        for (const auto& ancestor : ancestors) {
            auto group = document.importNode(ancestor, false); parent.appendChild(group); parent = group;
        }
        if (!fill.isNull()) {
            auto group = document.importNode(fill, false); parent.appendChild(group); parent = group;
        }
        for (const auto& node : nodes) parent.appendChild(document.importNode(node, true));
        result.append(document.toByteArray(-1));
    };
    for (const auto& layer : children) {
        if (layer.attribute(QStringLiteral("id")) != QStringLiteral("fill")) {
            // In particular, keep the lines group's opacity as one Qt layer.
            append({layer}); continue;
        }
        const auto paths = childrenOf(layer);
        bool separable = layer.attributes().size()==1; // Generated fill has only id="fill".
        for (const auto& path : paths) separable &= path.tagName()==QStringLiteral("path");
        if (!separable) { append({layer}); continue; }
        QList<QDomElement> pending;
        for (const auto& path : paths) {
            if (needsExtendedPath(path.attribute(QStringLiteral("d")))) {
                append(pending, layer); pending.clear(); append({path}, layer);
            } else pending.append(path);
        }
        append(pending, layer);
    }
    return result.isEmpty() ? QList<QByteArray>{svg} : result;
}
}

struct GeneratedSvgRenderer::Private {
    struct Layer {
        std::unique_ptr<QSvgRenderer> qt;
        void* native = nullptr;
        ~Layer() { if (native) nativeApi().unref(native); }
    };
    std::vector<std::unique_ptr<Layer>> layers;
    QRectF box;
    QString error;
};

GeneratedSvgRenderer::GeneratedSvgRenderer(const QByteArray& svg, bool allowExtendedPaths)
    : d(std::make_unique<Private>()) {
    d->error = svgMemoryError(quint64(svg.size()));
    if (!d->error.isEmpty()) return;
    const auto metadata = inspect(svg);
    if (!metadata.error.isEmpty()) { d->error = metadata.error; return; }
    if (metadata.extended && !metadata.subsetError.isEmpty()) { d->error = metadata.subsetError; return; }
    if (metadata.extended && (!allowExtendedPaths || !nativeApi().available)) {
        d->error = QStringLiteral("長いベクターパスの描画にはlibrsvg 2.46以降とCairoが必要です。"
                                  "現在の環境では読み込めないため、描画を中止しました。");
        return;
    }
    if (metadata.extended && !finiteBox(metadata.box)) { d->error = QStringLiteral("生成SVGのviewBoxが不正です。"); return; }
    const auto fragments = metadata.extended ? splitGeneratedLayers(svg) : QList<QByteArray>{svg};
    for (const auto& fragment : fragments) {
        const auto part = metadata.extended ? inspect(fragment) : metadata;
        auto layer = std::make_unique<Private::Layer>();
        if (!part.extended) {
            layer->qt = std::make_unique<QSvgRenderer>(fragment);
            d->box = layer->qt->viewBoxF();
            if (!layer->qt->isValid() || !finiteBox(d->box)) {
                d->error = QStringLiteral("生成されたSVGを描画できません。"); return;
            }
        } else {
            if (part.groupOpacity) {
                d->error = QStringLiteral("長いベクターパスを含むグループの不透明度には対応していません。描画を中止しました。");
                return;
            }
            auto& api = nativeApi();
            NativeError* error = nullptr;
            // Synchronous load borrows the fragment only for this call. No base
            // URL or unlimited flag is supplied. Long paths are isolated from
            // ordinary paths to avoid librsvg's separate million-node limit.
            NativeObject stream{api.newStream(fragment.constData(), qintptr(fragment.size()), nullptr), api.unref};
            if (!stream.value) { d->error = QStringLiteral("SVGの読み込み領域を確保できません。"); return; }
            layer->native = api.newHandle(stream.value, nullptr, 0, nullptr, &error);
            if (!layer->native) {
                d->error = QStringLiteral("大きいSVGを読み込めません: ") + takeError(api, error, QStringLiteral("librsvgの読み込みに失敗しました。"));
                return;
            }
            if (error) api.freeError(error);
            d->box = part.box;
        }
        d->layers.push_back(std::move(layer));
    }
}

GeneratedSvgRenderer::~GeneratedSvgRenderer() = default;
bool GeneratedSvgRenderer::isValid() const { return d->error.isEmpty() && !d->layers.empty(); }
QRectF GeneratedSvgRenderer::viewBoxF() const { return d->box; }
QString GeneratedSvgRenderer::errorString() const { return d->error; }
bool GeneratedSvgRenderer::usesExtendedRenderer() const {
    for (const auto& layer : d->layers) if (layer->native) return true;
    return false;
}

bool GeneratedSvgRenderer::render(QImage& image, const QTransform& sourceToDevice, QString* error) {
    const auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    if (!isValid()) return fail(d->error);
    if (image.isNull() || image.format() != QImage::Format_ARGB32_Premultiplied)
        return fail(QStringLiteral("SVGの描画先画像が不正です。"));
    if (image.bytesPerLine() > std::numeric_limits<int>::max())
        return fail(QStringLiteral("SVGの描画先画像の幅が大きすぎます。"));
    if (!sourceToDevice.isAffine() || !sourceToDevice.isInvertible()
        || !std::isfinite(sourceToDevice.m11()) || !std::isfinite(sourceToDevice.m12())
        || !std::isfinite(sourceToDevice.m21()) || !std::isfinite(sourceToDevice.m22())
        || !std::isfinite(sourceToDevice.dx()) || !std::isfinite(sourceToDevice.dy()))
        return fail(QStringLiteral("SVGの描画座標が不正です。"));
    if (usesExtendedRenderer() && (image.width() > 32767 || image.height() > 32767))
        return fail(QStringLiteral("大きいパスのCairo描画は一辺32767pxまでです。画像化の倍率を下げてください。"));
    for (const auto& layer : d->layers) {
        if (layer->qt) {
            QPainter painter(&image);
            painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
            painter.setWorldTransform(sourceToDevice);
            layer->qt->render(&painter, d->box);
            continue;
        }
        auto& api = nativeApi();
        NativeObject surface{api.newSurface(image.bits(), 0, image.width(), image.height(), image.bytesPerLine()), api.destroySurface};
        if (!surface.value) return fail(QStringLiteral("Cairoの描画面を確保できません。"));
        int status = api.surfaceStatus(surface.value);
        if (status) return fail(QString::fromUtf8(api.statusText(status)));
        NativeObject context{api.newContext(surface.value), api.destroyContext};
        if (!context.value) return fail(QStringLiteral("Cairoの描画領域を確保できません。"));
        status = api.contextStatus(context.value);
        if (status) return fail(QString::fromUtf8(api.statusText(status)));
        const NativeMatrix matrix{sourceToDevice.m11(), sourceToDevice.m12(), sourceToDevice.m21(),
                                  sourceToDevice.m22(), sourceToDevice.dx(), sourceToDevice.dy()};
        api.setMatrix(context.value, &matrix);
        const NativeRectangle viewport{d->box.x(), d->box.y(), d->box.width(), d->box.height()};
        NativeError* nativeError = nullptr;
        const bool rendered = api.renderDocument(layer->native, context.value, &viewport, &nativeError);
        const auto message = takeError(api, nativeError, QStringLiteral("librsvgの描画に失敗しました。"));
        api.flushSurface(surface.value);
        status = api.contextStatus(context.value);
        if (!status) status = api.surfaceStatus(surface.value);
        if (!rendered) return fail(message);
        if (status) return fail(QString::fromUtf8(api.statusText(status)));
    }
    return true;
}

}
