// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/document/Links.h>

#include <lcad/document/File.h>
#include <lcad/kernel/Transform.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

namespace lcad::document {

namespace {

std::filesystem::path folderOf(const std::filesystem::path& path) {
    return path.empty() ? std::filesystem::path() : std::filesystem::absolute(path).parent_path();
}

// The same file however it is reached, as far as it can be told.
std::filesystem::path identity(const std::filesystem::path& file) {
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(file, ec);
    return ec ? std::filesystem::absolute(file).lexically_normal() : canonical;
}

std::vector<LinkStatus> check(const std::vector<Component>& components,
                              const std::filesystem::path& folder,
                              std::set<std::filesystem::path>& open) {
    std::vector<LinkStatus> statuses;
    for (const Component& component : components) {
        if (component.link.empty()) {
            continue;
        }
        LinkStatus status;
        status.component = component.id;
        status.name = component.name;
        status.link = component.link;
        status.file = resolveLink(component.link, folder);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(status.file, ec)) {
            status.state = LinkStatus::State::Missing;
            status.problem = "There is no file at " + status.file.string() + ".";
            statuses.push_back(std::move(status));
            continue;
        }
        try {
            status.state = stampOf(status.file) == component.linkStamp ? LinkStatus::State::UpToDate
                                                                       : LinkStatus::State::Changed;
            // What it links to in turn, unless that leads back to a file already being looked at.
            const std::filesystem::path same = identity(status.file);
            if (open.insert(same).second) {
                status.links =
                    check(readLinkedComponents(status.file), status.file.parent_path(), open);
                open.erase(same);
            }
        } catch (const FileError& error) {
            status.state = LinkStatus::State::Unreadable;
            status.problem = error.what();
        }
        statuses.push_back(std::move(status));
    }
    return statuses;
}

// A name for a file in the package that no other file there has: "wheel.lcad", "wheel 2.lcad".
std::string freeName(const std::filesystem::path& wanted, std::set<std::string>& taken) {
    const std::string stem = wanted.stem().string();
    const std::string extension = wanted.extension().string();
    std::string name = stem + extension;
    for (int number = 2; taken.contains(name); ++number) {
        name = stem + " " + std::to_string(number) + extension;
    }
    taken.insert(name);
    return name;
}

} // namespace

std::string stampOf(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw FileError("Could not read " + file.string() + ".");
    }
    // FNV-1a over the bytes: enough to tell one version of a file from another.
    std::uint64_t hash = 14695981039346656037ULL;
    std::uint64_t size = 0;
    char buffer[65536];
    while (in) {
        in.read(buffer, sizeof buffer);
        const std::streamsize count = in.gcount();
        for (std::streamsize i = 0; i < count; ++i) {
            hash ^= static_cast<unsigned char>(buffer[i]);
            hash *= 1099511628211ULL;
        }
        size += static_cast<std::uint64_t>(count);
    }
    if (in.bad()) {
        throw FileError("Could not read " + file.string() + ".");
    }
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(hash));
    return std::to_string(size) + "-" + text;
}

LinkedContent readLinkedContent(const std::filesystem::path& file) {
    const LoadedDocument loaded = loadDocument(file);
    const Document& document = *loaded.document;
    LinkedContent content;
    for (const Instance& instance : document.instances()) {
        if (!instance.visible) {
            continue;
        }
        // Tells the bodies apart, also where the same component appears twice: the occurrences
        // on the way to it, then the body.
        std::string prefix;
        for (const ObjectId occurrence : instance.path) {
            prefix += std::to_string(occurrence.value) + "/";
        }
        for (const Body& body : document.bodies()) {
            if (body.component != instance.component || !body.visible || body.shape.isNull()) {
                continue;
            }
            LinkedBody linked;
            linked.key = prefix + std::to_string(body.id.value);
            linked.name = instance.path.empty()
                              ? body.name
                              : document.occurrenceName(instance.path.back()) + " " + body.name;
            linked.shape = kernel::placed(body.shape, instance.placement);
            linked.faceNames = body.faceNames;
            linked.material = body.material;
            content.bodies.push_back(std::move(linked));
        }
    }
    content.stamp = stampOf(file);
    return content;
}

std::filesystem::path resolveLink(const std::string& link, const std::filesystem::path& folder) {
    const std::filesystem::path given(link);
    if (given.is_absolute() || folder.empty()) {
        return given.lexically_normal();
    }
    return (folder / given).lexically_normal();
}

std::string makeLink(const std::filesystem::path& file, const std::filesystem::path& folder) {
    const std::filesystem::path whole = std::filesystem::absolute(file).lexically_normal();
    if (folder.empty()) {
        return whole.generic_string();
    }
    const std::filesystem::path relative =
        whole.lexically_relative(std::filesystem::absolute(folder).lexically_normal());
    return relative.empty() ? whole.generic_string() : relative.generic_string();
}

