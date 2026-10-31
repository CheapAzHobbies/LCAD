// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/DrawingWorkspace.h>

#include <lcad/app/Paths.h>
#include <lcad/drawing/Dxf.h>

#include <QFileInfo>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace lcad::app {

using document::DimensionKind;
using document::Drawing;
using document::DrawingDimension;
using document::DrawingNote;
using document::DrawingPoint;
using document::DrawingView;
using document::ObjectId;
using document::ViewKind;
using drawing::Point2D;

namespace {

double distanceTo(const Point2D& p, const Point2D& a, const Point2D& b) {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double squared = dx * dx + dy * dy;
    const double along =
        squared > 0.0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / squared, 0.0, 1.0) : 0.0;
    return std::hypot(p.x - a.x - along * dx, p.y - a.y - along * dy);
}

} // namespace

DrawingWorkspace::DrawingWorkspace(QWidget* parent)
    : QObject(parent), m_view(new SheetView(parent)), m_panel(new DrawingPanel(parent)) {
    connect(m_view, &SheetView::selectionChanged, this, [this] {
        m_panel->setSelected(m_view->selected());
        emit stateChanged();
    });
    connect(m_view, &SheetView::clicked, this, &DrawingWorkspace::onClicked);
    connect(m_view, &SheetView::canceled, this, [this] {
        if (m_tool != Tool::Select) {
            setTool(Tool::Select);
        } else {
            m_view->setSelected({});
            m_panel->setSelected({});
            emit stateChanged();
        }
    });
    connect(m_view, &SheetView::viewMoved, this, [this](ObjectId id, double x, double y) {
        Drawing changed = *drawing();
        for (DrawingView& view : changed.views) {
            if (view.id == id) {
                view.x = x;
                view.y = y;
            }
        }
        change(changed, tr("Move View"));
    });
    connect(m_view, &SheetView::noteMoved, this, [this](ObjectId id, double x, double y) {
        Drawing changed = *drawing();
        for (DrawingNote& note : changed.notes) {
            if (note.id == id) {
                note.x = x;
                note.y = y;
            }
        }
        change(changed, tr("Move Note"));
    });
    connect(m_view, &SheetView::textEntered, this, [this](const Point2D& at, const QString& text) {
        if (drawing() == nullptr || text.trimmed().isEmpty()) {
            return;
        }
        Drawing changed = *drawing();
        DrawingNote note;
        note.id = m_document->newId();
        note.text = text.toStdString();
        note.x = at.x;
        note.y = at.y;
        changed.notes.push_back(std::move(note));
        change(changed, tr("Add Note"));
        setTool(Tool::Select);
    });
    connect(m_panel, &DrawingPanel::changed, this,
            [this](const Drawing& changed, const QString& label) { change(changed, label); });
    connect(m_panel, &DrawingPanel::drawingChosen, this, [this](ObjectId id) {
        m_current = id;
        m_view->setSelected({});
        m_panel->setSelected({});
        rebuild();
    });
}

DrawingWorkspace::~DrawingWorkspace() {
    if (m_document != nullptr) {
        m_document->removeListener(m_listener);
    }
}

void DrawingWorkspace::setDocument(document::Document* document) {
    if (m_document != nullptr) {
        m_document->removeListener(m_listener);
    }
    m_document = document;
    m_builder.reset();
    m_current = {};
    m_tool = Tool::Select;
    m_pending.reset();
    if (document != nullptr) {
        m_listener = document->addListener(
            [this](const document::Change& change) { onDocumentChanged(change); });
        m_builder = std::make_unique<drawing::SheetBuilder>(*document);
        if (!document->drawings().empty()) {
            m_current = document->drawings().front().id;
        }
    }
    m_view->setSelected({});
    m_panel->setSelected({});
    rebuild();
}

void DrawingWorkspace::setShown(bool shown) {
    m_shown = shown;
    if (shown) {
        rebuild();
    }
}

const Drawing* DrawingWorkspace::drawing() const {
    return m_document != nullptr ? m_document->findDrawing(m_current) : nullptr;
}

