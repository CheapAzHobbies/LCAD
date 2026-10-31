// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/app/DrawingPanel.h>
#include <lcad/app/SheetView.h>
#include <lcad/document/Document.h>
#include <lcad/drawing/Sheet.h>

#include <QObject>
#include <QString>

#include <memory>
#include <optional>

namespace lcad::app {

/// The Drawing workspace: a sheet in the middle of the window, its panel on the right, and what
/// the tools do. It keeps the sheet in step with the document, and changes drawings through it,
/// so each change can be undone.
class DrawingWorkspace : public QObject {
    Q_OBJECT

public:
    /// What a click on the sheet does.
    enum class Tool {
        Select,
        /// Click an edge, a circle, or two points.
        Dimension,
        /// Click where a cutting line goes, up or across a view.
        SectionUp,
        SectionAcross,
        /// Click the middle of what to show larger.
        Detail,
        /// Click where a note goes.
        Note,
    };

    explicit DrawingWorkspace(QWidget* parent);
    ~DrawingWorkspace() override;

    [[nodiscard]] SheetView* view() const { return m_view; }
    [[nodiscard]] DrawingPanel* panel() const { return m_panel; }

    /// Shows a document's drawings. The document has to stay alive until another one, or
    /// nullptr, is set.
    void setDocument(document::Document* document);

    /// Whether the workspace is showing, so that it works out sheets.
    void setShown(bool shown);

    /// The drawing on the sheet, or nullptr.
    [[nodiscard]] const document::Drawing* drawing() const;
    /// Adds a drawing with the standard views of the model, and shows it.
    void newDrawing();
    /// Adds a view of one kind to the drawing on the sheet.
    void addView(document::ViewKind kind);
    void setTool(Tool tool);
    [[nodiscard]] Tool tool() const { return m_tool; }
    /// Removes what is selected. Returns false if nothing is.
    bool deleteSelected();
    /// Writes the sheet to a file: PDF for a name ending in .pdf, and DXF for the rest. Returns
    /// false, and says why in `error`, if it cannot.
    bool exportTo(const QString& path, QString* error = nullptr);

signals:
    /// The tool in use, or what can be done, changed.
    void stateChanged();
    /// Something the user should be told.
    void message(const QString& text);

private:
    void onDocumentChanged(const document::Change& change);
    void rebuild();
    void change(const document::Drawing& drawing, const QString& label);
    void onClicked(const drawing::Point2D& at, Qt::KeyboardModifiers modifiers);
    void clickDimension(const drawing::Point2D& at, Qt::KeyboardModifiers modifiers);
    void clickSection(const drawing::Point2D& at, bool vertical);
    void clickDetail(const drawing::Point2D& at);
    void addLinear(document::ObjectId view, const document::DrawingPoint& first,
                   const drawing::Point2D& from, const document::DrawingPoint& second,
                   const drawing::Point2D& to);
    [[nodiscard]] QString nextLabel() const;
    [[nodiscard]] QString prompt() const;
    [[nodiscard]] const drawing::ViewBox* viewAt(const drawing::Point2D& at) const;

    document::Document* m_document = nullptr;
    document::Document::ListenerId m_listener = 0;
    std::unique_ptr<drawing::SheetBuilder> m_builder;
    SheetView* m_view;
    DrawingPanel* m_panel;
    document::ObjectId m_current;
    bool m_shown = false;
    Tool m_tool = Tool::Select;
    /// The Dimension tool's first point, until it has its second.
    std::optional<drawing::SnapPoint> m_pending;
};

} // namespace lcad::app
