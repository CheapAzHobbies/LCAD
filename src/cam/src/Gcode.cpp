// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/cam/Gcode.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
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

constexpr double pi = 3.14159265358979323846;

// How far moves may lie from an arc written for them, and how far they may be found from it.
constexpr double arcFit = 0.003;
constexpr double arcFound = 0.005;
// How much further from its center an arc may end than it starts. Grbl and LinuxCNC both refuse an
// arc whose end is not on its circle, and allow about 0.005 mm: this is to be well within that.
constexpr double arcRadiusError = 0.002;
// A run has to be worth an arc. Short of half a turn, so that which way around it goes is plain
// and rounding can never turn it into a whole circle.
constexpr std::size_t fewestArcMoves = 3;
constexpr double mostArcSweep = 170.0 * pi / 180.0;
constexpr double smallestArcRadius = 0.05;
constexpr double largestArcRadius = 1000.0;

// Three decimals, with a point whatever the language of the system: printf would write a comma
// on a German one, which a machine reads as something else.
std::string number(double value, int decimals = 3) {
    char text[64];
    const auto end =
        std::to_chars(text, text + sizeof(text), value, std::chars_format::fixed, decimals).ptr;
    std::string out(text, end);
    if (out.find('.') != std::string::npos) {
        out.erase(out.find_last_not_of('0') + 1);
        if (out.back() == '.') {
            out.pop_back();
        }
    }
    return out == "-0" ? "0" : out;
}

// A number as a machine reads it: a sign, digits, and a point, and nothing else.
std::optional<double> parsedNumber(const std::string& text) {
    std::size_t i = 0;
    const bool negative = !text.empty() && text[0] == '-';
    if (negative) {
        ++i;
    }
    double whole = 0.0;
    double scale = 1.0;
    bool point = false;
    bool digits = false;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '.' && !point) {
            point = true;
        } else if (c >= '0' && c <= '9') {
            digits = true;
            whole = whole * 10.0 + (c - '0');
            if (point) {
                scale *= 10.0;
            }
        } else {
            return std::nullopt;
        }
    }
    if (!digits) {
        return std::nullopt;
    }
    return (negative ? -whole : whole) / scale;
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

// A run of moves that lies on a circle.
struct Arc {
    // The move after the last it takes the place of.
    std::size_t next = 0;
    double centerX = 0.0;
    double centerY = 0.0;
    bool clockwise = false;
};

// The arc through three places of `points`, if the whole of the run lies on it.
std::optional<Arc> arcAlong(const std::vector<Point>& points) {
    const Point& a = points.front();
    const Point& b = points[points.size() / 2];
    const Point& c = points.back();
    const double twice = 2.0 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
    if (std::abs(twice) < 1e-9) {
        return std::nullopt;
    }
    const double aa = a.x * a.x + a.y * a.y;
    const double bb = b.x * b.x + b.y * b.y;
    const double cc = c.x * c.x + c.y * c.y;
    Arc arc;
    arc.centerX = (aa * (b.y - c.y) + bb * (c.y - a.y) + cc * (a.y - b.y)) / twice;
    arc.centerY = (aa * (c.x - b.x) + bb * (a.x - c.x) + cc * (b.x - a.x)) / twice;
    arc.clockwise = twice < 0.0;
    const double radius = std::hypot(a.x - arc.centerX, a.y - arc.centerY);
    if (radius < smallestArcRadius || radius > largestArcRadius) {
        return std::nullopt;
    }
    const auto off = [&arc, radius](double x, double y) {
        return std::abs(std::hypot(x - arc.centerX, y - arc.centerY) - radius);
    };
    double swept = 0.0;
    for (std::size_t i = 1; i < points.size(); ++i) {
        const Point& from = points[i - 1];
        const Point& to = points[i];
        if (off(to.x, to.y) > arcFit ||
            off((from.x + to.x) / 2.0, (from.y + to.y) / 2.0) > arcFit) {
            return std::nullopt;
        }
        const double start = std::atan2(from.y - arc.centerY, from.x - arc.centerX);
        const double end = std::atan2(to.y - arc.centerY, to.x - arc.centerX);
        double step = arc.clockwise ? start - end : end - start;
        step = std::remainder(step, 2.0 * pi);
        if (step <= 0.0) {
            return std::nullopt;
        }
        swept += step;
    }
    if (swept > mostArcSweep) {
        return std::nullopt;
    }
    return arc;
}