void DrawingWorkspace::onDocumentChanged(const document::Change& change) {
    using Kind = document::Change::Kind;
    switch (change.kind) {
    case Kind::BodyAdded:
    case Kind::BodyRemoved:
    case Kind::BodyVisibilityChanged:
    case Kind::BodyGeometryChanged:
    case Kind::PlacementsChanged:
    case Kind::AssemblyChanged:
        if (m_builder != nullptr) {
            m_builder->invalidate();
        }
        rebuild();
        break;
    case Kind::DrawingsChanged:
        if (drawing() == nullptr) {
            m_current =
                m_document->drawings().empty() ? ObjectId{} : m_document->drawings().front().id;
            m_view->setSelected({});
            m_panel->setSelected({});
        }
        rebuild();
        break;
    default:
        break;
    }
}

void DrawingWorkspace::rebuild() {
    const Drawing* current = drawing();
    m_panel->setDrawings(m_document != nullptr ? m_document->drawings() : std::vector<Drawing>{},
                         current);
    m_panel->setSelected(m_view->selected());
    m_panel->setPrompt(prompt());
    if (!m_shown) {
        return;
    }
    if (current == nullptr || m_builder == nullptr) {
        m_view->setSheet({});
        m_view->setNotes({});
        m_panel->setProblems({});
        emit stateChanged();
        return;
    }
    drawing::Sheet sheet = m_builder->build(*current);
    QStringList problems;
    for (const std::string& problem : sheet.problems) {
        problems.append(QString::fromStdString(problem));
    }
    m_panel->setProblems(problems);
    m_view->setSheet(std::move(sheet));
    m_view->setNotes(current->notes);
    emit stateChanged();
}

void DrawingWorkspace::change(const Drawing& drawing, const QString& label) {
    try {
        m_document->changeDrawing(drawing, label.toStdString());
    } catch (const std::invalid_argument& failure) {
        emit message(QString::fromUtf8(failure.what()));
        rebuild();
    }
}

void DrawingWorkspace::newDrawing() {
    if (m_document == nullptr) {
        return;
    }
    Drawing fresh;
    fresh.name = m_document->uniqueDrawingName("Drawing");
    drawing::addStandardViews(*m_document, fresh);
    m_current = m_document->addDrawing(std::move(fresh));
    m_view->setSelected({});
    rebuild();
    m_view->fit();
}

void DrawingWorkspace::addView(ViewKind kind) {
    if (drawing() == nullptr) {
        return;
    }
    Drawing changed = *drawing();
    const drawing::SheetDimensions size = drawing::sheetDimensions(changed.size);
    DrawingView view;
    view.id = m_document->newId();
    view.kind = kind;
    view.scale = changed.views.empty() ? 1.0 : changed.views.front().scale;
    view.x = size.width / 2.0;
    view.y = size.height / 2.0;
    view.showHidden = kind != ViewKind::Isometric;
    view.centerMarks = kind != ViewKind::Isometric;
    changed.views.push_back(view);
    change(changed, tr("Add View"));
    m_view->setSelected({SheetView::Selected::Kind::View, view.id});
    m_panel->setSelected(m_view->selected());
}

void DrawingWorkspace::setTool(Tool tool) {
    m_tool = tool;
    m_pending.reset();
    m_view->setMark(std::nullopt);
    m_view->setMode(tool == Tool::Select ? SheetView::Mode::Select : SheetView::Mode::Pick);
    m_panel->setPrompt(prompt());
    emit stateChanged();
}

QString DrawingWorkspace::prompt() const {
    switch (m_tool) {
    case Tool::Select:
        return {};
    case Tool::Dimension:
        return m_pending.has_value() ? tr("Click the second point")
                                     : tr("Click a line or a circle, or the first of two points");
    case Tool::SectionUp:
        return tr("Click the view where the cut goes, up it");
    case Tool::SectionAcross:
        return tr("Click the view where the cut goes, across it");
    case Tool::Detail:
        return tr("Click the middle of what to show larger");
    case Tool::Note:
        return tr("Click where the note goes");
    }
    return {};
}

