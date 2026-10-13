// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/document/File.h>

#include "Archive.h"
#include "Migration.h"

#include <lcad/core/Log.h>
#include <lcad/core/Version.h>
#include <lcad/kernel/Error.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Serialization.h>
#include <lcad/kernel/Version.h>

#include <nlohmann/json.hpp>

#include <random>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace lcad::document {

namespace {

using detail::ArchiveContents;
using nlohmann::json;

const char* const manifestName = "manifest.json";
const char* const documentName = "document.json";
const char* const thumbnailName = "thumbnail.png";
const char* const formatName = "lcad";

// Forces what was written to `path` (a file or a directory) onto the disk. A rename is only
// crash-safe once the new file's data, and then the directory entry, have been flushed.
void flushToDisk(const std::filesystem::path& path, bool isDirectory) {
#if defined(__unix__) || defined(__APPLE__)
    const int fd = ::open(path.c_str(), isDirectory ? O_RDONLY | O_DIRECTORY : O_RDONLY);
    if (fd < 0) {
        throw FileError("Could not flush " + path.string() + " to disk.");
    }
    const int result = ::fsync(fd);
    ::close(fd);
    // Some file systems cannot flush a directory; that is not a failed save.
    if (result != 0 && !isDirectory) {
        throw FileError("Could not flush " + path.string() + " to disk.");
    }
#else
    // The Windows port needs FlushFileBuffers here.
    (void)path;
    (void)isDirectory;
#endif
}

std::string randomSuffix() {
    std::random_device device;
    static const char digits[] = "0123456789abcdef";
    std::string suffix;
    for (int i = 0; i < 12; ++i) {
        suffix += digits[device() % 16];
    }
    return suffix;
}

void writeAtomically(const std::filesystem::path& requested,
                     const std::vector<std::pair<std::string, std::string>>& entries) {
    std::error_code ec;
    std::filesystem::path target = std::filesystem::absolute(requested);
    // Saving through a symbolic link updates the file it points to, not the link.
    if (std::filesystem::is_symlink(target, ec)) {
        const std::filesystem::path resolved = std::filesystem::canonical(target, ec);
        if (!ec) {
            target = resolved;
        }
    }
    const std::filesystem::path directory = target.parent_path();
    if (!std::filesystem::is_directory(directory, ec)) {
        throw FileError("The folder " + directory.string() + " does not exist.");
    }

    // In the same folder as the target, so that the rename below stays within one file system.
    const std::filesystem::path temporary =
        directory / ("." + target.filename().string() + "." + randomSuffix() + ".tmp");
    try {
        detail::writeArchive(temporary, entries);
        if (std::filesystem::exists(target, ec)) {
            // The new file takes the place of the old one, so it keeps its permissions.
            std::filesystem::permissions(temporary, std::filesystem::status(target).permissions(),
                                         ec);
        }
        flushToDisk(temporary, false);
        std::filesystem::rename(temporary, target, ec);
        if (ec) {
            const std::string reason = ec.message();
            if (std::filesystem::is_directory(target, ec)) {
                throw FileError(target.string() + " is a folder. Choose a file name inside it.");
            }
            throw FileError("Could not save " + target.string() + ": " + reason + ".");
        }
    } catch (...) {
        std::filesystem::remove(temporary, ec);
        throw;
    }
    flushToDisk(directory, true);
}

// Reading document.json: every wrong or missing value is reported, never guessed.

[[noreturn]] void damaged(const std::filesystem::path& path, const std::string& problem) {
    throw FileError(path.string() + " is damaged: " + problem + ".");
}

json parse(const detail::Entries& entries, const char* name, const std::filesystem::path& path) {
    const auto entry = entries.find(name);
    if (entry == entries.end()) {
        throw FileError(path.string() + " is not an LCAD document (it has no " + name + ").");
    }
    json value = json::parse(entry->second, nullptr, /*allow_exceptions=*/false);
    if (value.is_discarded() || !value.is_object()) {
        damaged(path, std::string(name) + " is not valid JSON");
    }
    return value;
}

const json& member(const json& object, const char* key, std::string_view where,
                   const std::filesystem::path& path) {
    const auto found = object.find(key);
    if (found == object.end()) {
        damaged(path, std::string(where) + " has no \"" + key + "\"");
    }
    return *found;
}

std::string stringMember(const json& object, const char* key, std::string_view where,
                         const std::filesystem::path& path) {
    const json& value = member(object, key, where, path);
    if (!value.is_string()) {
        damaged(path, "\"" + std::string(key) + "\" in " + std::string(where) + " is not text");
    }
    return value.get<std::string>();
}

std::uint64_t idMember(const json& object, const char* key, std::string_view where,
                       const std::filesystem::path& path) {
    const json& value = member(object, key, where, path);
    if (!value.is_number_unsigned()) {
        damaged(path,
                "\"" + std::string(key) + "\" in " + std::string(where) + " is not a whole number");
    }
    return value.get<std::uint64_t>();
}

bool boolMember(const json& object, const char* key, std::string_view where,
                const std::filesystem::path& path) {
    const json& value = member(object, key, where, path);
    if (!value.is_boolean()) {
        damaged(path,
                "\"" + std::string(key) + "\" in " + std::string(where) + " is not true or false");
    }
    return value.get<bool>();
}

const json& objectMember(const json& object, const char* key, std::string_view where,
                         const std::filesystem::path& path) {
    const json& value = member(object, key, where, path);
    if (!value.is_object()) {
        damaged(path,
                "\"" + std::string(key) + "\" in " + std::string(where) + " is not an object");
    }
    return value;
}

std::string describeWriter(const json& manifest) {
    const auto writer = manifest.find("writtenBy");
    if (writer == manifest.end() || !writer->is_object()) {
        return "an unknown program";
    }
    const std::string application = writer->value("application", std::string("an unknown program"));
    const std::string version = writer->value("version", std::string());
    return version.empty() ? application : application + " " + version;
}

DocumentState interpret(const ArchiveContents& contents, const std::filesystem::path& path,
                        std::vector<std::string>& warnings) {
    const json& units = objectMember(contents.manifest, "units", manifestName, path);
    const std::string lengthUnit = stringMember(units, "length", "the units", path);
    if (lengthUnit != "mm") {
        throw FileError(path.string() + " uses the length unit \"" + lengthUnit +
                        "\", which this version of LCAD does not support.");
    }

    DocumentState state;
    state.nextId = idMember(contents.document, "nextId", documentName, path);
    const json& bodies = member(contents.document, "bodies", documentName, path);
    if (!bodies.is_array()) {
        damaged(path, "\"bodies\" in document.json is not a list");
    }
    for (const json& entry : bodies) {
        if (!entry.is_object()) {
            damaged(path, "an entry of \"bodies\" is not an object");
        }
        Body body;
        body.id = ObjectId{idMember(entry, "id", "a body", path)};
        const std::string where = "body " + std::to_string(body.id.value);
        body.name = stringMember(entry, "name", where, path);
        body.visible = boolMember(entry, "visible", where, path);

        const json& geometry = objectMember(entry, "geometry", where, path);
        const std::string role = stringMember(geometry, "role", where, path);
        if (role != "source") {
            damaged(path, where + " has geometry of the unknown kind \"" + role + "\"");
        }
        const std::string file = stringMember(geometry, "file", where, path);
        const auto bytes = contents.entries.find(file);
        if (bytes == contents.entries.end()) {
            damaged(path, "the geometry of \"" + body.name + "\" (" + file + ") is missing");
        }
        try {
            body.shape = kernel::fromBrep(bytes->second);
        } catch (const kernel::Error& error) {
            damaged(path, "the geometry of \"" + body.name + "\" cannot be read (" +
                              error.reason() + ")");
        }
        if (!kernel::isValid(body.shape)) {
            warnings.push_back("The geometry of \"" + body.name +
                               "\" does not pass the validity check. It was loaded as it is.");
        }
        state.bodies.push_back(std::move(body));
    }
    return state;
}

} // namespace

