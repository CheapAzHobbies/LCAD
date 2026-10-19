// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/ObjectId.h>
#include <lcad/kernel/Check.h>

#include <QDialog>
#include <QString>

#include <vector>

class QPushButton;
class QTreeWidget;

namespace lcad::app {

/// What Check Geometry found in one body.
struct BodyCheck {
    document::ObjectId body;
    QString name;
    std::vector<kernel::GeometryProblem> problems;
};

/// Lists what is wrong with each body checked, or that it is sound. Choosing a problem shows
/// the faces and edges it is about; Repair starts a Repair of the body chosen.
class GeometryCheckDialog : public QDialog {
    Q_OBJECT

public:
    explicit GeometryCheckDialog(QWidget* parent = nullptr);

    void setResults(const std::vector<BodyCheck>& results);
    [[nodiscard]] QTreeWidget* tree() const { return m_tree; }

signals:
    /// A problem was chosen: its faces and edges, by number, of `body`.
    void problemChosen(lcad::document::ObjectId body, const std::vector<int>& faces,
                       const std::vector<int>& edges);
    void repairRequested(lcad::document::ObjectId body);

private:
    void selectionChanged();

    QTreeWidget* m_tree;
    QPushButton* m_repair;
    std::vector<BodyCheck> m_results;
};

} // namespace lcad::app
