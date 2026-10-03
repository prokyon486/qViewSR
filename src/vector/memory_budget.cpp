// SPDX-License-Identifier: GPL-3.0-or-later
#include "memory_budget.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <limits>
#include <optional>

namespace Vector {
namespace {
using Bytes = std::optional<quint64>;
constexpr quint64 MiB = 1024ULL * 1024;

#ifdef Q_OS_LINUX
QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    // procfs/cgroup files are small. A cap also keeps malformed data bounded.
    return file.read(1024 * 1024);
}

Bytes unsignedValue(const QByteArray& value)
{
    const auto text = value.trimmed();
    if (text.isEmpty()) return {};
    for (const char c : text)
        if (c < '0' || c > '9') return {};
    bool ok = false;
    const quint64 result = text.toULongLong(&ok);
    return ok ? Bytes(result) : Bytes();
}

void tighten(Bytes& available, Bytes candidate)
{
    if (candidate && (!available || *candidate < *available)) available = candidate;
}

QString unescapeMountPath(const QByteArray& input)
{
    QByteArray result;
    for (qsizetype i = 0; i < input.size(); ++i) {
        if (input[i] == '\\' && i + 3 < input.size()
            && input[i+1] >= '0' && input[i+1] <= '7'
            && input[i+2] >= '0' && input[i+2] <= '7'
            && input[i+3] >= '0' && input[i+3] <= '7') {
            result += char((input[i+1]-'0')*64 + (input[i+2]-'0')*8 + input[i+3]-'0');
            i += 3;
        } else result += input[i];
    }
    return QFile::decodeName(result);
}

Bytes cgroupRemaining(const QString& directory, bool unified)
{
    const auto limit = unsignedValue(readFile(directory + (unified
        ? QStringLiteral("/memory.max") : QStringLiteral("/memory.limit_in_bytes"))));
    const auto current = unsignedValue(readFile(directory + (unified
        ? QStringLiteral("/memory.current") : QStringLiteral("/memory.usage_in_bytes"))));
    // cgroup v2's "max" does not parse as a number. v1 uses a page-aligned
    // near-LONG_MAX value to mean unlimited, which must not invent free RAM.
    if (!limit || !current || (!unified && *limit >= (1ULL << 60))) return {};
    return *current >= *limit ? 0 : *limit - *current;
}

void readCgroupAncestors(Bytes& available, const QString& mountPoint,
                        const QString& mountRoot, const QString& membership, bool unified)
{
    QString relative;
    if (membership == mountRoot) relative = QString();
    else if (mountRoot == QStringLiteral("/")) relative = membership.mid(1);
    else if (membership.startsWith(mountRoot + '/')) relative = membership.mid(mountRoot.size()+1);
    else relative = membership.mid(1); // A cgroup namespace can expose relative membership.

    const QString base = QDir::cleanPath(mountPoint);
    QDir directory(QDir(base).absoluteFilePath(relative));
    if (!directory.exists()) return;
    for (;;) {
        const QString path = directory.absolutePath();
        if (path != base && !path.startsWith(base + '/')) break;
        tighten(available, cgroupRemaining(path, unified));
        if (path == base || !directory.cdUp()) break;
    }
}
#endif
}

