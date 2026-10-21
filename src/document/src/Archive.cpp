// SPDX-License-Identifier: GPL-3.0-or-later

#include "Archive.h"

#include <lcad/core/Path.h>
#include <lcad/document/File.h>

#include <zip.h>

#include <memory>
#include <new>
#include <system_error>

namespace lcad::document::detail {

namespace {

std::string describe(int code) {
    zip_error_t error;
    zip_error_init_with_code(&error, code);
    const std::string text = zip_error_strerror(&error);
    zip_error_fini(&error);
    return text;
}

struct Closer {
    void operator()(zip_t* archive) const { zip_discard(archive); }
    void operator()(zip_file_t* file) const { zip_fclose(file); }
};
using ArchiveHandle = std::unique_ptr<zip_t, Closer>;
using FileHandle = std::unique_ptr<zip_file_t, Closer>;

ArchiveHandle openForReading(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        throw FileError("There is no file at " + core::toUtf8(path) + ".");
    }
    if (std::filesystem::is_directory(path, ec)) {
        throw FileError(core::toUtf8(path) + " is a folder, not a document.");
    }
    int code = 0;
    ArchiveHandle archive(zip_open(core::toUtf8(path).c_str(), ZIP_RDONLY, &code));
    if (!archive) {
        if (code == ZIP_ER_NOZIP || code == ZIP_ER_INCONS || code == ZIP_ER_EOF) {
            throw FileError(core::toUtf8(path) + " is not an LCAD document, or it is damaged.");
        }
        throw FileError("Could not open " + core::toUtf8(path) + ": " + describe(code) + ".");
    }
    return archive;
}

std::string readEntry(zip_t* archive, zip_uint64_t index, const std::filesystem::path& path,
                      std::uint64_t& budget) {
    zip_stat_t info;
    zip_stat_init(&info);
    if (zip_stat_index(archive, index, 0, &info) != 0 || (info.valid & ZIP_STAT_SIZE) == 0 ||
        (info.valid & ZIP_STAT_NAME) == 0) {
        throw FileError(core::toUtf8(path) + " is damaged: an entry cannot be read.");
    }
    if (info.size > budget) {
        throw FileError(core::toUtf8(path) + " holds more data than LCAD can open.");
    }
    budget -= info.size;

    FileHandle file(zip_fopen_index(archive, index, 0));
    if (!file) {
        throw FileError(core::toUtf8(path) + " is damaged: " + info.name + " cannot be read.");
    }
    std::string bytes;
    try {
        bytes.resize(static_cast<std::size_t>(info.size));
    } catch (const std::bad_alloc&) {
        throw FileError(core::toUtf8(path) + " holds more data than fits in memory.");
    }
    std::size_t done = 0;
    while (done < bytes.size()) {
        const zip_int64_t got = zip_fread(file.get(), bytes.data() + done, bytes.size() - done);
        if (got <= 0) {
            // Short data, or a checksum that does not match.
            throw FileError(core::toUtf8(path) + " is damaged: " + info.name + " is incomplete.");
        }
        done += static_cast<std::size_t>(got);
    }
    // The checksum is verified when the end of the entry is reached.
    char extra = 0;
    if (zip_fread(file.get(), &extra, 1) != 0) {
        throw FileError(core::toUtf8(path) + " is damaged: " + info.name +
                        " does not match its checksum.");
    }
    return bytes;
}

} // namespace

Entries readArchive(const std::filesystem::path& path, std::uint64_t byteLimit) {
    const ArchiveHandle archive = openForReading(path);
    Entries entries;
    std::uint64_t budget = byteLimit;
    const zip_int64_t count = zip_get_num_entries(archive.get(), 0);
    for (zip_int64_t i = 0; i < count; ++i) {
        const auto index = static_cast<zip_uint64_t>(i);
        const char* name = zip_get_name(archive.get(), index, 0);
        if (name == nullptr) {
            throw FileError(core::toUtf8(path) + " is damaged: an entry has no name.");
        }
        entries[name] = readEntry(archive.get(), index, path, budget);
    }
    return entries;
}

std::optional<std::string> readArchiveEntry(const std::filesystem::path& path,
                                            const std::string& name) {
    const ArchiveHandle archive = openForReading(path);
    const zip_int64_t index = zip_name_locate(archive.get(), name.c_str(), 0);
    if (index < 0) {
        return std::nullopt;
    }
    std::uint64_t budget = maximumArchiveBytes;
    return readEntry(archive.get(), static_cast<zip_uint64_t>(index), path, budget);
}

void writeArchive(const std::filesystem::path& path,
                  const std::vector<std::pair<std::string, std::string>>& entries) {
    int code = 0;
    zip_t* archive = zip_open(core::toUtf8(path).c_str(), ZIP_CREATE | ZIP_EXCL, &code);
    if (archive == nullptr) {
        throw FileError("Could not create " + core::toUtf8(path) + ": " + describe(code) + ".");
    }
    for (const auto& [name, bytes] : entries) {
        // The archive refers to `bytes` without copying; they stay alive until zip_close().
        zip_source_t* source = zip_source_buffer(archive, bytes.data(), bytes.size(), 0);
        if (source == nullptr ||
            zip_file_add(archive, name.c_str(), source, ZIP_FL_ENC_UTF_8) < 0) {
            const std::string reason = zip_strerror(archive);
            if (source != nullptr) {
                zip_source_free(source);
            }
            zip_discard(archive);
            throw FileError("Could not write " + name + " to " + core::toUtf8(path) + ": " +
                            reason + ".");
        }
    }
    // Nothing reaches the disk before this call.
    if (zip_close(archive) != 0) {
        const std::string reason = zip_strerror(archive);
        zip_discard(archive);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        throw FileError("Could not write " + core::toUtf8(path) + ": " + reason + ".");
    }
}

} // namespace lcad::document::detail
