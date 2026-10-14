// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/core/Expression.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>

namespace lcad::core {

namespace {

constexpr double pi = 3.14159265358979323846;

// Units, and what one of each is in millimeters or degrees.
const std::map<std::string, double, std::less<>>& units() {
    static const std::map<std::string, double, std::less<>> table{
        {"mm", 1.0},   {"cm", 10.0}, {"m", 1000.0},       {"in", 25.4},
        {"ft", 304.8}, {"deg", 1.0}, {"rad", 180.0 / pi},
    };
    return table;
}

bool isFunction(std::string_view name) {
    static const char* const functions[] = {"sqrt", "abs", "sin", "cos",   "tan",   "asin", "acos",
                                            "atan", "min", "max", "round", "floor", "ceil"};
    return std::any_of(std::begin(functions), std::end(functions),
                       [name](const char* f) { return name == f; });
}

bool startsName(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool inName(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// A recursive-descent reader. With `collect`, it gathers names instead of looking them up, and
// works nothing out.
class Reader {
public:
    Reader(std::string_view text, const Lookup* lookup, std::vector<std::string>* collect)
        : m_text(text), m_lookup(lookup), m_collect(collect) {}

    double all() {
        skipSpace();
        if (m_at >= m_text.size()) {
            throw ExpressionError("There is nothing to work out.");
        }
        const double value = sum();
        skipSpace();
        if (m_at < m_text.size()) {
            fail("\"" + std::string(1, m_text[m_at]) + "\" is not expected here");
        }
        return value;
    }

private:
    [[noreturn]] void fail(const std::string& what) const {
        throw ExpressionError(what + " (at character " + std::to_string(m_at + 1) + ").");
    }

    void skipSpace() {
        while (m_at < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_at]))) {
            ++m_at;
        }
    }

    bool take(char c) {
        skipSpace();
        if (m_at < m_text.size() && m_text[m_at] == c) {
            ++m_at;
            return true;
        }
        return false;
    }

    double sum() {
        double value = product();
        for (;;) {
            if (take('+')) {
                value += product();
            } else if (take('-')) {
                value -= product();
            } else {
                return value;
            }
        }
    }

    double product() {
        double value = power();
        for (;;) {
            if (take('*')) {
                value *= power();
            } else if (take('/')) {
                const double divisor = power();
                if (divisor == 0.0 && m_collect == nullptr) {
                    fail("It divides by zero");
                }
                value = m_collect == nullptr ? value / divisor : 0.0;
            } else {
                return value;
            }
        }
    }

    double power() {
        const double base = unary();
        if (take('^')) {
            // Right to left: 2^3^2 is 2^9.
            const double exponent = power();
            return std::pow(base, exponent);
        }
        return base;
    }

    double unary() {
        if (take('-')) {
            return -unary();
        }
        if (take('+')) {
            return unary();
        }
        return withUnit(primary());
    }

    // A unit right after a number or a parenthesis.
    double withUnit(double value) {
        skipSpace();
        const std::size_t start = m_at;
        if (m_at < m_text.size() && startsName(m_text[m_at]) && m_afterNumber) {
            std::size_t end = m_at;
            while (end < m_text.size() && inName(m_text[end])) {
                ++end;
            }
            const auto unit = units().find(m_text.substr(start, end - start));
            if (unit != units().end()) {
                m_at = end;
                m_afterNumber = false;
                return value * unit->second;
            }
        }
        m_afterNumber = false;
        return value;
    }

    double primary() {
        skipSpace();
        m_afterNumber = false;
        if (m_at >= m_text.size()) {
            fail("A number or a name is missing at the end");
        }
        const char c = m_text[m_at];
        if (take('(')) {
            const double value = sum();
            if (!take(')')) {
                fail("A \")\" is missing");
            }
            m_afterNumber = true;
            return value;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '.') {
            return number();
        }
        if (startsName(c)) {
            return name();
        }
        fail("\"" + std::string(1, c) + "\" is not expected here");
    }

    double number() {
        const std::size_t start = m_at;
        while (
            m_at < m_text.size() &&
            (std::isdigit(static_cast<unsigned char>(m_text[m_at])) != 0 || m_text[m_at] == '.')) {
            ++m_at;
        }
        // An exponent, as in 1e-3, but not the start of a name such as "e".
        if (m_at + 1 < m_text.size() && (m_text[m_at] == 'e' || m_text[m_at] == 'E')) {
            std::size_t after = m_at + 1;
            if (m_text[after] == '+' || m_text[after] == '-') {
                ++after;
            }
            if (after < m_text.size() &&
                std::isdigit(static_cast<unsigned char>(m_text[after])) != 0) {
                m_at = after;
                while (m_at < m_text.size() &&
                       std::isdigit(static_cast<unsigned char>(m_text[m_at])) != 0) {
                    ++m_at;
                }
            }
        }
        const std::string digits(m_text.substr(start, m_at - start));
        if (std::count(digits.begin(), digits.end(), '.') > 1 || digits == ".") {
            m_at = start;
            fail("\"" + digits + "\" is not a number");
        }
        char* end = nullptr;
        const double value = std::strtod(digits.c_str(), &end);
        m_afterNumber = true;
        return value;
    }

