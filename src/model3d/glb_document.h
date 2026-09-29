// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>
#include <QStringList>

namespace Model3D {
struct Document {
    QString error;
    QStringList warnings;
    int meshes = 0;
    bool hasAnimations = false;
};
bool isGlb(const QString &path);
// Inspect the container before handing it to Qt's asset importer. No file writes.
Document inspectGlb(const QString &path);
}