void DrawingWorkspace::onClicked(const Point2D& at, Qt::KeyboardModifiers modifiers) {
    if (drawing() == nullptr) {
        return;
    }
    switch (m_tool) {
    case Tool::Select:
        break;
    case Tool::Dimension:
        clickDimension(at, modifiers);
        break;
    case Tool::SectionUp:
        clickSection(at, true);
        break;
    case Tool::SectionAcross:
        clickSection(at, false);
        break;
    case Tool::Detail:
        clickDetail(at);
        break;
    case Tool::Note:
        m_view->askForText(at);
        break;
    }
}

const drawing::ViewBox* DrawingWorkspace::viewAt(const Point2D& at) const {
    const auto& boxes = m_view->sheet().views;
    for (auto box = boxes.rbegin(); box != boxes.rend(); ++box) {
        const double reach = m_view->reach();
        if (at.x >= box->left - reach && at.x <= box->right + reach &&
            at.y >= box->bottom - reach && at.y <= box->top + reach) {
            return &*box;
        }
    }
    return nullptr;
}

void DrawingWorkspace::clickDimension(const Point2D& at, Qt::KeyboardModifiers modifiers) {
    const drawing::Sheet& sheet = m_view->sheet();
    const double reach = m_view->reach();

    // What is nearest: a point first, since a point lies on the ends of what it is on. The
    // middle of an edge counts only as a second point: a click there means the edge.
    const drawing::SnapPoint* nearest = nullptr;
    double best = reach;
    for (const drawing::SnapPoint& point : sheet.snapPoints) {
        if (m_pending.has_value() && point.view != m_pending->view) {
            continue;
        }
        if (point.point.where == DrawingPoint::Where::Middle && !m_pending.has_value()) {
            continue;
        }
        const double away = std::hypot(point.at.x - at.x, point.at.y - at.y);
        if (away <= best) {
            best = away;
            nearest = &point;
        }
    }

    if (m_pending.has_value()) {
        if (nearest == nullptr) {
            emit message(tr("Click a point of the same view."));
            return;
        }
        const drawing::SnapPoint first = *m_pending;
        const drawing::SnapPoint second = *nearest;
        m_pending.reset();
        m_view->setMark(std::nullopt);
        addLinear(first.view, first.point, first.at, second.point, second.at);
        m_panel->setPrompt(prompt());
        return;
    }
    if (nearest != nullptr) {
        m_pending = *nearest;
        m_view->setMark(nearest->at);
        m_panel->setPrompt(prompt());
        return;
    }

    // A circle or an arc: a whole circle by its diameter, an arc by its radius. Shift says the
    // other.
    for (const drawing::SnapCircle& circle : sheet.snapCircles) {
        const double fromCenter = std::hypot(circle.center.x - at.x, circle.center.y - at.y);
        if (std::abs(fromCenter - circle.radius) > reach) {
            continue;
        }
        const bool shifted = modifiers.testFlag(Qt::ShiftModifier);
        DrawingDimension dimension;
        dimension.id = m_document->newId();
        dimension.view = circle.view;
        dimension.kind = circle.whole != shifted ? DimensionKind::Diameter : DimensionKind::Radius;
        dimension.first = circle.point;
        dimension.offset = 8.0;
        Drawing changed = *drawing();
        changed.dimensions.push_back(std::move(dimension));
        change(changed, tr("Add Dimension"));
        return;
    }

    // A straight edge: its length.
    for (const drawing::SnapEdge& edge : sheet.snapEdges) {
        if (distanceTo(at, edge.from, edge.to) <= reach) {
            addLinear(edge.view, edge.start, edge.from, edge.end, edge.to);
            return;
        }
    }
    emit message(tr("Click a line, a circle, or a point of a view."));
}

