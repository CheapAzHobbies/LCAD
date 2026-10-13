// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/Document.h>
#include <lcad/viewport/ViewportWidget.h>

#include <QStringList>
#include <QWidget>

#include <functional>
#include <vector>

class QCheckBox;
class QFormLayout;
class QLabel;
class QLineEdit;

namespace lcad::app {

/// What the right-hand panel shows while no command is running: the properties of the
/// selection. For one body that is its name and visibility, which can be changed here, and its
/// measurements. For one face, edge, or vertex it is what that measures.
///
/// Every number is measured on the exact geometry when the panel is refreshed.
class PropertiesPanel : public QWidget {
    Q_OBJECT

public:
    explicit PropertiesPanel(QWidget* parent = nullptr);

    /// Shows the properties of the given selection of `document`, which may be nullptr.
    void refresh(document::Document* document, const std::vector<document::ObjectId>& bodies,
                 const std::vector<viewport::SelectedItem>& parts);

    /// The line at the top: "Box", "Face of Box", "3 items selected", "Nothing selected".
    [[nodiscard]] QString heading() const;
    /// The read-only rows below it, as "Label: value".
    [[nodiscard]] QStringList rows() const;

private:
    void clearRows();
    void addRow(const QString& label, const QString& value);

    QLabel* m_heading;
    QFormLayout* m_form;
    QLineEdit* m_name = nullptr;
    QCheckBox* m_visible = nullptr;
    QStringList m_rows;
    document::Document* m_document = nullptr;
    document::ObjectId m_body;
    // Set while an edit made in the panel is on its way through the document. What that edit
    // leads to must not rebuild the panel under the control that is making it.
    bool m_applying = false;
    bool m_refreshWaiting = false;
    std::vector<document::ObjectId> m_waitingBodies;
    std::vector<viewport::SelectedItem> m_waitingParts;
    void applyEdit(const std::function<void()>& edit);
};

/// A number as the interface shows it: three decimals at most, without trailing zeros.
[[nodiscard]] QString formatNumber(double value);

} // namespace lcad::app
