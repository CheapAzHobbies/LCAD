// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/cam/Toolpath.h>
#include <lcad/document/Machining.h>
#include <lcad/kernel/Exchange.h>

#include <cmath>
#include <memory>
#include <vector>

// Running a program on a block of material that exists only in memory. The block is a grid of
// heights: enough for a tool that comes down along Z, which cannot reach under anything. What
// it finds is what would go wrong on the machine, in time to do something about it.
namespace lcad::cam {

/// A grid of heights over a rectangle. A height is that of the top of the material over the
/// middle of a cell.
class HeightField {
public:
    HeightField() = default;
    HeightField(double left, double bottom, double width, double depth, double cell, float height);

    [[nodiscard]] int columns() const { return m_columns; }
    [[nodiscard]] int rows() const { return m_rows; }
    [[nodiscard]] double cell() const { return m_cell; }
    [[nodiscard]] double x(int column) const { return m_left + (column + 0.5) * m_cell; }
    [[nodiscard]] double y(int row) const { return m_bottom + (row + 0.5) * m_cell; }
    [[nodiscard]] int columnAt(double x) const {
        return static_cast<int>(std::floor((x - m_left) / m_cell));
    }
    [[nodiscard]] int rowAt(double y) const {
        return static_cast<int>(std::floor((y - m_bottom) / m_cell));
    }
    [[nodiscard]] float& at(int column, int row) {
        return m_heights[static_cast<std::size_t>(row) * static_cast<std::size_t>(m_columns) +
                         static_cast<std::size_t>(column)];
    }
    [[nodiscard]] float at(int column, int row) const {
        return m_heights[static_cast<std::size_t>(row) * static_cast<std::size_t>(m_columns) +
                         static_cast<std::size_t>(column)];
    }
    /// The height at a place, or `outside` off the grid.
    [[nodiscard]] float sample(double px, double py, float outside) const;

private:
    double m_left = 0.0;
    double m_bottom = 0.0;
    double m_cell = 1.0;
    int m_columns = 0;
    int m_rows = 0;
    std::vector<float> m_heights;
};

/// What came of a run.
struct SimulationReport {
    /// What the tools took away, in cubic millimeters.
    double removedVolume = 0.0;
    /// How long the machine takes.
    double cuttingSeconds = 0.0;
    double rapidSeconds = 0.0;
    /// How far the tools went.
    double cuttingLength = 0.0;
    double rapidLength = 0.0;
    /// How much stands above the part when it is done, in millimeters at the most, and in
    /// cubic millimeters.
    double leftAbovePart = 0.0;
    double volumeLeft = 0.0;
    /// What would go wrong: an Error for each kind of thing and operation, with where.
    std::vector<Problem> problems;

    [[nodiscard]] double seconds() const { return cuttingSeconds + rapidSeconds; }
    /// It was stopped before it got to the end, which is as good as an error: what came after
    /// has not been checked.
    bool stopped = false;

    [[nodiscard]] bool hasErrors() const {
        if (stopped) {
            return true;
        }
        for (const Problem& problem : problems) {
            if (problem.severity == Problem::Severity::Error) {
                return true;
            }
        }
        return false;
    }
};

/// A program being run on the stock of its setup.
class Simulation {
public:
    /// `part` is what is to be left: the tool must not cut into it. `cell` is the size of a
    /// cell of the grid in millimeters; zero picks one for the size of the stock.
    Simulation(const Program& program, const kernel::Mesh& part, const document::Machine& machine,
               double cell = 0.0);
    ~Simulation();

    /// How many moves the program has, from the first to the last.
    [[nodiscard]] std::size_t moveCount() const;
    /// Runs on to the end of the move with this number, from where it stands. Moves that have
    /// been run are not run again.
    void runTo(std::size_t moves);
    /// Runs to the end of the program, and says how it went.
    SimulationReport run();
    [[nodiscard]] std::size_t movesDone() const;

    /// The tip of the tool now.
    [[nodiscard]] kernel::Point toolPosition() const;
    /// The tool in use now, or nullptr before the first and after the last.
    [[nodiscard]] const document::Tool* tool() const;
    /// The stock as it is now.
    [[nodiscard]] const HeightField& stock() const;
    /// The part's own heights: lowest where there is no part.
    [[nodiscard]] const HeightField& part() const;
    /// The stock as it is now as a closed surface, a cell of every `step` being a corner of it.
    [[nodiscard]] kernel::Mesh stockMesh(int step = 1) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace lcad::cam
