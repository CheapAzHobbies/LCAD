// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Exchange.h>

#include <lcad/core/Path.h>
#include <lcad/kernel/Error.h>

#include <zip.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace lcad::kernel {

namespace {

using Triangle = std::array<Point, 3>;

std::string bytesOf(const std::filesystem::path& path, const std::string& operation) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        throw Error(operation, "the file does not exist");
    }
    if (std::filesystem::is_directory(path, ec)) {
        throw Error(operation, "it is a folder, not a file");
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw Error(operation, "the file cannot be opened for reading");
    }
    in.seekg(0, std::ios::end);
    std::string bytes(static_cast<std::size_t>(in.tellg()), '\0');
    in.seekg(0, std::ios::beg);
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

float floatAt(const char* bytes) {
    std::uint32_t raw = 0;
    std::memcpy(&raw, bytes, sizeof raw);
    if constexpr (std::endian::native == std::endian::big) {
        raw = __builtin_bswap32(raw);
    }
    return std::bit_cast<float>(raw);
}

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string_view nextWord(std::string_view text, std::size_t& at) {
    while (at < text.size() && isSpace(text[at])) {
        ++at;
    }
    const std::size_t start = at;
    while (at < text.size() && !isSpace(text[at])) {
        ++at;
    }
    return text.substr(start, at - start);
}

