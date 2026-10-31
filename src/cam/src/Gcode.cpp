// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/cam/Gcode.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <optional>
#include <set>
#include <sstream>

namespace lcad::cam {

using document::PostDialect;
using kernel::Point;

namespace {

// Three decimals: a micrometer, finer than the machines LCAD writes for can move.
constexpr double tolerance = 0.0011;

// Grbl reads a line into a buffer of 80 characters, and refuses a longer one.
constexpr std::size_t longestLine = 78;

std::string number(double value) {
    char text[64];
    std::snprintf(text, sizeof(text), "%.3f", value);
    std::string out(text);
    if (out.find('.') != std::string::npos) {
        out.erase(out.find_last_not_of('0') + 1);
        if (out.back() == '.') {
            out.pop_back();
        }
    }
    return out == "-0" ? "0" : out;
}

// Text fit for a comment: no parentheses, which end it, and nothing outside plain ASCII.
std::string commentText(const std::string& text) {
    std::string out;
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 32 || u > 126 || c == '(' || c == ')' || c == ';') {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out.substr(0, 60);
}

// A toolpath in the machine's coordinates, with the moves that go around it.
struct MachinePath {
    const Toolpath* path = nullptr;
    std::vector<Move> moves;
};

std::string toolLine(const document::Tool& tool) {
    return "T" + std::to_string(tool.number) + " " + tool.name + ", " + number(tool.diameter) +
           " mm";
}

double distance(const Point& a, const Point& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
                     (a.z - b.z) * (a.z - b.z));
}

std::vector<MachinePath> machinePaths(const Program& program) {
    const Point& zero = program.work.origin;
    const auto toMachine = [&zero](const Point& p) {
        return Point{p.x - zero.x, p.y - zero.y, p.z - zero.z};
    };
    const double safe = program.safeZ - zero.z;
    std::vector<MachinePath> paths;
    for (const Toolpath& path : program.paths) {
        if (path.moves.empty()) {
            continue;
        }
        MachinePath out;
        out.path = &path;
        const auto add = [&out](const Move& move) {
            if (out.moves.empty() || distance(out.moves.back().to, move.to) >= 1e-9) {
                out.moves.push_back(move);
            }
        };
        // Over the start at the safe height, nothing sideways near the work, and then down.
        const Point first = toMachine(path.moves.front().to);
        add({MoveKind::Rapid, {first.x, first.y, safe}, 0.0});
        for (const Move& move : path.moves) {
            add({move.kind, toMachine(move.to), move.feed});
        }
        add({MoveKind::Rapid, {out.moves.back().to.x, out.moves.back().to.y, safe}, 0.0});
        paths.push_back(std::move(out));
    }
    return paths;
}

} // namespace

std::vector<Move> machineMoves(const Program& program) {
    std::vector<Move> moves;
    for (const MachinePath& path : machinePaths(program)) {
        moves.insert(moves.end(), path.moves.begin(), path.moves.end());
    }
    return moves;
}

std::string writeGcode(const Program& program, const document::Machine& machine,
                       const std::string& name) {
    const bool linuxCnc = machine.dialect == PostDialect::LinuxCnc;
    std::ostringstream out;
    const auto comment = [&out](const std::string& text) {
        out << '(' << commentText(text) << ")\n";
    };
    comment("LCAD: " + name);
    comment("For " + machine.name + (linuxCnc ? ", LinuxCNC" : ", Grbl"));
    comment("Zero is at the top of the stock. Run it in the air first.");
    if (linuxCnc) {
        out << "G21 G90 G17 G94 G40 G49 G80\n";
        out << "G54\n";
        out << "G64 P0.01\n";
    } else {
        out << "G21 G90 G17 G94\n";
        out << "G54\n";
    }
    // Up first, whatever the machine was doing.
    const double safe = program.safeZ - program.work.origin.z;
    out << "G0 Z" << number(safe) << '\n';

    bool placed = false;
    Point at{0.0, 0.0, safe};
    double feed = 0.0;
    const auto moveTo = [&](const Move& move) {
        const int code = move.kind == MoveKind::Rapid ? 0 : 1;
        std::string line = "G" + std::to_string(code);
        if (!placed || std::abs(move.to.x - at.x) >= 1e-9) {
            line += " X" + number(move.to.x);
        }
        if (!placed || std::abs(move.to.y - at.y) >= 1e-9) {
            line += " Y" + number(move.to.y);
        }
        if (std::abs(move.to.z - at.z) >= 1e-9) {
            line += " Z" + number(move.to.z);
        }
        if (code == 1 && std::abs(move.feed - feed) >= 1e-9) {
            line += " F" + number(move.feed);
            feed = move.feed;
        }
        out << line << '\n';
        at = move.to;
        placed = true;
    };

    const document::Tool* loaded = nullptr;
    for (const MachinePath& path : machinePaths(program)) {
        const Toolpath& source = *path.path;
        comment(source.name);
        if (loaded == nullptr || loaded->id != source.tool.id) {
            comment(toolLine(source.tool));
            if (linuxCnc) {
                if (loaded != nullptr) {
                    out << "M5\n";
                }
                out << "T" << source.tool.number << " M6\n";
                out << "G43 H" << source.tool.number << '\n';
            } else if (loaded != nullptr) {
                // Grbl has no tool changer: it stops, and the operator changes the tool.
                out << "M5\n";
                comment("Change to " + toolLine(source.tool) + ", set Z zero, and resume");
                out << "M0\n";
            }
            loaded = &source.tool;
        }
        out << "S" << number(source.spindle) << " M3\n";
        // Up to speed before anything moves toward the work.
        out << "G4 P2\n";
        for (const Move& move : path.moves) {
            moveTo(move);
        }
    }
    out << "M5\n";
    out << (linuxCnc ? "M2\n" : "M30\n");
    return out.str();
}

