// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/cam/Toolpath.h>
#include <lcad/document/Machining.h>

#include <string>
#include <vector>

// G-code, what a machine is told. Each dialect has a post: what writes it. What it writes is then
// read back by a strict reader of that dialect, which knows nothing about how it was written,
// and has to come to the same moves. A program that does not is not handed out.
namespace lcad::cam {

/// The moves a program comes to in the machine's coordinates: from the machine's zero, with the
/// moves to and from the safe height that go around each toolpath. Writers say first how high to
/// go, on Z alone, which is not among these: the machine starts wherever it was left.
[[nodiscard]] std::vector<Move> machineMoves(const Program& program);

/// A program as the text of a G-code file, for a machine.
[[nodiscard]] std::string writeGcode(const Program& program, const document::Machine& machine,
                                     const std::string& name);

/// What a G-code text says.
struct GcodeReading {
    std::vector<Move> moves;
    /// What is wrong with it, each with its line number: a word the dialect does not have, a
    /// move before the units are set, a cut with the spindle standing still.
    std::vector<std::string> errors;
    /// The highest spindle speed it asks for, and the highest feed.
    double spindle = 0.0;
    double feed = 0.0;
};

/// Reads a G-code text as a machine of a dialect would.
[[nodiscard]] GcodeReading readGcode(const std::string& text, document::PostDialect dialect);

/// What is wrong with a G-code text as the program it was written from. Empty if it reads back as
/// the same moves, in the same order, to within what its numbers can say.
[[nodiscard]] std::vector<std::string> verifyGcode(const std::string& text, const Program& program,
                                                   document::PostDialect dialect);

} // namespace lcad::cam
