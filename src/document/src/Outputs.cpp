// SPDX-License-Identifier: GPL-3.0-or-later

// What is made from the model: drawings and machining setups, and the tools they use. The parts
// of the Document that keep them, and the names their kinds have in a file.

#include <lcad/document/Document.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace lcad::document {

namespace {

constexpr std::size_t notFound = static_cast<std::size_t>(-1);

template <typename Kind, std::size_t N>
const char* nameIn(const std::array<std::pair<Kind, const char*>, N>& names, Kind kind) {
    for (const auto& [value, name] : names) {
        if (value == kind) {
            return name;
        }
    }
    return "";
}

template <typename Kind, std::size_t N>
bool kindIn(const std::array<std::pair<Kind, const char*>, N>& names, const std::string& name,
            Kind& kind) {
    for (const auto& [value, text] : names) {
        if (name == text) {
            kind = value;
            return true;
        }
    }
    return false;
}

constexpr std::array<std::pair<SheetSize, const char*>, 9> sheetSizes{{
    {SheetSize::A4, "A4"},
    {SheetSize::A3, "A3"},
    {SheetSize::A2, "A2"},
    {SheetSize::A1, "A1"},
    {SheetSize::A0, "A0"},
    {SheetSize::Letter, "Letter"},
    {SheetSize::Tabloid, "Tabloid"},
    {SheetSize::AnsiC, "ANSI C"},
    {SheetSize::AnsiD, "ANSI D"},
}};

constexpr std::array<std::pair<ViewKind, const char*>, 9> viewKinds{{
    {ViewKind::Front, "front"},
    {ViewKind::Back, "back"},
    {ViewKind::Top, "top"},
    {ViewKind::Bottom, "bottom"},
    {ViewKind::Left, "left"},
    {ViewKind::Right, "right"},
    {ViewKind::Isometric, "isometric"},
    {ViewKind::Section, "section"},
    {ViewKind::Detail, "detail"},
}};

constexpr std::array<std::pair<DimensionKind, const char*>, 5> dimensionKinds{{
    {DimensionKind::Aligned, "aligned"},
    {DimensionKind::Horizontal, "horizontal"},
    {DimensionKind::Vertical, "vertical"},
    {DimensionKind::Radius, "radius"},
    {DimensionKind::Diameter, "diameter"},
}};

constexpr std::array<std::pair<ToolKind, const char*>, 3> toolKinds{{
    {ToolKind::FlatEndMill, "flat"},
    {ToolKind::BallEndMill, "ball"},
    {ToolKind::Drill, "drill"},
}};

constexpr std::array<std::pair<PostDialect, const char*>, 2> postDialects{{
    {PostDialect::Grbl, "grbl"},
    {PostDialect::LinuxCnc, "linuxcnc"},
}};

constexpr std::array<std::pair<WorkOrigin, const char*>, 2> workOrigins{{
    {WorkOrigin::StockTopCorner, "stock top corner"},
    {WorkOrigin::StockTopCenter, "stock top center"},
}};

constexpr std::array<std::pair<OperationKind, const char*>, 4> operationKinds{{
    {OperationKind::Face, "face"},
    {OperationKind::Pocket, "pocket"},
    {OperationKind::Contour, "contour"},
    {OperationKind::Drill, "drill"},
}};

bool isBlank(const std::string& name) {
    return name.find_first_not_of(" \t\r\n") == std::string::npos;
}

void requireName(const std::string& name, const char* function) {
    if (isBlank(name)) {
        throw std::invalid_argument(std::string(function) + ": it needs a name");
    }
}

// Each view has to be something a sheet can show.
void requireViews(const Drawing& drawing, const char* function) {
    for (const DrawingView& view : drawing.views) {
        if (!std::isfinite(view.scale) || view.scale <= 0.0 || !std::isfinite(view.x) ||
            !std::isfinite(view.y)) {
            throw std::invalid_argument(std::string(function) +
                                        ": a view needs a scale above zero and a place");
        }
    }
}

template <typename Item>
std::size_t indexIn(const std::vector<Item>& items, ObjectId id) {
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].id == id) {
            return i;
        }
    }
    return notFound;
}

template <typename Item>
const Item* findIn(const std::vector<Item>& items, ObjectId id) {
    const std::size_t index = indexIn(items, id);
    return index == notFound ? nullptr : &items[index];
}

template <typename Item>
void addOrRemove(std::vector<Item>& items, std::size_t index, const Item& item, bool add) {
    const auto place = items.begin() + static_cast<std::ptrdiff_t>(std::min(index, items.size()));
    if (add) {
        items.insert(place, item);
    } else {
        items.erase(items.begin() + static_cast<std::ptrdiff_t>(indexIn(items, item.id)));
    }
}