// The longest arc that can take the place of the moves from `first`, which start from the place
// the move before ends. Level cuts at one feed, and nothing else.
std::optional<Arc> arcFrom(const std::vector<Move>& moves, std::size_t first) {
    if (first == 0) {
        return std::nullopt;
    }
    const Point& start = moves[first - 1].to;
    std::vector<Point> points{start};
    std::optional<Arc> best;
    for (std::size_t i = first; i < moves.size(); ++i) {
        const Move& move = moves[i];
        if (move.kind != MoveKind::Feed || std::abs(move.to.z - start.z) >= 1e-9 ||
            std::abs(move.feed - moves[first].feed) >= 1e-9) {
            break;
        }
        points.push_back(move.to);
        if (points.size() <= fewestArcMoves) {
            continue;
        }
        std::optional<Arc> arc = arcAlong(points);
        if (!arc.has_value()) {
            break;
        }
        arc->next = i + 1;
        best = arc;
    }
    return best;
}

// The way around from one direction to another, by the way an arc goes.
double turnedBy(double from, double to, bool clockwise) {
    const double turn = clockwise ? from - to : to - from;
    return turn - 2.0 * pi * std::floor(turn / (2.0 * pi));
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

    // The machine goes from where it was told, so the center is measured from there, not from
    // where it should be. In a finer step than places are, for the end to stay on the circle.
    const auto arcTo = [&](const Arc& arc, const Move& last) {
        const auto told = [](double value) { return std::round(value * 1000.0) / 1000.0; };
        std::string line = arc.clockwise ? "G2" : "G3";
        line += " X" + number(last.to.x) + " Y" + number(last.to.y);
        line += " I" + number(arc.centerX - told(at.x), 4);
        line += " J" + number(arc.centerY - told(at.y), 4);
        if (std::abs(last.feed - feed) >= 1e-9) {
            line += " F" + number(last.feed);
            feed = last.feed;
        }
        out << line << '\n';
        at = last.to;
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
        for (std::size_t i = 0; i < path.moves.size();) {
            const std::optional<Arc> arc =
                machine.arcs ? arcFrom(path.moves, i) : std::optional<Arc>();
            if (arc.has_value()) {
                arcTo(*arc, path.moves[arc->next - 1]);
                i = arc->next;
            } else {
                moveTo(path.moves[i]);
                ++i;
            }
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
    bool levelPlane = false;
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
            const std::optional<double> value = parsedNumber(code.substr(i, end - i));
            if (!value.has_value()) {
                fail(std::string("the word ") + letter + " has no number.");
                broken = true;
                break;
            }
            words.push_back({letter, *value});
            i = end;
        }
        if (broken) {
            continue;
        }

        std::optional<double> x;
        std::optional<double> y;
        std::optional<double> z;
        std::optional<double> f;
        std::optional<double> centerI;
        std::optional<double> centerJ;
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
                if (code2 >= 0 && code2 <= 3) {
                    thisMotion = code2;
                    moves = true;
                } else if (code2 == 17) {
                    levelPlane = true;
                } else if (code2 == 18 || code2 == 19) {
                    levelPlane = false;
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
            case 'I':
                centerI = word.value;
                break;
            case 'J':
                centerJ = word.value;
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
        const bool arc = motion == 2 || motion == 3;
        if ((centerI.has_value() || centerJ.has_value()) && !arc) {
            fail("I or J on a move that is not an arc.");
        }
        if (motion != 0) {
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
        const Point from = at;
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
            if (motion != 0) {
                fail("a cut before the machine is told where it is on every axis.");
            }
            continue;
        }
        if (arc) {
            if (!levelPlane) {
                fail("an arc before G17: the machine would not know which plane it lies in.");
            }
            const double i2 = centerI.value_or(0.0);
            const double j2 = centerJ.value_or(0.0);
            const double radius = std::hypot(i2, j2);
            if (radius < 1e-6) {
                fail("an arc with no center.");
            } else if (std::abs(std::hypot(at.x - from.x - i2, at.y - from.y - j2) - radius) >
                       arcRadiusError) {
                fail("an arc that does not end on its circle, which the machine would refuse.");
            }
            reading.arcs.push_back({reading.moves.size(), from.x + i2, from.y + j2, motion == 2});
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
    std::size_t matched = 0;
    std::size_t nextArc = 0;
    const auto tooMany = [&](std::size_t read) {
        errors.push_back("It has moves left at move " + std::to_string(read + 1) +
                         ", and the program has " + std::to_string(wanted.size()) + ".");
    };
    for (std::size_t i = 0; i < reading.moves.size(); ++i) {
        const Move& read = reading.moves[i];
        const std::string name = "Move " + std::to_string(i + 1);
        if (matched >= wanted.size()) {
            tooMany(i);
            return errors;
        }
        if (nextArc >= reading.arcs.size() || reading.arcs[nextArc].move != i) {
            const Move& mine = wanted[matched];
            const bool rapid = mine.kind == MoveKind::Rapid;
            if ((read.kind == MoveKind::Rapid) != rapid) {
                errors.push_back(name + " is of the wrong kind.");
            }
            if (distance(read.to, mine.to) > tolerance) {
                errors.push_back(name + " ends in the wrong place.");
            }
            if (!rapid && std::abs(read.feed - mine.feed) > tolerance) {
                errors.push_back(name + " has the wrong feed.");
            }
            ++matched;
            continue;
        }

        // An arc: the moves it stands for, in turn, along it and the way it goes, up to its end.
        const GcodeArc& arc = reading.arcs[nextArc];
        ++nextArc;
        if (i == 0) {
            errors.push_back(name + " is an arc from nowhere.");
            continue;
        }
        Point at = reading.moves[i - 1].to;
        const double radius = std::hypot(at.x - arc.centerX, at.y - arc.centerY);
        const auto direction = [&arc](const Point& p) {
            return std::atan2(p.y - arc.centerY, p.x - arc.centerX);
        };
        const double whole = turnedBy(direction(at), direction(read.to), arc.clockwise);
        double turned = 0.0;
        bool ended = false;
        while (!ended && matched < wanted.size()) {
            const Move& mine = wanted[matched];
            const double away =
                std::abs(std::hypot(mine.to.x - arc.centerX, mine.to.y - arc.centerY) - radius);
            const double middle = std::abs(std::hypot((at.x + mine.to.x) / 2.0 - arc.centerX,
                                                      (at.y + mine.to.y) / 2.0 - arc.centerY) -
                                           radius);
            const double step = turnedBy(direction(at), direction(mine.to), arc.clockwise);
            if (mine.kind != MoveKind::Feed || std::abs(mine.to.z - read.to.z) > tolerance ||
                std::abs(mine.feed - read.feed) > tolerance || away > arcFound ||
                middle > arcFound || step > pi) {
                errors.push_back(name + " is an arc that does not follow the moves of the " +
                                 "program.");
                break;
            }
            turned += step;
            at = mine.to;
            ++matched;
            ended = distance(mine.to, read.to) <= tolerance;
            if (!ended && turned > whole + 1e-6) {
                errors.push_back(name + " is an arc that ends before the moves of the program do.");
                break;
            }
        }
        if (!ended) {
            if (errors.empty()) {
                errors.push_back(name + " is an arc that goes further than the program does.");
            }
            return errors;
        }
    }
    if (matched != wanted.size()) {
        errors.push_back("It stops " + std::to_string(wanted.size() - matched) +
                         " moves short of the program, which has " + std::to_string(wanted.size()) +
                         ".");
    }
    return errors;
}

} // namespace lcad::cam
