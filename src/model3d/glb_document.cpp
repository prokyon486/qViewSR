// SPDX-License-Identifier: GPL-3.0-or-later
#include "glb_document.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QtEndian>

namespace Model3D {
bool isGlb(const QString &path) { return QFileInfo(path).suffix().compare("glb", Qt::CaseInsensitive) == 0; }

Document inspectGlb(const QString &path)
{
    Document result;
    auto fail = [&](const QString &error) { result.error = error; return result; };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("GLBを開けません: ") + file.errorString());
    const auto header = file.read(12);
    const auto u32 = [](const QByteArray &bytes, int offset) {
        return qFromLittleEndian<quint32>(bytes.constData() + offset);
    };
    if (header.size() != 12 || header.first(4) != "glTF" || u32(header, 4) != 2)
        return fail(QStringLiteral("glTF 2.0のGLBファイルではありません。"));
    if (qint64(u32(header, 8)) != file.size())
        return fail(QStringLiteral("GLBのファイル長が不正です（破損またはダウンロード未完了）。"));
    QJsonObject json;
    bool first = true, binarySeen = false;
    while (!file.atEnd()) {
        const auto chunk = file.read(8);
        if (chunk.size() != 8) return fail(QStringLiteral("GLBのチャンクヘッダーが不完全です。"));
        const qint64 length = u32(chunk, 0);
        const auto type = u32(chunk, 4);
        if (length % 4 || length > file.size() - file.pos())
            return fail(QStringLiteral("GLBのチャンク長が不正です。"));
        if (first) {
            if (type != 0x4e4f534a || length > 64 * 1024 * 1024)
                return fail(QStringLiteral("GLBの先頭に有効なJSONがありません（上限64 MiB）。"));
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(file.read(length), &error);
            if (error.error != QJsonParseError::NoError || !document.isObject())
                return fail(QStringLiteral("GLBのJSONを解析できません: ") + error.errorString());
            json = document.object();
            first = false;
        } else {
            if (type == 0x4e4f534a || (type == 0x004e4942 && binarySeen))
                return fail(QStringLiteral("GLBに重複したチャンクがあります。"));
            binarySeen |= type == 0x004e4942;
            if (!file.seek(file.pos() + length)) return fail(file.errorString());
        }
    }
    const auto asset = json.value("asset").toObject();
    if (first || asset.value("version").toString() != "2.0"
            || (!asset.value("minVersion").isUndefined() && asset.value("minVersion").toString() != "2.0"))
        return fail(QStringLiteral("このGLBには未対応のglTFバージョンが必要です。"));

    // Conservative subset of the Qt 6.4 Assimp runtime material importer.
    // In particular Draco, meshopt, BasisU and unlit are not promised by this backend.
    const QSet<QString> supported{
        "KHR_texture_transform", "KHR_materials_clearcoat", "KHR_materials_transmission",
        "KHR_materials_volume", "KHR_materials_ior", "KHR_materials_pbrSpecularGlossiness",
        "KHR_lights_punctual"
    };
    QStringList unsupported;
    for (const auto &extension : json.value("extensionsRequired").toArray()) {
        if (!extension.isString() || !supported.contains(extension.toString()))
            unsupported << (extension.isString() ? extension.toString() : QStringLiteral("不正な拡張名"));
    }
    if (!unsupported.isEmpty())
        return fail(QStringLiteral("このGLBに必須の拡張は未対応です: %1\n標準glTF 2.0（非圧縮）のGLBに書き出すと表示できます。")
                    .arg(unsupported.join(", ")));
    for (const auto &extension : json.value("extensionsUsed").toArray()) {
        if (!supported.contains(extension.toString()))
            result.warnings << QStringLiteral("任意拡張 %1 は標準材質などの代替表現で表示します。")
                                .arg(extension.toString());
    }
    result.meshes = json.value("meshes").toArray().size();
    result.hasAnimations = !json.value("animations").toArray().isEmpty();
    if (result.hasAnimations) result.warnings << QStringLiteral("アニメーションは再生せず、初期姿勢を表示します。");
    if (!result.meshes) return fail(QStringLiteral("このGLBには表示できるメッシュがありません。"));
    return result;
}
}