    double name() {
        const std::size_t start = m_at;
        while (m_at < m_text.size() && inName(m_text[m_at])) {
            ++m_at;
        }
        const std::string word(m_text.substr(start, m_at - start));
        if (word == "pi") {
            // A number like any other: "pi rad" is half a turn.
            m_afterNumber = true;
            return pi;
        }
        if (isFunction(word)) {
            if (!take('(')) {
                fail(word + " needs \"(\" after it");
            }
            std::vector<double> arguments{sum()};
            while (take(',')) {
                arguments.push_back(sum());
            }
            if (!take(')')) {
                fail("A \")\" is missing");
            }
            m_afterNumber = true;
            return apply(word, arguments);
        }
        if (units().contains(word)) {
            m_at = start;
            fail("The unit " + word + " has to follow a number");
        }
        if (m_collect != nullptr) {
            if (std::find(m_collect->begin(), m_collect->end(), word) == m_collect->end()) {
                m_collect->push_back(word);
            }
            return 1.0;
        }
        const std::optional<double> value =
            m_lookup != nullptr && *m_lookup ? (*m_lookup)(word) : std::nullopt;
        if (!value.has_value()) {
            throw ExpressionError("There is no parameter called " + word + ".");
        }
        return *value;
    }

    double apply(const std::string& function, const std::vector<double>& a) {
        const auto one = [&]() {
            if (a.size() != 1) {
                fail(function + " takes one value");
            }
            return a.front();
        };
        const double toRadians = pi / 180.0;
        if (m_collect != nullptr) {
            return 1.0;
        }
        if (function == "sqrt") {
            const double x = one();
            if (x < 0.0) {
                fail("sqrt of a negative number has no value");
            }
            return std::sqrt(x);
        }
        if (function == "abs") {
            return std::abs(one());
        }
        if (function == "sin") {
            return std::sin(one() * toRadians);
        }
        if (function == "cos") {
            return std::cos(one() * toRadians);
        }
        if (function == "tan") {
            return std::tan(one() * toRadians);
        }
        if (function == "asin" || function == "acos") {
            const double x = one();
            if (x < -1.0 || x > 1.0) {
                fail(function + " takes a value from -1 to 1");
            }
            return (function == "asin" ? std::asin(x) : std::acos(x)) / toRadians;
        }
        if (function == "atan") {
            return std::atan(one()) / toRadians;
        }
        if (function == "round") {
            return std::round(one());
        }
        if (function == "floor") {
            return std::floor(one());
        }
        if (function == "ceil") {
            return std::ceil(one());
        }
        if (a.size() < 2) {
            fail(function + " takes two values or more");
        }
        return function == "min" ? *std::min_element(a.begin(), a.end())
                                 : *std::max_element(a.begin(), a.end());
    }

    std::string_view m_text;
    const Lookup* m_lookup;
    std::vector<std::string>* m_collect;
    std::size_t m_at = 0;
    bool m_afterNumber = false;
};

} // namespace

double evaluate(std::string_view expression, const Lookup& lookup) {
    Reader reader(expression, &lookup, nullptr);
    const double value = reader.all();
    if (!std::isfinite(value)) {
        throw ExpressionError("It does not come out as a number.");
    }
    return value;
}

std::vector<std::string> namesIn(std::string_view expression) {
    std::vector<std::string> names;
    Reader reader(expression, nullptr, &names);
    (void)reader.all();
    return names;
}

std::string renamed(std::string_view expression, std::string_view from, std::string_view to) {
    std::string result;
    std::size_t at = 0;
    while (at < expression.size()) {
        const char c = expression[at];
        // A name starts with a letter that does not continue a number or another name.
        const bool continues = at > 0 && (inName(expression[at - 1]) || expression[at - 1] == '.');
        if (startsName(c) && !continues) {
            std::size_t end = at;
            while (end < expression.size() && inName(expression[end])) {
                ++end;
            }
            const std::string_view word = expression.substr(at, end - at);
            result += word == from ? std::string(to) : std::string(word);
            at = end;
            continue;
        }
        result += c;
        ++at;
    }
    return result;
}

bool isValidName(std::string_view name) {
    if (name.empty() || !startsName(name.front()) ||
        !std::all_of(name.begin(), name.end(), inName)) {
        return false;
    }
    return name != "pi" && !isFunction(name) && !units().contains(name);
}

bool isPlainNumber(std::string_view text) {
    try {
        return namesIn(text).empty() && text.find_first_of("+*/^(") == std::string_view::npos &&
               text.find('-', 1) == std::string_view::npos;
    } catch (const ExpressionError&) {
        return false;
    }
}

} // namespace lcad::core
