// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QImage>
#include <memory>

class MainWindow;
class QVGraphicsView;
namespace Sr { class Controller; }

namespace Vector {
class GeneratedSvgRenderer;
bool rasterSize(QSize source, double scale, QSize* size, QString* error = nullptr);
QImage rasterizeSvg(const QByteArray& svg, QSize size, QString* error = nullptr);
QByteArray readSvgAsset(const QString& requestedPath, const QString& cacheDirectory, QString* error = nullptr);
class Controller : public QObject {
    Q_OBJECT
public:
    Controller(MainWindow* window, QVGraphicsView* view, Sr::Controller* sr);
    ~Controller() override;
    bool hasResult() const;
    bool showingResult() const;
    bool isBusy() const;
    bool isEditing() const;
    bool hasEdits() const;
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
    void setEditing(bool editing);
    void discardEdits();

signals:
    void previewReady(const QImage& sRGBImage, const QByteArray& svg, double transferScale,
                      std::shared_ptr<GeneratedSvgRenderer> renderer = {});
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
