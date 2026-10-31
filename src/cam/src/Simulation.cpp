// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/cam/Simulation.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <string>

namespace lcad::cam {

using document::Tool;
using document::ToolKind;
using kernel::Point;

namespace {

constexpr double pi = 3.14159265358979323846;

// The tool is thought of as this much smaller than it is when it is held against the part, and
// what it cuts counts only this much below the part's surface. The part's surface here is
// triangles, and the tool's path is offset to a thousandth or so: without these, a tool that runs
// exactly along a wall would be said to cut into it.
constexpr double radialAllowance = 0.03;
constexpr double depthAllowance = 0.02;

constexpr float noPart = -std::numeric_limits<float>::max();

// How far above its tip a tool's surface is, at a distance from its middle.
double profile(const Tool& tool, double distance) {
    const double radius = tool.diameter / 2.0;
    switch (tool.kind) {
    case ToolKind::FlatEndMill:
        return 0.0;
    case ToolKind::BallEndMill:
        return radius - std::sqrt(std::max(0.0, radius * radius - distance * distance));
    case ToolKind::Drill:
        return distance / std::tan(tool.tipAngle * pi / 360.0);
    }
    return 0.0;
}

std::string at(double x, double y) {
    const auto text = [](double value) {
        char buffer[64];
        const auto end =
            std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, 1).ptr;
        return std::string(buffer, end);
    };
    return "(" + text(x) + ", " + text(y) + ")";
}

} // namespace

HeightField::HeightField(double left, double bottom, double width, double depth, double cell,
                         float height)
    : m_left(left), m_bottom(bottom), m_cell(cell),
      m_columns(std::max(1, static_cast<int>(std::ceil(width / cell)))),
      m_rows(std::max(1, static_cast<int>(std::ceil(depth / cell)))),
      m_heights(static_cast<std::size_t>(m_columns) * static_cast<std::size_t>(m_rows), height) {}

float HeightField::sample(double px, double py, float outside) const {
    const int column = columnAt(px);
    const int row = rowAt(py);
    if (column < 0 || row < 0 || column >= m_columns || row >= m_rows) {
        return outside;
    }
    return at(column, row);
}

struct Simulation::Impl {
    const Program& program;
    document::Machine machine;
    HeightField stock;
    HeightField part;
    SimulationReport report;
    /// All the moves, one list, each with the path it belongs to.
    struct Step {
        Move move;
        std::size_t path = 0;
    };
    std::vector<Step> steps;
    std::size_t done = 0;
    Point position;
    // What has been said already: a problem once for each kind and operation.
    std::map<std::pair<int, std::uint64_t>, bool> said;
    double cellArea = 1.0;

    Impl(const Program& given, const kernel::Mesh& mesh, const document::Machine& theMachine,
         double cell)
        : program(given), machine(theMachine) {
        const kernel::BoundingBox& box = program.work.stock;
        const double width = box.max.x - box.min.x;
        const double depth = box.max.y - box.min.y;
        if (cell <= 0.0) {
            // Fine enough to see a thousandth of the stock, within a grid of a few million.
            cell = std::clamp(std::sqrt(width * depth / 3000000.0), 0.1, 2.0);
        }
        cellArea = cell * cell;
        const double grow = 0.0;
        stock = HeightField(box.min.x - grow, box.min.y - grow, width + grow * 2.0,
                            depth + grow * 2.0, cell, static_cast<float>(box.max.z));
        part = HeightField(box.min.x - grow, box.min.y - grow, width + grow * 2.0,
                           depth + grow * 2.0, cell, noPart);
        rasterize(mesh);
        for (std::size_t i = 0; i < program.paths.size(); ++i) {
            for (const Move& move : program.paths[i].moves) {
                steps.push_back({move, i});
            }
        }
        if (!steps.empty()) {
            const Point& first = steps.front().move.to;
            position = {first.x, first.y, program.safeZ};
        }
    }