void saveDocument(const Document& document, const std::filesystem::path& path,
                  const SaveOptions& options) {
    // Ordered, so that the files read top to bottom the way the specification lists them.
    using nlohmann::ordered_json;
    ordered_json manifest;
    manifest["format"] = formatName;
    manifest["formatVersion"] = currentFormatVersion;
    manifest["writtenBy"] = ordered_json{{"application", "LCAD"},
                                         {"version", core::version()},
                                         {"kernel", "OCCT " + kernel::occtVersion()}};
    manifest["units"] = ordered_json{{"length", "mm"}};

    ordered_json model;
    model["nextId"] = document.nextObjectId();
    model["bodies"] = ordered_json::array();

    std::vector<std::pair<std::string, std::string>> geometry;
    for (const Body& body : document.bodies()) {
        const std::string file = "geometry/" + std::to_string(body.id.value) + ".brep";
        try {
            geometry.emplace_back(file, kernel::toBrep(body.shape));
        } catch (const kernel::Error& error) {
            throw FileError("Could not save " + path.string() + ": the geometry of \"" + body.name +
                            "\" cannot be written (" + error.reason() + ").");
        }
        ordered_json entry;
        entry["id"] = body.id.value;
        entry["name"] = body.name;
        entry["visible"] = body.visible;
        entry["geometry"] = ordered_json{{"file", file}, {"role", "source"}};
        model["bodies"].push_back(std::move(entry));
    }

    std::vector<std::pair<std::string, std::string>> entries;
    entries.emplace_back(manifestName, manifest.dump(2) + "\n");
    entries.emplace_back(documentName, model.dump(2) + "\n");
    for (auto& entry : geometry) {
        entries.push_back(std::move(entry));
    }
    if (!options.thumbnailPng.empty()) {
        entries.emplace_back(thumbnailName, options.thumbnailPng);
    }
    writeAtomically(path, entries);
}