std::vector<LinkStatus> checkLinks(const Document& document, const std::filesystem::path& path) {
    std::set<std::filesystem::path> open;
    if (!path.empty()) {
        open.insert(identity(path));
    }
    return check(document.components(), folderOf(path), open);
}

std::vector<std::string> updateLinks(Document& document, const std::filesystem::path& path) {
    std::vector<std::string> problems;
    const std::vector<LinkStatus> statuses = checkLinks(document, path);
    Transaction transaction(document, "Update Links");
    for (const LinkStatus& status : statuses) {
        switch (status.state) {
        case LinkStatus::State::UpToDate:
            break;
        case LinkStatus::State::Changed:
            try {
                document.updateLinkedComponent(status.component, readLinkedContent(status.file));
            } catch (const FileError& error) {
                problems.push_back(status.name + " could not be updated. " + error.what());
            }
            break;
        case LinkStatus::State::Missing:
        case LinkStatus::State::Unreadable:
            problems.push_back(status.name + " could not be updated. " + status.problem);
            break;
        }
    }
    transaction.commit();
    return problems;
}

PackageResult packageProject(const Document& document, const std::filesystem::path& path,
                             const std::filesystem::path& folder) {
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (!std::filesystem::is_directory(folder, ec)) {
        throw FileError("The folder " + folder.string() + " cannot be made.");
    }
    PackageResult result;
    std::set<std::string> taken;
    const std::string hostName =
        freeName(path.empty() ? std::filesystem::path("Untitled.lcad") : path.filename(), taken);
    // Each file linked to, by what it is, and the name its copy has in the package.
    std::map<std::filesystem::path, std::string> copies;
    if (!path.empty()) {
        copies[identity(path)] = hostName;
    }

    // The links a document's copy is to have: to the copies beside it, with the stamps of the
    // copies where the document was up to date with the files. A file that cannot be found
    // keeps a link to where it was looked for.
    std::map<std::filesystem::path, std::string> stamps;
    std::function<SaveOptions(const std::vector<Component>&, const std::filesystem::path&)> gather =
        [&](const std::vector<Component>& components,
            const std::filesystem::path& from) -> SaveOptions {
        SaveOptions written;
        std::map<ObjectId, std::string>& links = written.links;
        const auto stampFor = [&](const Component& component, const std::filesystem::path& file,
                                  const std::filesystem::path& same) {
            const auto copied = stamps.find(same);
            try {
                if (copied != stamps.end() && stampOf(file) == component.linkStamp) {
                    written.linkStamps[component.id] = copied->second;
                }
            } catch (const FileError&) {
            }
        };
        for (const Component& component : components) {
            if (component.link.empty()) {
                continue;
            }
            const std::filesystem::path file = resolveLink(component.link, from);
            if (!std::filesystem::is_regular_file(file, ec)) {
                result.problems.push_back(component.name + " links to " + file.string() +
                                          ", which is missing. The copy of it kept in the "
                                          "document stands in for it.");
                links[component.id] = std::filesystem::absolute(file).generic_string();
                continue;
            }
            const std::filesystem::path same = identity(file);
            const auto copied = copies.find(same);
            if (copied != copies.end()) {
                links[component.id] = copied->second;
                stampFor(component, file, same);
                continue;
            }
            const std::string name = freeName(file.filename(), taken);
            copies[same] = name;
            links[component.id] = name;
            const std::filesystem::path target = folder / name;
            try {
                const std::vector<Component> inner = readLinkedComponents(file);
                if (inner.empty()) {
                    std::filesystem::copy_file(
                        file, target, std::filesystem::copy_options::overwrite_existing, ec);
                    if (ec) {
                        throw FileError("Could not copy " + file.string() + " to " +
                                        target.string() + ": " + ec.message() + ".");
                    }
                } else {
                    // A document that links to others is written again, its links leading to
                    // the copies.
                    const LoadedDocument loaded = loadDocument(file);
                    saveDocument(*loaded.document, target, gather(inner, file.parent_path()));
                }
                result.files.push_back(target);
                stamps[same] = stampOf(target);
                stampFor(component, file, same);
            } catch (const FileError& error) {
                result.problems.push_back(component.name + " could not be packaged. " +
                                          error.what());
            }
        }
        return written;
    };

    const std::filesystem::path host = folder / hostName;
    saveDocument(document, host, gather(document.components(), folderOf(path)));
    result.files.insert(result.files.begin(), host);
    return result;
}

} // namespace lcad::document
