// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/Document.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// Reading and writing LCAD's native format, one .lcad file per document. The format is
// specified in docs/FILE_FORMAT.md.
namespace lcad::document {

/// The format version this LCAD writes, and the newest it reads.
inline constexpr int currentFormatVersion = 1;

/// A file could not be read or written. what() is a complete sentence meant for the user.
class FileError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct SaveOptions {
    /// A preview image as PNG bytes, stored in the file for file managers and the open dialog.
    /// Empty for none.
    std::string thumbnailPng;
};

/// Writes `document` to `path`, replacing any file there in one step: after a crash or power
/// loss the file is either entirely the old version or entirely the new one. Throws FileError
/// when the file cannot be written, in which case an existing file is untouched.
///
/// Does not call Document::markSaved(); the caller does once this returns.
void saveDocument(const Document& document, const std::filesystem::path& path,
                  const SaveOptions& options = {});

struct LoadedDocument {
    /// The document, with an empty undo history and no unsaved changes.
    std::unique_ptr<Document> document;
    /// The format version the file was in. Smaller than currentFormatVersion when the content
    /// was converted on the way in; the file itself is never changed by loading.
    int formatVersion = 0;
    /// The program that wrote the file, for example "LCAD 0.0.2".
    std::string writtenBy;
    /// Problems that did not prevent loading, as sentences for the user.
    std::vector<std::string> warnings;
};

/// Reads the document at `path`. Throws FileError when it is missing, damaged, not an LCAD
/// document, or in a newer format than this LCAD reads. A document is never opened partially.
[[nodiscard]] LoadedDocument loadDocument(const std::filesystem::path& path);

/// The preview image stored in the file as PNG bytes, or an empty string when it has none.
/// Throws FileError when the file cannot be read.
[[nodiscard]] std::string readThumbnail(const std::filesystem::path& path);

} // namespace lcad::document