    // The highest the part is over the middle of each cell.
    void rasterize(const kernel::Mesh& mesh) {
        for (const auto& triangle : mesh.triangles) {
            const Point& a = mesh.vertices[triangle[0]];
            const Point& b = mesh.vertices[triangle[1]];
            const Point& c = mesh.vertices[triangle[2]];
            const double denominator = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
            if (std::abs(denominator) < 1e-12) {
                continue;
            }
            const int columnLow = std::max(0, part.columnAt(std::min({a.x, b.x, c.x})));
            const int columnHigh =
                std::min(part.columns() - 1, part.columnAt(std::max({a.x, b.x, c.x})));
            const int rowLow = std::max(0, part.rowAt(std::min({a.y, b.y, c.y})));
            const int rowHigh = std::min(part.rows() - 1, part.rowAt(std::max({a.y, b.y, c.y})));
            for (int row = rowLow; row <= rowHigh; ++row) {
                for (int column = columnLow; column <= columnHigh; ++column) {
                    const double px = part.x(column);
                    const double py = part.y(row);
                    const double u =
                        ((b.y - c.y) * (px - c.x) + (c.x - b.x) * (py - c.y)) / denominator;
                    const double v =
                        ((c.y - a.y) * (px - c.x) + (a.x - c.x) * (py - c.y)) / denominator;
                    const double w = 1.0 - u - v;
                    if (u < -1e-9 || v < -1e-9 || w < -1e-9) {
                        continue;
                    }
                    const auto z = static_cast<float>(u * a.z + v * b.z + w * c.z);
                    part.at(column, row) = std::max(part.at(column, row), z);
                }
            }
        }
    }

    void say(int kind, const Toolpath& path, std::string text) {
        if (said.emplace(std::make_pair(kind, path.operation.value), true).second) {
            report.problems.push_back({Problem::Severity::Error, path.operation, std::move(text)});
        }
    }

    // The tool along a straight piece of a move, or at its end if the piece goes straight up or
    // down: takes away what is above its surface, and sees what it hits. A cell meets the tool
    // where the tool comes nearest to it, and on a level piece that is where it is cut deepest.
    void stroke(const Toolpath& path, const Point& from, const Point& to, bool rapid) {
        const Tool& tool = path.tool;
        const double radius = tool.diameter / 2.0;
        const double acrossX = to.x - from.x;
        const double acrossY = to.y - from.y;
        const double lengthSquared = acrossX * acrossX + acrossY * acrossY;
        const int columnLow = std::max(0, stock.columnAt(std::min(from.x, to.x) - radius));
        const int columnHigh =
            std::min(stock.columns() - 1, stock.columnAt(std::max(from.x, to.x) + radius));
        const int rowLow = std::max(0, stock.rowAt(std::min(from.y, to.y) - radius));
        const int rowHigh =
            std::min(stock.rows() - 1, stock.rowAt(std::max(from.y, to.y) + radius));
        for (int row = rowLow; row <= rowHigh; ++row) {
            const double cellY = stock.y(row);
            for (int column = columnLow; column <= columnHigh; ++column) {
                const double cellX = stock.x(column);
                const double t =
                    lengthSquared > 0.0
                        ? std::clamp(((cellX - from.x) * acrossX + (cellY - from.y) * acrossY) /
                                         lengthSquared,
                                     0.0, 1.0)
                        : 1.0;
                const Point tip{from.x + acrossX * t, from.y + acrossY * t,
                                from.z + (to.z - from.z) * t};
                const double dx = cellX - tip.x;
                const double dy = cellY - tip.y;
                const double distanceSquared = dx * dx + dy * dy;
                if (distanceSquared > radius * radius) {
                    continue;
                }
                const double distance = std::sqrt(distanceSquared);
                float& top = stock.at(column, row);
                const double surface = tip.z + profile(tool, distance);
                if (static_cast<double>(top) > tip.z + tool.fluteLength) {
                    say(2, path,
                        path.name + ": the " + tool.name +
                            " is buried past its flutes, and its shank or holder hits the "
                            "material at " +
                            at(tip.x, tip.y) + ".");
                }
                if (static_cast<double>(top) <= surface) {
                    continue;
                }
                if (rapid) {
                    say(1, path,
                        path.name + ": a rapid move goes into the material at " + at(tip.x, tip.y) +
                            ".");
                }
                const auto floor = part.at(column, row);
                if (floor != noPart && surface < static_cast<double>(floor) - depthAllowance &&
                    distance <= radius - radialAllowance) {
                    say(3, path,
                        path.name + ": it cuts into the part at " + at(tip.x, tip.y) + ", " +
                            std::to_string(static_cast<int>(
                                std::ceil((static_cast<double>(floor) - surface) * 100.0))) +
                            " hundredths of a millimeter too deep.");
                }
                report.removedVolume += (static_cast<double>(top) - surface) * cellArea;
                top = static_cast<float>(surface);
            }
        }
    }

