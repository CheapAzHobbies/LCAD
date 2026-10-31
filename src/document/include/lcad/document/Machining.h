// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/ObjectId.h>

#include <string>
#include <vector>

// What making a part on a milling machine takes: the tools, the block it is cut from, the machine,
// and the operations. Toolpaths are worked out from these and the model each time they are
// wanted (see the cam module), so that they follow the model. The Document holds them; see
// Document.h.
namespace lcad::document {

enum class ToolKind {
    /// A square end.
    FlatEndMill,
    /// A round end.
    BallEndMill,
    /// A pointed end. Cuts straight down only.
    Drill,
};

[[nodiscard]] const char* toString(ToolKind kind);
[[nodiscard]] bool toolKindFromString(const std::string& name, ToolKind& kind);

/// A cutter.
struct Tool {
    ObjectId id;
    std::string name;
    ToolKind kind = ToolKind::FlatEndMill;
    /// Of what cuts, in millimeters.
    double diameter = 6.0;
    /// How deep it can cut, in millimeters.
    double fluteLength = 20.0;
    /// How far it sticks out of the holder, in millimeters.
    double stickout = 30.0;
    int flutes = 2;
    /// For a drill: the full angle of its point, in degrees.
    double tipAngle = 118.0;
    /// Its number in the machine's tool changer, or for the operator.
    int number = 1;

    friend bool operator==(const Tool&, const Tool&) = default;
};

/// What the G-code is written for.
enum class PostDialect { Grbl, LinuxCnc };

[[nodiscard]] const char* toString(PostDialect dialect);
[[nodiscard]] bool postDialectFromString(const std::string& name, PostDialect& dialect);

/// The machine an operation runs on.
struct Machine {
    std::string name = "Generic 3-axis mill";
    PostDialect dialect = PostDialect::Grbl;
    /// How far each axis can travel, in millimeters.
    double travelX = 300.0;
    double travelY = 300.0;
    double travelZ = 100.0;
    /// Rapid speed, and the fastest feed it allows, in millimeters a minute.
    double rapidRate = 3000.0;
    double maxFeed = 2000.0;
    /// The fastest its spindle turns, in turns a minute.
    double maxSpindle = 24000.0;

    friend bool operator==(const Machine&, const Machine&) = default;
};

/// The block a part is cut from: as much around the box of the part.
struct Stock {
    double side = 3.0;
    double top = 1.0;
    double bottom = 0.0;

    friend bool operator==(const Stock&, const Stock&) = default;
};

/// Where X, Y, and Z are zero on the machine.
enum class WorkOrigin {
    /// The top of the stock, at its corner of least X and least Y.
    StockTopCorner,
    /// The middle of the top of the stock.
    StockTopCenter,
};

[[nodiscard]] const char* toString(WorkOrigin origin);
[[nodiscard]] bool workOriginFromString(const std::string& name, WorkOrigin& origin);

enum class OperationKind {
    /// Levels the top of the stock down to the top of the part.
    Face,
    /// Clears a flat area, around its islands, down to it.
    Pocket,
    /// Cuts around an outline.
    Contour,
    /// Drills round holes.
    Drill,
};

[[nodiscard]] const char* toString(OperationKind kind);
[[nodiscard]] bool operationKindFromString(const std::string& name, OperationKind& kind);

/// A face of the model an operation works on, by what stays the same when the model changes.
struct OperationTarget {
    ObjectId body;
    std::vector<ObjectId> path;
    std::string face;

    friend bool operator==(const OperationTarget&, const OperationTarget&) = default;
};

/// Which side of an outline a contour runs on.
enum class ContourSide { Outside, Inside };

/// One thing the machine does with one tool.
struct MachiningOperation {
    ObjectId id;
    std::string name;
    OperationKind kind = OperationKind::Pocket;
    ObjectId tool;
    /// What it works on: a pocket's floor, or a face to cut around or through. Cylindrical faces
    /// for holes. None for facing.
    std::vector<OperationTarget> targets;
    /// Millimeters a minute along the cut, and going down into the material.
    double feed = 600.0;
    double plungeFeed = 200.0;
    /// Turns a minute.
    double spindle = 12000.0;
    /// How far apart the passes lie, in millimeters.
    double stepover = 2.4;
    /// How deep one pass goes, in millimeters.
    double stepdown = 2.0;
    /// How far above the stock's top rapid moves travel, in millimeters.
    double clearance = 5.0;
    /// How far above the stock's top a cut starts from at feed, in millimeters.
    double retract = 2.0;
    /// How much to leave on walls and floors for a later cut, in millimeters.
    double stockToLeave = 0.0;
    ContourSide side = ContourSide::Outside;
    /// How deep each peck of a drill goes, in millimeters. Zero drills in one go.
    double peck = 0.0;
    /// Pockets and contours: how steeply the tool goes down into the material along its path, in
    /// degrees from level. Zero goes straight down.
    double rampAngle = 3.0;
    /// Contours: how many bridges of material to leave, which hold the part in place when the cut
    /// goes right through. How wide they are along the cut, and how high, in millimeters.
    int tabs = 0;
    double tabWidth = 6.0;
    double tabHeight = 1.5;
    bool suppressed = false;

    friend bool operator==(const MachiningOperation&, const MachiningOperation&) = default;
};

/// One way of clamping the part: a machine, the stock, where zero is, and what is done there.
/// The tool comes down along the model's Z.
struct Setup {
    ObjectId id;
    std::string name;
    Machine machine;
    Stock stock;
    WorkOrigin origin = WorkOrigin::StockTopCorner;
    std::vector<MachiningOperation> operations;

    friend bool operator==(const Setup&, const Setup&) = default;
};

} // namespace lcad::document