LoadedDocument loadDocument(const std::filesystem::path& path) {
    ArchiveContents contents;
    contents.entries = detail::readArchive(path);
    contents.manifest = parse(contents.entries, manifestName, path);

    const auto format = contents.manifest.find("format");
    if (format == contents.manifest.end() || *format != formatName) {
        throw FileError(path.string() + " is not an LCAD document.");
    }
    const json& versionValue = member(contents.manifest, "formatVersion", manifestName, path);
    if (!versionValue.is_number_integer()) {
        damaged(path, "its format version is not a whole number");
    }
    const auto version = versionValue.get<std::int64_t>();
    const std::string writer = describeWriter(contents.manifest);
    if (version > currentFormatVersion) {
        throw FileError(path.string() + " was saved by " + writer + " in format version " +
                        std::to_string(version) + ". This is LCAD " + core::version() +
                        ", which reads up to version " + std::to_string(currentFormatVersion) +
                        ". Open the file with " + writer + " or newer.");
    }
    if (version < 1) {
        damaged(path, "its format version is " + std::to_string(version));
    }

    contents.document = parse(contents.entries, documentName, path);
    detail::migrate(contents, static_cast<int>(version), currentFormatVersion,
                    detail::migrations());

    LoadedDocument loaded;
    loaded.formatVersion = static_cast<int>(version);
    loaded.writtenBy = writer;
    DocumentState state = interpret(contents, path, loaded.warnings);
    try {
        loaded.document = std::make_unique<Document>(std::move(state));
    } catch (const std::invalid_argument& error) {
        damaged(path, error.what());
    }
    for (const std::string& warning : loaded.warnings) {
        core::log(core::LogLevel::Warning, "document", path.string() + ": " + warning);
    }
    return loaded;
}

std::string readThumbnail(const std::filesystem::path& path) {
    return detail::readArchiveEntry(path, thumbnailName).value_or(std::string());
}

} // namespace lcad::document