// A distance between two points, level or upright where they are, set outside the view on the
// side they lie nearest, beyond the dimensions already there.
void DrawingWorkspace::addLinear(ObjectId view, const DrawingPoint& first, const Point2D& from,
                                 const DrawingPoint& second, const Point2D& to) {
    DrawingDimension dimension;
    dimension.id = m_document->newId();
    dimension.view = view;
    dimension.first = first;
    dimension.second = second;
    const double dx = to.x - from.x;
    const double dy = to.y - from.y;
    const double least = 1e-6;
    Point2D across{0.0, 1.0};
    if (std::abs(dy) < least) {
        dimension.kind = DimensionKind::Horizontal;
    } else if (std::abs(dx) < least) {
        dimension.kind = DimensionKind::Vertical;
        across = {-1.0, 0.0};
    } else {
        dimension.kind = DimensionKind::Aligned;
        const double length = std::hypot(dx, dy);
        across = {-dy / length, dx / length};
    }
    const auto onAcross = [&across](const Point2D& p) { return p.x * across.x + p.y * across.y; };

    double low = 0.0;
    double high = 0.0;
    for (const drawing::ViewBox& box : m_view->sheet().views) {
        if (box.view != view) {
            continue;
        }
        low = high = onAcross({box.left, box.bottom});
        for (const Point2D& corner : {Point2D{box.right, box.bottom}, Point2D{box.left, box.top},
                                      Point2D{box.right, box.top}}) {
            low = std::min(low, onAcross(corner));
            high = std::max(high, onAcross(corner));
        }
    }
    const double pointA = onAcross(from);
    const double pointB = onAcross(to);
    const bool outward = (pointA + pointB) / 2.0 >= (low + high) / 2.0;

    Drawing changed = *drawing();
    int alongside = 0;
    for (const DrawingDimension& other : changed.dimensions) {
        if (other.view == view && other.kind == dimension.kind &&
            (other.offset >= 0.0) == outward) {
            ++alongside;
        }
    }
    const double stand = 10.0 + 8.0 * alongside;
    dimension.offset =
        outward ? high - std::max(pointA, pointB) + stand : low - std::min(pointA, pointB) - stand;
    changed.dimensions.push_back(std::move(dimension));
    change(changed, tr("Add Dimension"));
}

QString DrawingWorkspace::nextLabel() const {
    for (char letter = 'A'; letter <= 'Z'; ++letter) {
        const std::string label(1, letter);
        const auto& views = drawing()->views;
        if (std::none_of(views.begin(), views.end(),
                         [&label](const DrawingView& view) { return view.label == label; })) {
            return QString::fromStdString(label);
        }
    }
    return QStringLiteral("A");
}

void DrawingWorkspace::clickSection(const Point2D& at, bool vertical) {
    const drawing::ViewBox* box = viewAt(at);
    if (box == nullptr) {
        emit message(tr("Click a view."));
        return;
    }
    const Point2D inView = box->toView(at);
    Drawing changed = *drawing();
    DrawingView section;
    section.id = m_document->newId();
    section.kind = ViewKind::Section;
    section.label = nextLabel().toStdString();
    section.parent = box->view;
    section.sectionVertical = vertical;
    section.sectionPosition = vertical ? inView.x : inView.y;
    section.scale = box->scale;
    section.showHidden = false;
    // Beside the parent if the sheet has the room, and otherwise on the other side.
    const drawing::SheetDimensions size = drawing::sheetDimensions(changed.size);
    const double wide = box->right - box->left;
    const double tall = box->top - box->bottom;
    if (vertical) {
        section.y = box->y;
        section.x = box->right + wide / 2.0 + 30.0;
        if (section.x + wide / 2.0 > size.width - 10.0) {
            section.x = box->left - wide / 2.0 - 30.0;
        }
    } else {
        section.x = box->x;
        section.y = box->bottom - tall / 2.0 - 30.0;
        if (section.y - tall / 2.0 < 10.0) {
            section.y = box->top + tall / 2.0 + 30.0;
        }
    }
    changed.views.push_back(section);
    change(changed, tr("Add Section"));
    setTool(Tool::Select);
    m_view->setSelected({SheetView::Selected::Kind::View, section.id});
    m_panel->setSelected(m_view->selected());
}