template <typename Item>
void change(std::vector<Item>& items, const Item& item) {
    items[indexIn(items, item.id)] = item;
}

template <typename Item>
std::string uniqueName(const std::vector<Item>& items, std::string_view base) {
    for (int number = 1;; ++number) {
        std::string name = std::string(base) + " " + std::to_string(number);
        if (std::none_of(items.begin(), items.end(),
                         [&name](const Item& item) { return item.name == name; })) {
            return name;
        }
    }
}

} // namespace

const char* toString(SheetSize size) {
    return nameIn(sheetSizes, size);
}
bool sheetSizeFromString(const std::string& name, SheetSize& size) {
    return kindIn(sheetSizes, name, size);
}
const char* toString(ViewKind kind) {
    return nameIn(viewKinds, kind);
}
bool viewKindFromString(const std::string& name, ViewKind& kind) {
    return kindIn(viewKinds, name, kind);
}
const char* toString(DimensionKind kind) {
    return nameIn(dimensionKinds, kind);
}
bool dimensionKindFromString(const std::string& name, DimensionKind& kind) {
    return kindIn(dimensionKinds, name, kind);
}
const char* toString(ToolKind kind) {
    return nameIn(toolKinds, kind);
}
bool toolKindFromString(const std::string& name, ToolKind& kind) {
    return kindIn(toolKinds, name, kind);
}
const char* toString(PostDialect dialect) {
    return nameIn(postDialects, dialect);
}
bool postDialectFromString(const std::string& name, PostDialect& dialect) {
    return kindIn(postDialects, name, dialect);
}
const char* toString(WorkOrigin origin) {
    return nameIn(workOrigins, origin);
}
bool workOriginFromString(const std::string& name, WorkOrigin& origin) {
    return kindIn(workOrigins, name, origin);
}
const char* toString(OperationKind kind) {
    return nameIn(operationKinds, kind);
}
bool operationKindFromString(const std::string& name, OperationKind& kind) {
    return kindIn(operationKinds, name, kind);
}

ObjectId Document::newId() {
    return ObjectId{m_nextId++};
}

const Drawing* Document::findDrawing(ObjectId id) const {
    return findIn(m_drawings, id);
}

std::string Document::uniqueDrawingName(std::string_view base) const {
    return uniqueName(m_drawings, base);
}

ObjectId Document::addDrawing(Drawing drawing) {
    requireName(drawing.name, "addDrawing");
    requireViews(drawing, "addDrawing");
    drawing.id = newId();
    Edit edit{};
    edit.kind = Edit::Kind::AddDrawing;
    edit.id = drawing.id;
    edit.index = m_drawings.size();
    edit.drawing = std::move(drawing);
    record(std::move(edit), "New Drawing");
    return m_drawings.back().id;
}

void Document::changeDrawing(const Drawing& drawing, const std::string& label) {
    const std::size_t index = indexIn(m_drawings, drawing.id);
    if (index == notFound) {
        throw std::invalid_argument("changeDrawing: the document has no such drawing");
    }
    requireName(drawing.name, "changeDrawing");
    requireViews(drawing, "changeDrawing");
    if (m_drawings[index] == drawing) {
        return;
    }
    Edit edit{};
    edit.kind = Edit::Kind::ChangeDrawing;
    edit.id = drawing.id;
    edit.drawing = m_drawings[index];
    edit.newDrawing = drawing;
    record(std::move(edit), label);
}

void Document::removeDrawing(ObjectId id) {
    const std::size_t index = indexIn(m_drawings, id);
    if (index == notFound) {
        throw std::invalid_argument("removeDrawing: the document has no such drawing");
    }
    Edit edit{};
    edit.kind = Edit::Kind::RemoveDrawing;
    edit.id = id;
    edit.index = index;
    edit.drawing = m_drawings[index];
    record(std::move(edit), "Delete Drawing");
}

const Tool* Document::findTool(ObjectId id) const {
    return findIn(m_tools, id);
}

ObjectId Document::addTool(Tool tool) {
    requireName(tool.name, "addTool");
    if (!std::isfinite(tool.diameter) || tool.diameter <= 0.0) {
        throw std::invalid_argument("addTool: a tool needs a diameter above zero");
    }
    tool.id = newId();
    Edit edit{};
    edit.kind = Edit::Kind::AddTool;
    edit.id = tool.id;
    edit.index = m_tools.size();
    edit.tool = std::move(tool);
    record(std::move(edit), "Add Tool");
    return m_tools.back().id;
}

