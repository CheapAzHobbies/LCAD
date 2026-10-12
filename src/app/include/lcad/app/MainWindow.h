#pragma once

#include <QMainWindow>

namespace lcad::viewport {
class ViewportWidget;
}

namespace lcad::app {

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    [[nodiscard]] viewport::ViewportWidget* viewport() const { return m_viewport; }

    /// What Help > About shows: LCAD's version and the versions of Qt, Open CASCADE Technology,
    /// and the graphics driver in use.
    [[nodiscard]] QString aboutText() const;

private:
    void showAbout();
    void showViewportError(const QString& reason);

    viewport::ViewportWidget* m_viewport;
};

} // namespace lcad::app
