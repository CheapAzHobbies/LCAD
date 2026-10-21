// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/Document.h>
#include <lcad/document/Links.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Shape.h>

#include <QDialog>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

class QLabel;
class QPushButton;
class QTableWidget;
class QTreeWidget;

// The dialogs of the assembly tools: what overlaps, what things weigh, what they are made of,
// and which files the document links to. Each shows what the window works out, and says what
// the user asks for by signals.
namespace lcad::app {

/// Two parts that share space, and the space they share, in the root's coordinates.
struct Interference {
    QString first;
    QString second;
    double volume = 0.0;
    kernel::Shape overlap;
};

/// Lists the parts that overlap. Choosing one shows where.
class InterferenceDialog : public QDialog {
    Q_OBJECT

public:
    explicit InterferenceDialog(QWidget* parent = nullptr);

    /// `pairs` is how many pairs of bodies were looked at, `problems` what could not be.
    void setResults(const std::vector<Interference>& results, int pairs,
                    const QStringList& problems);
    [[nodiscard]] QTreeWidget* tree() const { return m_tree; }
    [[nodiscard]] QString summary() const;

signals:
    /// The user chose an entry: show its overlap. -1 for none.
    void interferenceChosen(int index);

private:
    QLabel* m_summary;
    QTreeWidget* m_tree;
};

/// One body's share of the mass properties.
struct MassRow {
    QString name;
    /// Empty when the body is not said to be made of anything.
    QString material;
    double volume = 0.0;
    std::optional<double> mass;
};

/// What the mass properties of a selection come to.
struct MassSummary {
    std::vector<MassRow> rows;
    /// Of the bodies whose material is known, together, in the root's coordinates.
    std::optional<kernel::MassProperties> total;
    double volume = 0.0;
    double area = 0.0;
    /// How many bodies have no material, and so no mass.
    int unknown = 0;
};

/// Physical properties: mass, volume, center of mass, and inertia, body by body and together.
class MassDialog : public QDialog {
    Q_OBJECT

public:
    explicit MassDialog(QWidget* parent = nullptr);

    /// `scope` says what was weighed: "3 bodies selected", "Everything shown".
    void setSummary(const MassSummary& summary, const QString& scope);
    /// The totals as "Name: value" lines, as shown.
    [[nodiscard]] QStringList totals() const { return m_totals; }
    [[nodiscard]] QTableWidget* table() const { return m_table; }

private:
    QLabel* m_scope;
    QTableWidget* m_table;
    QLabel* m_totalsLabel;
    QStringList m_totals;
};

/// The materials a document can use: LCAD's own, which cannot be changed, and the document's,
/// which can be added, renamed, given another density, and removed.
class MaterialsDialog : public QDialog {
    Q_OBJECT

public:
    explicit MaterialsDialog(QWidget* parent = nullptr);

    /// Shows a document's materials and edits them. The document has to stay alive until
    /// another one, or nullptr, is set.
    void setDocument(document::Document* document);
    [[nodiscard]] QTableWidget* table() const { return m_table; }

private:
    void fill();

    document::Document* m_document = nullptr;
    document::Document::ListenerId m_listener = 0;
    QTableWidget* m_table;
    QPushButton* m_remove;
    bool m_filling = false;
};

/// The files the document links to, and those they link to, with what state each is in.
class LinksDialog : public QDialog {
    Q_OBJECT

public:
    explicit LinksDialog(QWidget* parent = nullptr);

    void setStatuses(const std::vector<document::LinkStatus>& statuses);
    [[nodiscard]] QTreeWidget* tree() const { return m_tree; }

signals:
    void updateRequested();
    /// Find the file of this linked component elsewhere.
    void relinkRequested(lcad::document::ObjectId component);
    void openRequested(const QString& path);
    void packageRequested();

private:
    QTreeWidget* m_tree;
    QPushButton* m_update;
    QPushButton* m_relink;
    QPushButton* m_open;
    QLabel* m_summary;
};

/// What the state of a link is called: "Up to date", "Changed", "Missing", "Cannot be read".
[[nodiscard]] QString linkStateName(document::LinkStatus::State state);

} // namespace lcad::app
