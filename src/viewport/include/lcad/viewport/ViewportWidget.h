// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QOpenGLWidget>
#include <QString>
#include <QSurfaceFormat>

#include <memory>

namespace lcad::kernel {
class Shape;
}

namespace lcad::viewport {

/// Process-wide setup the viewport depends on. Call it once, before constructing QApplication.
///
/// It selects the X11 platform plugin on Linux (unless QT_QPA_PLATFORM is already set), makes
/// the viewport's surface format the default, and lets OpenGL contexts share resources.
void prepareApplication();

/// The 3D view of the model.
///
/// This is the Phase 0 viewport: it shows one shape and supports mouse navigation. It exists to
/// prove that OCCT's viewer can draw into a Qt widget on every supported distribution.
class ViewportWidget : public QOpenGLWidget {
    Q_OBJECT

public:
    explicit ViewportWidget(QWidget* parent = nullptr);
    ~ViewportWidget() override;

    /// The OpenGL surface format the viewport needs.
    [[nodiscard]] static QSurfaceFormat surfaceFormat();

    /// Shows `shape` in place of whatever was displayed, and frames it. A null shape empties
    /// the view.
    void setShape(const kernel::Shape& shape);

    /// Moves the camera so that everything displayed is in view.
    void fitAll();

    [[nodiscard]] QColor backgroundColor() const;

    /// The OpenGL vendor, device, and version the viewport renders with, one per line. Empty
    /// until the viewport has been shown.
    [[nodiscard]] QString graphicsInfo() const;

    /// Why the viewport cannot render, or an empty string when it is working.
    [[nodiscard]] QString errorString() const;

signals:
    /// Emitted once if the viewport cannot start rendering.
    void initializationFailed(const QString& reason);

protected:
    void initializeGL() override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    class Impl;
    void fail(const QString& reason);

    std::unique_ptr<Impl> m_impl;
};

} // namespace lcad::viewport