std::optional<double> numberIn(std::string_view word) {
    if (!word.empty() && word.front() == '+') {
        word.remove_prefix(1);
    }
    double value = 0.0;
    const auto [end, code] = std::from_chars(word.data(), word.data() + word.size(), value);
    if (code != std::errc{} || end != word.data() + word.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

bool sameWord(std::string_view word, std::string_view lowercase) {
    return word.size() == lowercase.size() &&
           std::equal(word.begin(), word.end(), lowercase.begin(), [](char a, char b) {
               return (a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a) == b;
           });
}

std::vector<Triangle> binaryStl(const std::string& bytes) {
    constexpr std::size_t header = 84;
    constexpr std::size_t record = 50;
    std::uint32_t count = 0;
    std::memcpy(&count, bytes.data() + 80, sizeof count);
    if constexpr (std::endian::native == std::endian::big) {
        count = __builtin_bswap32(count);
    }
    std::vector<Triangle> triangles;
    triangles.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const char* corners = bytes.data() + header + i * record + 12;
        Triangle triangle;
        bool finite = true;
        for (std::size_t c = 0; c < 3; ++c) {
            const char* at = corners + c * 12;
            triangle[c] = {floatAt(at), floatAt(at + 4), floatAt(at + 8)};
            finite = finite && std::isfinite(triangle[c].x) && std::isfinite(triangle[c].y) &&
                     std::isfinite(triangle[c].z);
        }
        if (finite) {
            triangles.push_back(triangle);
        }
    }
    return triangles;
}

std::vector<Triangle> textStl(const std::string& bytes, const std::string& operation) {
    std::vector<Triangle> triangles;
    const std::string_view text(bytes);
    std::size_t at = 0;
    std::array<Point, 3> corners;
    std::size_t have = 0;
    while (true) {
        const std::string_view word = nextWord(text, at);
        if (word.empty()) {
            break;
        }
        if (!sameWord(word, "vertex")) {
            continue;
        }
        const auto x = numberIn(nextWord(text, at));
        const auto y = numberIn(nextWord(text, at));
        const auto z = numberIn(nextWord(text, at));
        if (!x || !y || !z) {
            throw Error(operation, "a vertex in the file is not three numbers");
        }
        corners[have++] = {*x, *y, *z};
        if (have == 3) {
            triangles.push_back(corners);
            have = 0;
        }
    }
    return triangles;
}

std::vector<Triangle> stl(const std::string& bytes, const std::string& operation) {
    // A binary file's size follows from the count in its header. A text file can start with the
    // word "solid", but so can a binary one, so the size is what tells them apart.
    if (bytes.size() >= 84) {
        std::uint32_t count = 0;
        std::memcpy(&count, bytes.data() + 80, sizeof count);
        if constexpr (std::endian::native == std::endian::big) {
            count = __builtin_bswap32(count);
        }
        if (84 + std::uint64_t{count} * 50 == bytes.size()) {
            return binaryStl(bytes);
        }
    }
    return textStl(bytes, operation);
}

std::vector<Triangle> obj(const std::string& bytes, const std::string& operation) {
    std::vector<Point> vertices;
    std::vector<Triangle> triangles;
    const std::string_view text(bytes);
    std::size_t line = 0;
    while (line < text.size()) {
        std::size_t end = text.find('\n', line);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view current = text.substr(line, end - line);
        line = end + 1;
        std::size_t at = 0;
        const std::string_view kind = nextWord(current, at);
        if (kind == "v") {
            const auto x = numberIn(nextWord(current, at));
            const auto y = numberIn(nextWord(current, at));
            const auto z = numberIn(nextWord(current, at));
            if (!x || !y || !z) {
                throw Error(operation, "a vertex in the file is not three numbers");
            }
            vertices.push_back({*x, *y, *z});
        } else if (kind == "f") {
            std::vector<std::size_t> corners;
            while (true) {
                const std::string_view word = nextWord(current, at);
                if (word.empty()) {
                    break;
                }
                // A corner may be given as v, v/vt, v//vn, or v/vt/vn; a negative number counts
                // back from the latest vertex.
                std::int64_t index = 0;
                const std::string_view first = word.substr(0, word.find('/'));
                const auto [p, code] =
                    std::from_chars(first.data(), first.data() + first.size(), index);
                if (code != std::errc{} || p != first.data() + first.size() || index == 0) {
                    throw Error(operation, "a face in the file refers to no vertex");
                }
                const std::int64_t resolved =
                    index > 0 ? index - 1 : static_cast<std::int64_t>(vertices.size()) + index;
                if (resolved < 0 || resolved >= static_cast<std::int64_t>(vertices.size())) {
                    throw Error(operation, "a face in the file refers to no vertex");
                }
                corners.push_back(static_cast<std::size_t>(resolved));
            }
            for (std::size_t c = 2; c < corners.size(); ++c) {
                triangles.push_back(
                    {vertices[corners[0]], vertices[corners[c - 1]], vertices[corners[c]]});
            }
        }
    }
    return triangles;
}

// 3MF is a zip file of XML. Only the elements below matter, and all that matters in them is
// their attributes, so tags are read one at a time rather than parsed into a tree.
struct Tag {
    std::string_view name;
    std::map<std::string, std::string, std::less<>> attributes;
    bool closing = false;
};

bool nextTag(std::string_view xml, std::size_t& at, Tag& tag) {
    while (true) {
        const std::size_t open = xml.find('<', at);
        if (open == std::string_view::npos) {
            return false;
        }
        if (xml.substr(open, 4) == "<!--") {
            const std::size_t end = xml.find("-->", open);
            if (end == std::string_view::npos) {
                return false;
            }
            at = end + 3;
            continue;
        }
        if (xml.substr(open, 2) == "<?" || xml.substr(open, 2) == "<!") {
            const std::size_t end = xml.find('>', open);
            if (end == std::string_view::npos) {
                return false;
            }
            at = end + 1;
            continue;
        }
        std::size_t p = open + 1;
        tag.closing = p < xml.size() && xml[p] == '/';
        p += tag.closing ? 1 : 0;
        const std::size_t nameStart = p;
        while (p < xml.size() && !isSpace(xml[p]) && xml[p] != '>' && xml[p] != '/') {
            ++p;
        }
        tag.name = xml.substr(nameStart, p - nameStart);
        const std::size_t colon = tag.name.rfind(':');
        if (colon != std::string_view::npos) {
            tag.name.remove_prefix(colon + 1);
        }
        tag.attributes.clear();
        while (p < xml.size() && xml[p] != '>') {
            if (isSpace(xml[p]) || xml[p] == '/') {
                ++p;
                continue;
            }
            const std::size_t keyStart = p;
            while (p < xml.size() && xml[p] != '=' && xml[p] != '>' && !isSpace(xml[p])) {
                ++p;
            }
            std::string_view key = xml.substr(keyStart, p - keyStart);
            while (p < xml.size() && xml[p] != '"' && xml[p] != '\'' && xml[p] != '>') {
                ++p;
            }
            if (p >= xml.size() || xml[p] == '>') {
                break;
            }
            const char quote = xml[p];
            const std::size_t valueStart = ++p;
            while (p < xml.size() && xml[p] != quote) {
                ++p;
            }
            tag.attributes.emplace(std::string(key),
                                   std::string(xml.substr(valueStart, p - valueStart)));
            ++p;
        }
        at = p + 1;
        return true;
    }
}

double attributeNumber(const Tag& tag, std::string_view name, double fallback) {
    const auto found = tag.attributes.find(name);
    if (found == tag.attributes.end()) {
        return fallback;
    }
    return numberIn(found->second).value_or(fallback);
}

using Matrix = std::array<double, 12>;
constexpr Matrix identity{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};

// A 3MF transform is twelve numbers, row by row with the fourth column left out: points are
// row vectors multiplied from the left.
Matrix matrixIn(const Tag& tag) {
    const auto found = tag.attributes.find(std::string_view("transform"));
    if (found == tag.attributes.end()) {
        return identity;
    }
    Matrix matrix = identity;
    std::size_t at = 0;
    for (double& value : matrix) {
        const auto number = numberIn(nextWord(found->second, at));
        if (!number) {
            return identity;
        }
        value = *number;
    }
    return matrix;
}

Matrix followedBy(const Matrix& a, const Matrix& b) {
    Matrix out{};
    for (std::size_t r = 0; r < 4; ++r) {
        for (std::size_t c = 0; c < 3; ++c) {
            double sum = r == 3 ? b[9 + c] : 0.0;
            for (std::size_t k = 0; k < 3; ++k) {
                sum += a[r * 3 + k] * b[k * 3 + c];
            }
            out[r * 3 + c] = sum;
        }
    }
    return out;
}

Point moved(const Matrix& m, const Point& p) {
    return {p.x * m[0] + p.y * m[3] + p.z * m[6] + m[9],
            p.x * m[1] + p.y * m[4] + p.z * m[7] + m[10],
            p.x * m[2] + p.y * m[5] + p.z * m[8] + m[11]};
}

struct ModelObject {
    std::vector<Point> vertices;
    std::vector<std::array<std::uint32_t, 3>> triangles;
    struct Part {
        std::string object;
        Matrix matrix;
    };
    std::vector<Part> parts;
};

double millimetersPer(std::string_view unit) {
    if (unit == "micron") {
        return 0.001;
    }
    if (unit == "centimeter") {
        return 10.0;
    }
    if (unit == "inch") {
        return 25.4;
    }
    if (unit == "foot") {
        return 304.8;
    }
    if (unit == "meter") {
        return 1000.0;
    }
    return 1.0;
}

// The objects of one model file by "<file>#<id>", and the items its build section places.
void readModel(const std::string& file, std::string_view xml,
               std::map<std::string, ModelObject>& objects,
               std::vector<ModelObject::Part>& items, double& unit) {
    std::size_t at = 0;
    Tag tag;
    ModelObject* current = nullptr;
    bool inBuild = false;
    while (nextTag(xml, at, tag)) {
        if (tag.name == "model" && !tag.closing) {
            const auto found = tag.attributes.find(std::string_view("unit"));
            if (found != tag.attributes.end()) {
                unit = millimetersPer(found->second);
            }
        } else if (tag.name == "object") {
            if (tag.closing) {
                current = nullptr;
            } else {
                const auto found = tag.attributes.find(std::string_view("id"));
                if (found != tag.attributes.end()) {
                    current = &objects[file + "#" + found->second];
                }
            }
        } else if (tag.closing) {
            if (tag.name == "build") {
                inBuild = false;
            }
        } else if (tag.name == "vertex" && current != nullptr) {
            current->vertices.push_back({attributeNumber(tag, "x", 0.0),
                                         attributeNumber(tag, "y", 0.0),
                                         attributeNumber(tag, "z", 0.0)});
        } else if (tag.name == "triangle" && current != nullptr) {
            current->triangles.push_back(
                {static_cast<std::uint32_t>(attributeNumber(tag, "v1", 0.0)),
                 static_cast<std::uint32_t>(attributeNumber(tag, "v2", 0.0)),
                 static_cast<std::uint32_t>(attributeNumber(tag, "v3", 0.0))});
        } else if (tag.name == "component" && current != nullptr) {
            // The production extension lets a component live in another model file.
            const auto path = tag.attributes.find(std::string_view("path"));
            const auto id = tag.attributes.find(std::string_view("objectid"));
            if (id != tag.attributes.end()) {
                current->parts.push_back(
                    {(path != tag.attributes.end() ? path->second : file) + "#" + id->second,
                     matrixIn(tag)});
            }
        } else if (tag.name == "build") {
            inBuild = true;
        } else if (tag.name == "item" && inBuild) {
            const auto id = tag.attributes.find(std::string_view("objectid"));
            const auto path = tag.attributes.find(std::string_view("path"));
            if (id != tag.attributes.end()) {
                items.push_back(
                    {(path != tag.attributes.end() ? path->second : file) + "#" + id->second,
                     matrixIn(tag)});
            }
        }
    }
}

void gather(const std::map<std::string, ModelObject>& objects, const std::string& key,
            const Matrix& placed, int depth, std::vector<Triangle>& out) {
    const auto found = objects.find(key);
    if (found == objects.end() || depth > 32) {
        return;
    }
    const ModelObject& object = found->second;
    for (const auto& corners : object.triangles) {
        if (std::max({corners[0], corners[1], corners[2]}) >= object.vertices.size()) {
            continue;
        }
        out.push_back({moved(placed, object.vertices[corners[0]]),
                       moved(placed, object.vertices[corners[1]]),
                       moved(placed, object.vertices[corners[2]])});
    }
    for (const ModelObject::Part& part : object.parts) {
        gather(objects, part.object, followedBy(part.matrix, placed), depth + 1, out);
    }
}

std::vector<Triangle> threeMf(const std::filesystem::path& path, const std::string& operation) {
    int code = 0;
    zip_t* archive = zip_open(core::toUtf8(path).c_str(), ZIP_RDONLY, &code);
    if (archive == nullptr) {
        throw Error(operation, "it is not a 3MF file");
    }
    std::map<std::string, ModelObject> objects;
    std::vector<ModelObject::Part> items;
    double unit = 1.0;
    // The root model comes first in the names only by chance, so it is looked for by its place.
    const zip_int64_t entries = zip_get_num_entries(archive, 0);
    for (zip_int64_t i = 0; i < entries; ++i) {
        const char* name = zip_get_name(archive, static_cast<zip_uint64_t>(i), 0);
        if (name == nullptr) {
            continue;
        }
        std::string entry = name;
        if (entry.size() < 6 || !sameWord(std::string_view(entry).substr(entry.size() - 6), ".model")) {
            continue;
        }
        zip_stat_t stat;
        zip_file_t* file = zip_fopen_index(archive, static_cast<zip_uint64_t>(i), 0);
        if (file == nullptr || zip_stat_index(archive, static_cast<zip_uint64_t>(i), 0, &stat) != 0) {
            if (file != nullptr) {
                zip_fclose(file);
            }
            continue;
        }
        std::string xml(static_cast<std::size_t>(stat.size), '\0');
        const zip_int64_t got = zip_fread(file, xml.data(), stat.size);
        zip_fclose(file);
        if (got < 0) {
            continue;
        }
        xml.resize(static_cast<std::size_t>(got));
        if (entry.front() != '/') {
            entry.insert(entry.begin(), '/');
        }
        double fileUnit = 1.0;
        std::vector<ModelObject::Part> fileItems;
        readModel(entry, xml, objects, fileItems, fileUnit);
        if (!fileItems.empty()) {
            items = std::move(fileItems);
            unit = fileUnit;
        }
    }
    zip_discard(archive);

    std::vector<Triangle> triangles;
    for (const ModelObject::Part& item : items) {
        gather(objects, item.object, item.matrix, 0, triangles);
    }
    if (unit != 1.0) {
        for (Triangle& triangle : triangles) {
            for (Point& p : triangle) {
                p = {p.x * unit, p.y * unit, p.z * unit};
            }
        }
    }
    return triangles;
}

// Corners that have the same place to within rounding become one vertex.
Mesh welded(const std::vector<Triangle>& triangles) {
    Point low{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
              std::numeric_limits<double>::max()};
    Point high{std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(),
               std::numeric_limits<double>::lowest()};
    for (const Triangle& triangle : triangles) {
        for (const Point& p : triangle) {
            low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
            high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
        }
    }
    const double size =
        std::hypot(high.x - low.x, high.y - low.y, high.z - low.z);
    const double cell = std::max(size * 1e-7, 1e-12);

    struct Key {
        std::int64_t x, y, z;
        bool operator==(const Key&) const = default;
    };
    struct Hash {
        std::size_t operator()(const Key& key) const {
            const auto mix = [](std::uint64_t a) { return a * 0x9E3779B97F4A7C15ULL; };
            return mix(static_cast<std::uint64_t>(key.x)) ^
                   (mix(static_cast<std::uint64_t>(key.y)) >> 1) ^
                   (mix(static_cast<std::uint64_t>(key.z)) << 1);
        }
    };
    std::unordered_map<Key, std::uint32_t, Hash> seen;
    seen.reserve(triangles.size());
    Mesh mesh;
    mesh.triangles.reserve(triangles.size());
    for (const Triangle& triangle : triangles) {
        std::array<std::uint32_t, 3> corners{};
        for (std::size_t c = 0; c < 3; ++c) {
            const Point& p = triangle[c];
            const Key key{std::llround(p.x / cell), std::llround(p.y / cell),
                          std::llround(p.z / cell)};
            const auto [found, added] =
                seen.try_emplace(key, static_cast<std::uint32_t>(mesh.vertices.size()));
            if (added) {
                mesh.vertices.push_back(p);
            }
            corners[c] = found->second;
        }
        if (corners[0] != corners[1] && corners[1] != corners[2] && corners[0] != corners[2]) {
            mesh.triangles.push_back(corners);
        }
    }
    return mesh;
}

} // namespace

Mesh readMesh(const std::filesystem::path& path) {
    const std::string operation = "read " + core::toUtf8(path.filename());
    std::string suffix = core::toUtf8(path.extension());
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    const std::string bytes = bytesOf(path, operation);
    std::vector<Triangle> triangles;
    if (suffix == ".3mf") {
        triangles = threeMf(path, operation);
    } else if (suffix == ".obj") {
        triangles = obj(bytes, operation);
    } else {
        triangles = stl(bytes, operation);
    }
    Mesh mesh = welded(triangles);
    if (mesh.triangles.empty()) {
        throw Error(operation, "it holds no triangles");
    }
    return mesh;
}

} // namespace lcad::kernel
