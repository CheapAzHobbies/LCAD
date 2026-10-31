// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/app/SheetView.h>
#include <lcad/document/Drawing.h>

#include <QString>
#include <QStringList>
#include <QWidget>

#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace lcad::app {

/// What the right-hand panel shows in the Drawing workspace: which drawing, the size of its sheet,
/// its title block, what is selected on it, and what the tool in use waits for. The panel holds
/// no drawing of its own: it shows the one it is given, and says what the user changed in it.
class DrawingPanel : public QWidget {
    Q_OBJECT

public:
    explicit DrawingPanel(QWidget* parent = nullptr);

    /// Shows the document's drawings, with `current` open. Null for none.
    void setDrawings(const std::vector<document::Drawing>& drawings,
                     const document::Drawing* current);
    void setSelected(SheetView::Selected selected);
    /// What the tool in use waits for. Empty for none.
    void setPrompt(const QString& prompt);
    /// What could not be shown.
    void setProblems(const QStringList& problems);

    [[nodiscard]] QString promptText() const { return m_promptText; }
    [[nodiscard]] QString problemsText() const;
    [[nodiscard]] QComboBox* drawingBox() const { return m_drawingBox; }
    [[nodiscard]] QComboBox* sizeBox() const { return m_sizeBox; }
    [[nodiscard]] QComboBox* standardBox() const { return m_standardBox; }
    [[nodiscard]] QLineEdit* titleField() const { return m_title; }
    [[nodiscard]] QLineEdit* partNumberField() const { return m_partNumber; }
    [[nodiscard]] QComboBox* scaleBox() const { return m_scale; }
    [[nodiscard]] QCheckBox* hiddenBox() const { return m_hidden; }
    [[nodiscard]] QLineEdit* suffixField() const { return m_suffix; }
    [[nodiscard]] QDoubleSpinBox* offsetField() const { return m_offset; }

signals:
    /// The user changed the drawing shown. `label` names the change for undo.
    void changed(const lcad::document::Drawing& drawing, const QString& label);
    /// The user chose another drawing.
    void drawingChosen(lcad::document::ObjectId drawing);

private:
    void fill();
    void edit(const QString& label);

    const document::Drawing* m_shown = nullptr;
    document::Drawing m_drawing;
    bool m_hasDrawing = false;
    SheetView::Selected m_selected;
    bool m_filling = false;
    QString m_promptText;

    QLabel* m_prompt;
    QComboBox* m_drawingBox;
    QLineEdit* m_name;
    QComboBox* m_sizeBox;
    QComboBox* m_standardBox;
    QLineEdit* m_title;
    QLineEdit* m_partNumber;
    QLineEdit* m_revision;
    QLineEdit* m_drawnBy;
    QLineEdit* m_date;
    QLineEdit* m_material;
    QLineEdit* m_company;
    QGroupBox* m_viewBox;
    QLabel* m_viewKind;
    QComboBox* m_scale;
    QCheckBox* m_hidden;
    QCheckBox* m_centerMarks;
    QCheckBox* m_flipped;
    QGroupBox* m_dimensionBox;
    QLineEdit* m_suffix;
    QDoubleSpinBox* m_offset;
    QLabel* m_problems;
};

} // namespace lcad::app
