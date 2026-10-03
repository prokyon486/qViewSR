// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QImage>
#include <memory>

class MainWindow;
class QVGraphicsView;

namespace Vector {
bool rasterSize(QSize source, double scale, QSize* size, QString* error = nullptr);
QImage rasterizeSvg(const QByteArray& svg, QSize size, QString* error = nullptr);
QByteArray readSvgAsset(const QString& requestedPath, const QString& cacheDirectory, QString* error = nullptr);
class Controller : public QObject {
    Q_OBJECT
public:
    Controller(MainWindow* window, QVGraphicsView* view);
    ~Controller() override;
    bool hasResult() const;
    bool showingResult() const;
    bool isBusy() const;
    QImage resultImage() const;
    bool saveSvg(const QString& path, QString* error = nullptr);
    bool savePng(const QString& path, double scale, QString* error = nullptr);

public slots:
    void showPanel();
    void setFullscreen(bool fullscreen);
    void deactivate();
    void generate();
    void cancel();
    void setExternalBusy(bool busy);
    void setShowingResult(bool showing);

signals:
    void previewReady(const QImage& sRGBImage, const QByteArray& svg, double transferScale);
    void originalRequested();
    void stateChanged();
    void failed(const QString& message);
    void resultReady();

protected:
    bool eventFilter(QObject* object, QEvent* event) override;

private:
    struct Private;
    std::unique_ptr<Private> d;
};
}