quint64 availableMemoryBytes()
{
#ifdef Q_OS_LINUX
    Bytes available;
    for (const auto& line : readFile(QStringLiteral("/proc/meminfo")).split('\n')) {
        if (!line.startsWith("MemAvailable:")) continue;
        const auto fields = line.mid(sizeof("MemAvailable:")-1).simplified().split(' ');
        const auto value = fields.isEmpty() ? Bytes() : unsignedValue(fields.first());
        if (value && fields.size() > 1 && fields[1] == "kB"
            && *value <= std::numeric_limits<quint64>::max()/1024)
            available = *value * 1024;
        break;
    }

    QString unifiedMembership, memoryMembership;
    for (const auto& line : readFile(QStringLiteral("/proc/self/cgroup")).split('\n')) {
        const auto first = line.indexOf(':');
        const auto second = first < 0 ? -1 : line.indexOf(':', first+1);
        if (second < 0) continue;
        const auto controllers = line.mid(first+1, second-first-1);
        const QString membership = QFile::decodeName(line.mid(second+1));
        if (!membership.startsWith('/')) continue;
        if (controllers.isEmpty()) unifiedMembership = membership;
        else if (controllers.split(',').contains("memory")) memoryMembership = membership;
    }
    for (const auto& line : readFile(QStringLiteral("/proc/self/mountinfo")).split('\n')) {
        const auto fields = line.split(' ');
        const auto separator = fields.indexOf("-");
        if (separator < 6 || fields.size() <= separator+3) continue;
        const bool unified = fields[separator+1] == "cgroup2";
        const bool memory = fields[separator+1] == "cgroup"
            && fields[separator+3].split(',').contains("memory");
        if (!unified && !memory) continue;
        const QString membership = unified ? unifiedMembership : memoryMembership;
        if (membership.isEmpty()) continue;
        readCgroupAncestors(available, unescapeMountPath(fields[4]),
                            unescapeMountPath(fields[3]), membership, unified);
    }
    // Also covers restricted procfs where mount/membership details are hidden.
    tighten(available, cgroupRemaining(QStringLiteral("/sys/fs/cgroup"), true));
    tighten(available, cgroupRemaining(QStringLiteral("/sys/fs/cgroup/memory"), false));
    return available ? qMax<quint64>(1, *available) : 0;
#else
    return 0;
#endif
}

QString preparationMemoryError(QSize sourceSize)
{
    if (sourceSize.isEmpty()) return QStringLiteral("入力画像のサイズが不正です。");
    const quint64 pixels = quint64(sourceSize.width()) * quint64(sourceSize.height());
    if (pixels > (std::numeric_limits<quint64>::max()-64*MiB)/32)
        return QStringLiteral("入力画像の作業メモリー見積りが扱える範囲を超えています。");
    const quint64 required = pixels*32 + 64*MiB;
    const quint64 available = availableMemoryBytes();
    if (!available) return {};
    const quint64 budget = (available/10)*7 + ((available%10)*7)/10;
    if (required <= budget) return {};
    const auto asMiB = [](quint64 bytes) { return bytes/MiB + (bytes%MiB != 0); };
    return QStringLiteral("入力準備用の空きメモリーが不足しています。追加見積り %1 MiB / "
                          "使用可能な目安 %2 MiB（現在の空き %3 MiB の70%）。"
                          "他のアプリを閉じるか、画像を小さくして試してください。")
        .arg(asMiB(required)).arg(asMiB(budget)).arg(asMiB(available));
}

QString svgMemoryError(quint64 serializedBytes, quint64 availableBytes)
{
    if (serializedBytes > MaxSvgSerializedBytes)
        return QStringLiteral("SVGデータが上限（単層・合成とも1GiB）を超えます。線抽出の強さや曲線の細かさを下げてください。");
    if (!availableBytes) {
        if (serializedBytes <= 64*MiB) return {};
        return QStringLiteral("使用可能なメモリー量を取得できないため、64MiBを超えるSVGは処理できません。");
    }
    // The serialized cap above also makes this multiplication overflow-safe.
    const quint64 required = serializedBytes*32 + 64*MiB;
    const quint64 budget = (availableBytes/10)*7 + ((availableBytes%10)*7)/10;
    if (required <= budget) return {};
    const auto asMiB = [](quint64 bytes) { return bytes/MiB + (bytes%MiB != 0); };
    return QStringLiteral("SVG処理用の空きメモリーが不足しています。追加見積り %1 MiB / "
                          "使用可能な目安 %2 MiB（現在の空き %3 MiB の70%）。"
                          "他のアプリを閉じてから再試行してください。")
        .arg(asMiB(required)).arg(asMiB(budget)).arg(asMiB(availableBytes));
}

QString svgMemoryError(quint64 serializedBytes)
{
    return svgMemoryError(serializedBytes, availableMemoryBytes());
}

}