void Document::changeTool(const Tool& tool) {
    const std::size_t index = indexIn(m_tools, tool.id);
    if (index == notFound) {
        throw std::invalid_argument("changeTool: the document has no such tool");
    }
    requireName(tool.name, "changeTool");
    if (!std::isfinite(tool.diameter) || tool.diameter <= 0.0) {
        throw std::invalid_argument("changeTool: a tool needs a diameter above zero");
    }
    if (m_tools[index] == tool) {
        return;
    }
    Edit edit{};
    edit.kind = Edit::Kind::ChangeTool;
    edit.id = tool.id;
    edit.tool = m_tools[index];
    edit.newTool = tool;
    record(std::move(edit), "Change Tool");
}

void Document::removeTool(ObjectId id) {
    const std::size_t index = indexIn(m_tools, id);
    if (index == notFound) {
        throw std::invalid_argument("removeTool: the document has no such tool");
    }
    for (const Setup& setup : m_setups) {
        for (const MachiningOperation& operation : setup.operations) {
            if (operation.tool == id) {
                throw std::invalid_argument("removeTool: the operation \"" + operation.name +
                                            "\" uses it");
            }
        }
    }
    Edit edit{};
    edit.kind = Edit::Kind::RemoveTool;
    edit.id = id;
    edit.index = index;
    edit.tool = m_tools[index];
    record(std::move(edit), "Delete Tool");
}

const Setup* Document::findSetup(ObjectId id) const {
    return findIn(m_setups, id);
}

std::string Document::uniqueSetupName(std::string_view base) const {
    return uniqueName(m_setups, base);
}

ObjectId Document::addSetup(Setup setup) {
    requireName(setup.name, "addSetup");
    setup.id = newId();
    Edit edit{};
    edit.kind = Edit::Kind::AddSetup;
    edit.id = setup.id;
    edit.index = m_setups.size();
    edit.setup = std::move(setup);
    record(std::move(edit), "New Setup");
    return m_setups.back().id;
}

void Document::changeSetup(const Setup& setup, const std::string& label) {
    const std::size_t index = indexIn(m_setups, setup.id);
    if (index == notFound) {
        throw std::invalid_argument("changeSetup: the document has no such setup");
    }
    requireName(setup.name, "changeSetup");
    if (m_setups[index] == setup) {
        return;
    }
    Edit edit{};
    edit.kind = Edit::Kind::ChangeSetup;
    edit.id = setup.id;
    edit.setup = m_setups[index];
    edit.newSetup = setup;
    record(std::move(edit), label);
}

void Document::removeSetup(ObjectId id) {
    const std::size_t index = indexIn(m_setups, id);
    if (index == notFound) {
        throw std::invalid_argument("removeSetup: the document has no such setup");
    }
    Edit edit{};
    edit.kind = Edit::Kind::RemoveSetup;
    edit.id = id;
    edit.index = index;
    edit.setup = m_setups[index];
    record(std::move(edit), "Delete Setup");
}

void Document::applyOutput(const Edit& edit, bool forward) {
    switch (edit.kind) {
    case Edit::Kind::AddDrawing:
    case Edit::Kind::RemoveDrawing:
        addOrRemove(m_drawings, edit.index, edit.drawing,
                    (edit.kind == Edit::Kind::AddDrawing) == forward);
        notify(Change::Kind::DrawingsChanged, edit.id);
        break;
    case Edit::Kind::ChangeDrawing:
        change(m_drawings, forward ? edit.newDrawing : edit.drawing);
        notify(Change::Kind::DrawingsChanged, edit.id);
        break;
    case Edit::Kind::AddTool:
    case Edit::Kind::RemoveTool:
        addOrRemove(m_tools, edit.index, edit.tool, (edit.kind == Edit::Kind::AddTool) == forward);
        notify(Change::Kind::MachiningChanged, edit.id);
        break;
    case Edit::Kind::ChangeTool:
        change(m_tools, forward ? edit.newTool : edit.tool);
        notify(Change::Kind::MachiningChanged, edit.id);
        break;
    case Edit::Kind::AddSetup:
    case Edit::Kind::RemoveSetup:
        addOrRemove(m_setups, edit.index, edit.setup,
                    (edit.kind == Edit::Kind::AddSetup) == forward);
        notify(Change::Kind::MachiningChanged, edit.id);
        break;
    case Edit::Kind::ChangeSetup:
        change(m_setups, forward ? edit.newSetup : edit.setup);
        notify(Change::Kind::MachiningChanged, edit.id);
        break;
    default:
        break;
    }
}

} // namespace lcad::document
