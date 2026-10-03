// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QSaveFile>
#include <atomic>
#include <memory>
#include <utility>

namespace Vector {
// QImageWriter reports a device error when compression reaches its next output
// chunk. QSaveFile keeps any previous destination intact until a complete commit.
class CancellableSaveFile : public QSaveFile {
public:
    CancellableSaveFile(const QString& path, std::shared_ptr<std::atomic_bool> cancelled = {})
        : QSaveFile(path), cancelled_(std::move(cancelled)) {}

    bool cancellationRequested() const {
        return cancelled_ && cancelled_->load(std::memory_order_relaxed);
    }

    bool commit() {
        if (stopIfCancelled()) return false;
        return QSaveFile::commit();
    }

protected:
    qint64 writeData(const char* data, qint64 size) override {
        if (stopIfCancelled()) return -1;
        return QSaveFile::writeData(data, size);
    }

private:
    bool stopIfCancelled() {
        if (!cancellationRequested()) return false;
        cancelWriting();
        setErrorString(QStringLiteral("入力画像の準備を中止しました。"));
        return true;
    }
    std::shared_ptr<std::atomic_bool> cancelled_;
};
}