void DrawingWorkspace::clickDetail(const Point2D& at) {
    const drawing::ViewBox* box = viewAt(at);
    if (box == nullptr) {
        emit message(tr("Click a view."));
        return;
    }
    const Point2D inView = box->toView(at);
    Drawing changed = *drawing();
    DrawingView detail;
    detail.id = m_document->newId();
    detail.kind = ViewKind::Detail;
    detail.label = nextLabel().toStdString();
    detail.parent = box->view;
    detail.detailX = inView.x;
    detail.detailY = inView.y;
    detail.detailRadius = 15.0 / box->scale;
    detail.scale = box->scale * 2.0;
    const drawing::SheetDimensions size = drawing::sheetDimensions(changed.size);
    detail.x = std::min(box->right + 40.0, size.width - 45.0);
    detail.y = std::min(box->top, size.height - 45.0);
    changed.views.push_back(detail);
    change(changed, tr("Add Detail"));
    setTool(Tool::Select);
    m_view->setSelected({SheetView::Selected::Kind::View, detail.id});
    m_panel->setSelected(m_view->selected());
}

bool DrawingWorkspace::deleteSelected() {
    const SheetView::Selected selected = m_view->selected();
    if (drawing() == nullptr || selected.kind == SheetView::Selected::Kind::None) {
        return false;
    }
    Drawing changed = *drawing();
    switch (selected.kind) {
    case SheetView::Selected::Kind::View: {
        // With what hangs on it: the sections and details taken from it, and its dimensions.
        std::vector<ObjectId> gone{selected.id};
        for (std::size_t i = 0; i < gone.size(); ++i) {
            for (const DrawingView& view : changed.views) {
                if (view.parent == gone[i]) {
                    gone.push_back(view.id);
                }
            }
        }
        const auto isGone = [&gone](ObjectId id) {
            return std::find(gone.begin(), gone.end(), id) != gone.end();
        };
        std::erase_if(changed.views, [&](const DrawingView& view) { return isGone(view.id); });
        std::erase_if(changed.dimensions,
                      [&](const DrawingDimension& dimension) { return isGone(dimension.view); });
        break;
    }
    case SheetView::Selected::Kind::Dimension:
        std::erase_if(changed.dimensions, [&selected](const DrawingDimension& dimension) {
            return dimension.id == selected.id;
        });
        break;
    case SheetView::Selected::Kind::Note:
        std::erase_if(changed.notes,
                      [&selected](const DrawingNote& note) { return note.id == selected.id; });
        break;
    case SheetView::Selected::Kind::None:
        break;
    }
    m_view->setSelected({});
    m_panel->setSelected({});
    change(changed, tr("Delete"));
    return true;
}

bool DrawingWorkspace::exportTo(const QString& path, QString* error) {
    const Drawing* current = drawing();
    if (current == nullptr || m_builder == nullptr) {
        if (error != nullptr) {
            *error = tr("There is no drawing to export.");
        }
        return false;
    }
    const drawing::Sheet sheet = m_builder->build(*current);
    if (QFileInfo(path).suffix().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) != 0) {
        try {
            drawing::writeDxf(sheet, toPath(path));
        } catch (const std::runtime_error& failure) {
            if (error != nullptr) {
                *error = QString::fromUtf8(failure.what());
            }
            return false;
        }
        return true;
    }
    QPdfWriter writer(path);
    writer.setCreator(QStringLiteral("LCAD"));
    writer.setTitle(QString::fromStdString(current->name));
    writer.setResolution(1200);
    writer.setPageSize(QPageSize(QSizeF(sheet.width, sheet.height), QPageSize::Millimeter));
    writer.setPageMargins(QMarginsF(0.0, 0.0, 0.0, 0.0));
    QPainter painter;
    if (!painter.begin(&writer)) {
        if (error != nullptr) {
            *error = tr("Could not write %1.").arg(path);
        }
        return false;
    }
    const double scale = writer.resolution() / 25.4;
    paintSheet(painter, sheet, QPointF(0.0, sheet.height * scale), scale);
    painter.end();
    return true;
}

} // namespace lcad::app
