// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QImage>
#include <QString>

namespace Sr {
struct Profile {
    QByteArray icc;
    QString description;
    QString error;
    bool assumedSrgb = false;
};
Profile readProfile(const QString& path, const QImage& decoded);
QByteArray srgbProfile();
QImage convert(const QImage& source, const Profile& profile, const QByteArray& destination,
               QString* error = nullptr);
QImage toSrgb(const QImage& source, const Profile& profile, QString* error = nullptr);
}
