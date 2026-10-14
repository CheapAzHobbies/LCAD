// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Arithmetic as a user types it into a value box or a parameter: "width / 2 + 3 mm".
namespace lcad::core {

/// What a name in an expression stands for, or nothing if there is no such name.
using Lookup = std::function<std::optional<double>(const std::string& name)>;

/// An expression that cannot be worked out. The message is a sentence fit to show.
class ExpressionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Works an expression out. It may hold:
///
/// - numbers, with "." before the decimals, each with an optional unit after it: mm, cm, m, in,
///   and ft for lengths, worked out in millimeters; deg and rad for angles, in degrees. A
///   number without a unit is taken as it is: millimeters for a length, degrees for an angle;
/// - names, which `lookup` gives the values of;
/// - + - * / and ^ (power), with the usual precedence, and parentheses;
/// - the functions sqrt, abs, sin, cos, tan (of degrees), asin, acos, atan (in degrees), min,
///   max (of two or more, separated by commas), round, floor, ceil; and pi.
///
/// Units are converted, not checked: "2 in + 3 deg" is 53.8. Throws ExpressionError.
[[nodiscard]] double evaluate(std::string_view expression, const Lookup& lookup = {});

/// The names an expression uses, each once, in the order they first appear. Throws
/// ExpressionError if the expression does not parse.
[[nodiscard]] std::vector<std::string> namesIn(std::string_view expression);

/// An expression with every use of the name `from` changed to `to`, and nothing else changed.
[[nodiscard]] std::string renamed(std::string_view expression, std::string_view from,
                                  std::string_view to);

/// Whether a name can be given to a parameter: a letter or "_", then letters, digits, and "_";
/// and not the name of a function, a unit, or pi.
[[nodiscard]] bool isValidName(std::string_view name);

/// Whether text is a plain number, which needs no working out: "40", "-2.5", "40 mm".
[[nodiscard]] bool isPlainNumber(std::string_view text);

} // namespace lcad::core
