// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/Machining.h>
#include <lcad/kernel/Measure.h>

#include <string>
#include <vector>

// What a toolpath is, once it has been worked out: straight moves of the tip of a tool, in the
// model's coordinates, in millimeters. Z is up, and the tool comes down along it.
namespace lcad::cam {

enum class MoveKind {
    /// As fast as the machine goes, cutting nothing.
    Rapid,
    /// Along the work, at the feed.
    Feed,
    /// Down into the material, at the plunge feed.
    Plunge,
};

/// A straight move to a point, from where the tool is.
struct Move {
    MoveKind kind = MoveKind::Rapid;
    kernel::Point to;
    /// Millimeters a minute. Not used for a rapid move.
    double feed = 0.0;
};

/// What one operation has the machine do.
struct Toolpath {
    document::ObjectId operation;
    std::string name;
    document::Tool tool;
    /// Turns a minute.
    double spindle = 0.0;
    std::vector<Move> moves;
};

/// Something about a program that the user has to know.
struct Problem {
    enum class Severity {
        /// It can be run, and this is worth knowing.
        Warning,
        /// It must not be run. G-code is not written while there is one.
        Error,
    };
    Severity severity = Severity::Error;
    /// The operation it is about. Not valid for what is about the whole setup.
    document::ObjectId operation;
    std::string text;
};

/// The block the part is cut from and where the machine's zero is, in the model's coordinates.
struct WorkArea {
    kernel::BoundingBox part;
    kernel::BoundingBox stock;
    /// Where X, Y, and Z are zero on the machine.
    kernel::Point origin;
};

/// All a setup comes to.
struct Program {
    WorkArea work;
    std::vector<Toolpath> paths;
    std::vector<Problem> problems;
    /// Where the tool starts from and ends up: over the stock's top, clear of everything.
    double safeZ = 0.0;

    [[nodiscard]] bool hasErrors() const {
        for (const Problem& problem : problems) {
            if (problem.severity == Problem::Severity::Error) {
                return true;
            }
        }
        return false;
    }
};

} // namespace lcad::cam