    void runOne() {
        const Step& step = steps[done];
        const Toolpath& path = program.paths[step.path];
        const Move& move = step.move;
        const Point from = position;
        const Point& to = move.to;
        const double length =
            std::sqrt((to.x - from.x) * (to.x - from.x) + (to.y - from.y) * (to.y - from.y) +
                      (to.z - from.z) * (to.z - from.z));
        const bool rapid = move.kind == MoveKind::Rapid;
        if (rapid) {
            report.rapidLength += length;
            report.rapidSeconds += length / machine.rapidRate * 60.0;
        } else {
            report.cuttingLength += length;
            report.cuttingSeconds += length / std::min(move.feed, machine.maxFeed) * 60.0;
        }
        // Level, a piece is exact at any length, and is kept short only for its box of cells to
        // stay close around it. On a slant the tool's height changes along the piece, so the
        // pieces are short enough for that to stay well under the depth allowance. Straight down,
        // there is one.
        const double reach = std::hypot(to.x - from.x, to.y - from.y);
        const double longest = std::max(path.tool.diameter / 2.0, stock.cell());
        const double drop = std::abs(to.z - from.z);
        const double piece =
            drop == 0.0 ? longest : std::clamp(0.01 * reach / drop, stock.cell() / 2.0, longest);
        const int pieces = std::max(1, static_cast<int>(std::ceil(reach / piece)));
        Point start = from;
        for (int i = 1; i <= pieces; ++i) {
            const double t = static_cast<double>(i) / pieces;
            const Point end{from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t,
                            from.z + (to.z - from.z) * t};
            stroke(path, start, end, rapid);
            start = end;
        }
        position = to;
        ++done;
    }

    void measureWhatIsLeft() {
        report.leftAbovePart = 0.0;
        report.volumeLeft = 0.0;
        for (int row = 0; row < stock.rows(); ++row) {
            for (int column = 0; column < stock.columns(); ++column) {
                const float floor = part.at(column, row);
                if (floor == noPart) {
                    continue;
                }
                const double above =
                    static_cast<double>(stock.at(column, row)) - static_cast<double>(floor);
                if (above > 0.0) {
                    report.leftAbovePart = std::max(report.leftAbovePart, above);
                    report.volumeLeft += above * cellArea;
                }
            }
        }
    }
};

Simulation::Simulation(const Program& program, const kernel::Mesh& part,
                       const document::Machine& machine, double cell)
    : m_impl(std::make_unique<Impl>(program, part, machine, cell)) {}

Simulation::~Simulation() = default;

std::size_t Simulation::moveCount() const {
    return m_impl->steps.size();
}

std::size_t Simulation::movesDone() const {
    return m_impl->done;
}

void Simulation::runTo(std::size_t moves) {
    while (m_impl->done < std::min(moves, m_impl->steps.size())) {
        m_impl->runOne();
    }
}

SimulationReport Simulation::run() {
    runTo(m_impl->steps.size());
    m_impl->measureWhatIsLeft();
    return m_impl->report;
}

Point Simulation::toolPosition() const {
    return m_impl->position;
}

const Tool* Simulation::tool() const {
    if (m_impl->steps.empty() || m_impl->done >= m_impl->steps.size()) {
        return nullptr;
    }
    return &m_impl->program.paths[m_impl->steps[m_impl->done].path].tool;
}

const HeightField& Simulation::stock() const {
    return m_impl->stock;
}

const HeightField& Simulation::part() const {
    return m_impl->part;
}

kernel::Mesh Simulation::stockMesh(int step) const {
    const HeightField& field = m_impl->stock;
    const int columns = field.columns() / step + 1;
    const int rows = field.rows() / step + 1;
    kernel::Mesh mesh;
    mesh.vertices.reserve(static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows));
    for (int j = 0; j < rows; ++j) {
        for (int i = 0; i < columns; ++i) {
            const int column = std::min(i * step, field.columns() - 1);
            const int row = std::min(j * step, field.rows() - 1);
            mesh.vertices.push_back(
                {field.x(column), field.y(row), static_cast<double>(field.at(column, row))});
        }
    }
    const auto corner = [columns](int i, int j) {
        return static_cast<std::uint32_t>(j * columns + i);
    };
    for (int j = 0; j + 1 < rows; ++j) {
        for (int i = 0; i + 1 < columns; ++i) {
            mesh.triangles.push_back({corner(i, j), corner(i + 1, j), corner(i + 1, j + 1)});
            mesh.triangles.push_back({corner(i, j), corner(i + 1, j + 1), corner(i, j + 1)});
        }
    }
    return mesh;
}

} // namespace lcad::cam