GcodeReading readGcode(const std::string& text, PostDialect dialect) {
    GcodeReading reading;
    const bool grbl = dialect == PostDialect::Grbl;
    const std::set<double> grblG{0,  1,  2,  3,  4,  10, 17, 18, 19, 20, 21, 28, 30, 40,
                                 53, 54, 55, 56, 57, 58, 59, 61, 80, 90, 91, 92, 93, 94};
    const std::set<double> linuxG{0,  1,  2,  3,  4,  17, 18, 19, 20, 21, 28, 30, 40, 43, 49,
                                  53, 54, 55, 56, 57, 58, 59, 61, 64, 80, 90, 91, 92, 93, 94};
    const std::set<double> grblM{0, 1, 2, 3, 4, 5, 7, 8, 9, 30};
    const std::set<double> linuxM{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 30};

    bool metric = false;
    bool absolute = false;
    bool perMinute = false;
    bool spindleOn = false;
    double feed = 0.0;
    double speed = 0.0;
    int motion = -1;
    Point at;
    bool hasX = false;
    bool hasY = false;
    bool hasZ = false;

    std::istringstream lines(text);
    std::string raw;
    int lineNumber = 0;
    const auto fail = [&](const std::string& why) {
        reading.errors.push_back("Line " + std::to_string(lineNumber) + ": " + why);
    };
    while (std::getline(lines, raw)) {
        ++lineNumber;
        // Comments, which neither dialect counts toward the length of a line.
        std::string code;
        bool inComment = false;
        for (const char c : raw) {
            if (inComment) {
                if (c == ')') {
                    inComment = false;
                }
                continue;
            }
            if (c == '(') {
                inComment = true;
            } else if (c == ';') {
                break;
            } else if (c != '\r') {
                code += c;
            }
        }
        if (inComment) {
            fail("a comment is not closed.");
        }
        if (code.size() > longestLine) {
            fail("the line is too long for the machine.");
        }

        struct Word {
            char letter;
            double value;
        };
        std::vector<Word> words;
        std::size_t i = 0;
        bool broken = false;
        while (i < code.size()) {
            if (std::isspace(static_cast<unsigned char>(code[i])) != 0) {
                ++i;
                continue;
            }
            const char letter =
                static_cast<char>(std::toupper(static_cast<unsigned char>(code[i])));
            ++i;
            if (std::isalpha(static_cast<unsigned char>(letter)) == 0) {
                fail(std::string("\"") + letter + "\" is not a word.");
                broken = true;
                break;
            }
            std::size_t end = i;
            while (end < code.size() && (std::isdigit(static_cast<unsigned char>(code[end])) != 0 ||
                                         code[end] == '.' || code[end] == '-')) {
                ++end;
            }
            double value = 0.0;
            const auto [next, status] = std::from_chars(code.data() + i, code.data() + end, value);
            if (end == i || status != std::errc() || next != code.data() + end) {
                fail(std::string("the word ") + letter + " has no number.");
                broken = true;
                break;
            }
            words.push_back({letter, value});
            i = end;
        }
        if (broken) {
            continue;
        }

        std::optional<double> x;
        std::optional<double> y;
        std::optional<double> z;
        std::optional<double> f;
        bool moves = false;
        int thisMotion = motion;
        for (const Word& word : words) {
            switch (word.letter) {
            case 'G': {
                const std::set<double>& known = grbl ? grblG : linuxG;
                if (known.count(word.value) == 0) {
                    fail("this machine has no G" + number(word.value) + ".");
                    break;
                }
                const auto code2 = static_cast<int>(word.value);
                if (code2 == 0 || code2 == 1) {
                    thisMotion = code2;
                    moves = true;
                } else if (code2 == 2 || code2 == 3) {
                    fail("LCAD's posts write no arcs.");
                } else if (code2 == 20) {
                    metric = false;
                } else if (code2 == 21) {
                    metric = true;
                } else if (code2 == 90) {
                    absolute = true;
                } else if (code2 == 91) {
                    absolute = false;
                } else if (code2 == 94) {
                    perMinute = true;
                } else if (code2 == 93) {
                    perMinute = false;
                }
                break;
            }
            case 'M': {
                const std::set<double>& known = grbl ? grblM : linuxM;
                if (known.count(word.value) == 0) {
                    fail("this machine has no M" + number(word.value) + ".");
                    break;
                }
                const auto code2 = static_cast<int>(word.value);
                if (code2 == 3 || code2 == 4) {
                    if (speed <= 0.0) {
                        fail("the spindle is started without a speed.");
                    }
                    spindleOn = true;
                } else if (code2 == 5) {
                    spindleOn = false;
                }
                break;
            }
            case 'X':
                x = word.value;
                break;
            case 'Y':
                y = word.value;
                break;
            case 'Z':
                z = word.value;
                break;
            case 'F':
                f = word.value;
                break;
            case 'S':
                speed = word.value;
                reading.spindle = std::max(reading.spindle, speed);
                break;
            case 'P':
                if (std::none_of(words.begin(), words.end(), [](const Word& other) {
                        return other.letter == 'G' && (other.value == 64.0 || other.value == 4.0);
                    })) {
                    fail("P with nothing it belongs to.");
                }
                break;
            case 'T':
            case 'H':
                if (grbl) {
                    fail(std::string("this machine has no ") + word.letter + " word.");
                }
                break;
            default:
                fail(std::string("this machine has no ") + word.letter + " word.");
                break;
            }
        }
        if (f.has_value()) {
            feed = *f;
            reading.feed = std::max(reading.feed, feed);
        }
        if (!x.has_value() && !y.has_value() && !z.has_value()) {
            if (moves) {
                motion = thisMotion;
            }
            continue;
        }
        motion = thisMotion;
        if (motion < 0) {
            fail("coordinates with no move to say what they are for.");
            continue;
        }
        if (!metric) {
            fail("a move before G21: the machine would take it for inches.");
        }
        if (!absolute) {
            fail("a move before G90: the machine would take it as relative.");
        }
        if (motion == 1) {
            if (!perMinute) {
                fail("a feed before G94.");
            }
            if (feed <= 0.0) {
                fail("a cut with no feed.");
            }
            if (!spindleOn) {
                fail("a cut with the spindle standing still.");
            }
        }
        if (x.has_value()) {
            at.x = *x;
            hasX = true;
        }
        if (y.has_value()) {
            at.y = *y;
            hasY = true;
        }
        if (z.has_value()) {
            at.z = *z;
            hasZ = true;
        }
        // The machine starts wherever it was left, so nothing moves sideways until it is up.
        if ((x.has_value() || y.has_value()) && !hasZ) {
            fail("a move sideways before the machine is told its height.");
        }
        if (!(hasX && hasY && hasZ)) {
            if (motion == 1) {
                fail("a cut before the machine is told where it is on every axis.");
            }
            continue;
        }
        Move move;
        move.kind = motion == 0 ? MoveKind::Rapid : MoveKind::Feed;
        move.to = at;
        move.feed = motion == 0 ? 0.0 : feed;
        reading.moves.push_back(move);
    }
    return reading;
}

std::vector<std::string> verifyGcode(const std::string& text, const Program& program,
                                     PostDialect dialect) {
    GcodeReading reading = readGcode(text, dialect);
    std::vector<std::string> errors = std::move(reading.errors);
    const std::vector<Move> wanted = machineMoves(program);
    if (reading.moves.size() != wanted.size()) {
        errors.push_back("It has " + std::to_string(reading.moves.size()) + " moves, and the " +
                         "program has " + std::to_string(wanted.size()) + ".");
        return errors;
    }
    for (std::size_t i = 0; i < wanted.size(); ++i) {
        const Move& mine = wanted[i];
        const Move& read = reading.moves[i];
        const bool rapid = mine.kind == MoveKind::Rapid;
        if ((read.kind == MoveKind::Rapid) != rapid) {
            errors.push_back("Move " + std::to_string(i + 1) + " is of the wrong kind.");
        }
        if (distance(read.to, mine.to) > tolerance) {
            errors.push_back("Move " + std::to_string(i + 1) + " ends in the wrong place.");
        }
        if (!rapid && std::abs(read.feed - mine.feed) > tolerance) {
            errors.push_back("Move " + std::to_string(i + 1) + " has the wrong feed.");
        }
    }
    return errors;
}

} // namespace lcad::cam
