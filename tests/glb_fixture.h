// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QBuffer>
#include <QDataStream>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVector3D>

namespace GlbFixture {
inline QByteArray container(QJsonObject json, QByteArray binary = {})
{
    auto text = QJsonDocument(json).toJson(QJsonDocument::Compact);
    while (text.size() % 4) text += ' ';
    while (binary.size() % 4) binary += '\0';
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly); stream.setByteOrder(QDataStream::LittleEndian);
    stream << quint32(0x46546c67) << quint32(2) << quint32(20 + text.size() + (binary.isEmpty() ? 0 : 8 + binary.size()));
    stream << quint32(text.size()) << quint32(0x4e4f534a);
    stream.writeRawData(text.constData(), text.size());
    if (!binary.isEmpty()) {
        stream << quint32(binary.size()) << quint32(0x004e4942);
        stream.writeRawData(binary.constData(), binary.size());
    }
    return bytes;
}
inline bool write(const QString &path, const QByteArray &bytes)
{
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
inline bool cube(const QString &path, bool animated = false)
{
    QByteArray binary;
    QDataStream vertices(&binary, QIODevice::WriteOnly);
    vertices.setByteOrder(QDataStream::LittleEndian); vertices.setFloatingPointPrecision(QDataStream::SinglePrecision);
    const QVector<QVector3D> normals{{0,0,1},{1,0,0},{0,0,-1},{-1,0,0},{0,1,0},{0,-1,0}};
    const QVector<QVector3D> horizontal{{1,0,0},{0,0,-1},{-1,0,0},{0,0,1},{1,0,0},{1,0,0}};
    const QVector<QVector3D> vertical{{0,1,0},{0,1,0},{0,1,0},{0,1,0},{0,0,-1},{0,0,1}};
    const QVector<QPointF> uv{{0,0},{1,0},{1,1},{0,0},{1,1},{0,1}};
    for (int side = 0; side < 6; ++side) for (const auto &p : uv) {
        const auto normal = normals[side];
        const auto point = normal + horizontal[side] * (2 * p.x() - 1) + vertical[side] * (2 * p.y() - 1);
        vertices << point.x() << point.y() << point.z() << normal.x() << normal.y() << normal.z()
                 << float(p.x()) << float(p.y());
    }
    const int vertexBytes = binary.size();
    QImage texture(8, 8, QImage::Format_RGB32);
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x)
        texture.setPixelColor(x, y, (x < 4) != (y < 4) ? QColor(240,130,30) : QColor(30,100,220));
    QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly); texture.save(&buffer, "png");
    binary += png;
    QJsonObject json{
        {"asset", QJsonObject{{"version","2.0"},{"generator","qViewSR regression fixture"}}},
        {"scene",0}, {"scenes",QJsonArray{QJsonObject{{"nodes",QJsonArray{0}}}}},
        {"nodes",QJsonArray{QJsonObject{{"mesh",0},{"translation",QJsonArray{123,-57,19}},{"scale",QJsonArray{2,3,1}}}}},
        {"buffers",QJsonArray{QJsonObject{{"byteLength",binary.size()}}}},
        {"bufferViews",QJsonArray{QJsonObject{{"buffer",0},{"byteOffset",0},{"byteLength",vertexBytes},{"byteStride",32},{"target",34962}},
                                  QJsonObject{{"buffer",0},{"byteOffset",vertexBytes},{"byteLength",png.size()}}}},
        {"accessors",QJsonArray{QJsonObject{{"bufferView",0},{"byteOffset",0},{"componentType",5126},{"count",36},{"type","VEC3"},{"min",QJsonArray{-1,-1,-1}},{"max",QJsonArray{1,1,1}}},
                                QJsonObject{{"bufferView",0},{"byteOffset",12},{"componentType",5126},{"count",36},{"type","VEC3"}},
                                QJsonObject{{"bufferView",0},{"byteOffset",24},{"componentType",5126},{"count",36},{"type","VEC2"}}}},
        {"images",QJsonArray{QJsonObject{{"bufferView",1},{"mimeType","image/png"}}}},
        {"textures",QJsonArray{QJsonObject{{"source",0}}}},
        {"materials",QJsonArray{QJsonObject{{"pbrMetallicRoughness",QJsonObject{
                    {"baseColorTexture",QJsonObject{{"index",0}}},{"metallicFactor",0.2},{"roughnessFactor",0.45}}}}}},
        {"meshes",QJsonArray{QJsonObject{{"primitives",QJsonArray{QJsonObject{
                    {"attributes",QJsonObject{{"POSITION",0},{"NORMAL",1},{"TEXCOORD_0",2}}},{"material",0}}}}}}}
    };
    if (animated) {
        while (binary.size() % 4) binary += '\0';
        const auto offset = binary.size();
        QByteArray keys; QDataStream keyStream(&keys,QIODevice::WriteOnly);
        keyStream.setByteOrder(QDataStream::LittleEndian); keyStream.setFloatingPointPrecision(QDataStream::SinglePrecision);
        keyStream << 0.0f << 1.0f << 123.0f << -57.0f << 19.0f << 143.0f << -57.0f << 19.0f;
        binary += keys;
        auto views = json["bufferViews"].toArray();
        views.append(QJsonObject{{"buffer",0},{"byteOffset",offset},{"byteLength",8}});
        views.append(QJsonObject{{"buffer",0},{"byteOffset",offset+8},{"byteLength",24}});
        json["bufferViews"] = views;
        auto accessors = json["accessors"].toArray();
        accessors.append(QJsonObject{{"bufferView",2},{"componentType",5126},{"count",2},{"type","SCALAR"},{"min",QJsonArray{0}},{"max",QJsonArray{1}}});
        accessors.append(QJsonObject{{"bufferView",3},{"componentType",5126},{"count",2},{"type","VEC3"}});
        json["accessors"] = accessors;
        json["buffers"] = QJsonArray{QJsonObject{{"byteLength",binary.size()}}};
        json["animations"] = QJsonArray{QJsonObject{
            {"samplers",QJsonArray{QJsonObject{{"input",3},{"output",4},{"interpolation","LINEAR"}}}},
            {"channels",QJsonArray{QJsonObject{{"sampler",0},{"target",QJsonObject{{"node",0},{"path","translation"}}}}}}
        }};
    }
    return write(path,container(json,binary));
}
}
