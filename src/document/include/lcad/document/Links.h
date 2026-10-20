// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/Document.h>

#include <filesystem>
#include <string>
#include <vector>

// Linked components: documents that are parts of other documents. A link is kept as text in the
// component (Component::link), relative to the folder of the document that holds it where it
// can be, and these functions find, read, check, and gather the files behind the links.
namespace lcad::document {

/// What identifies a file's content: the same bytes give the same stamp wherever the file is.
/// Throws FileError if it cannot be read.
[[nodiscard]] std::string stampOf(const std::filesystem::path& file);

/// What a linked component takes from a document: every body that is shown in it, placed as it
/// is there, and the stamp of the file. Throws FileError if the file cannot be read.
[[nodiscard]] LinkedContent readLinkedContent(const std::filesystem::path& file);

/// The file a link leads to, for a document in `folder` (empty for one not saved yet).
[[nodiscard]] std::filesystem::path resolveLink(const std::string& link,
                                                const std::filesystem::path& folder);
/// How a document in `folder` refers to `file`: relative to the folder, or, when the document
/// has no folder yet, the whole path.
[[nodiscard]] std::string makeLink(const std::filesystem::path& file,
                                   const std::filesystem::path& folder);

/// How a linked component stands to its file.
struct LinkStatus {
    enum class State {
        UpToDate,
        /// The file is there, and holds something else than the component was last given.
        Changed,
        /// There is no file where the link leads. The copy kept in the document is shown.
        Missing,
        /// There is a file, and it cannot be read as an LCAD document.
        Unreadable,
    };
    ObjectId component;
    std::string name;
    std::string link;
    std::filesystem::path file;
    State state = State::UpToDate;
    /// Why it is missing or cannot be read, in a sentence.
    std::string problem;
    /// The links of the linked document itself, from its own folder.
    std::vector<LinkStatus> links;
};

/// The state of every linked component of `document`, which is saved at `path` (empty if it
/// is not saved yet), and of the links in the files they link to, as deep as they go.
[[nodiscard]] std::vector<LinkStatus> checkLinks(const Document& document,
                                                 const std::filesystem::path& path);

/// Reads every linked file that changed, and gives its component what it holds now, as one undo
/// step. Returns a sentence for each component that could not be updated.
std::vector<std::string> updateLinks(Document& document, const std::filesystem::path& path);

/// What packaging gathered.
struct PackageResult {
    /// The files written into the folder: the document first.
    std::vector<std::filesystem::path> files;
    /// What could not be gathered, in sentences: a linked file that is missing keeps its link,
    /// and the copy kept in the document stands in for it.
    std::vector<std::string> problems;
};

/// Gathers a document and every file it links to, and the files those link to, into `folder`,
/// with each copy's links leading to the copies beside it: a project that can be moved to
/// another computer whole. The document is written as it is now, under the name of `path` (or
/// "Untitled.lcad"). The files linked to are copied as they are on disk. Throws FileError if
/// the folder cannot be written; nothing outside it is changed.
PackageResult packageProject(const Document& document, const std::filesystem::path& path,
                             const std::filesystem::path& folder);

} // namespace lcad::document
