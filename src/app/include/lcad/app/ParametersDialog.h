// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/Document.h>

#include <QDialog>

class QLabel;
class QTableWidget;
class QTableWidgetItem;

namespace lcad::app {

/// The parameter table: each parameter's name, the expression it is worked out from, what it
/// comes to, and a comment. Names, expressions, and comments are edited in place; each edit is
/// an undo step of the document. A value that cannot be worked out says why.
class ParametersDialog : public QDialog {
    Q_OBJECT

public:
    explicit ParametersDialog(QWidget* parent = nullptr);
    ~ParametersDialog() override;

    /// Shows a document's parameters and follows them. The document has to stay alive until
    /// another one, or nullptr, is set.
    void setDocument(document::Document* document);
    [[nodiscard]] QTableWidget* table() const { return m_table; }
    /// What the last edit that was refused was refused for, or an empty string.
    [[nodiscard]] QString message() const;

    /// Adds a parameter with a free name, and opens its name for typing.
    void addParameter();
    /// Deletes the parameters whose rows are selected, as one undo step.
    void removeSelected();

    enum Column { Name = 0, Expression = 1, Value = 2, Comment = 3 };

private:
    void refresh();
    void onItemChanged(QTableWidgetItem* item);

    document::Document* m_document = nullptr;
    document::Document::ListenerId m_listener = 0;
    QTableWidget* m_table;
    QLabel* m_message;
    QLabel* m_problems = nullptr;
    bool m_filling = false;
    bool m_refreshQueued = false;
};

} // namespace lcad::app
