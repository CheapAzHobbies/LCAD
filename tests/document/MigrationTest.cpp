// SPDX-License-Identifier: GPL-3.0-or-later

#include "Migration.h"

#include <lcad/document/File.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace lcad::document;
using detail::ArchiveContents;
using detail::Migration;

TEST_CASE("Migrations run in order from the file's version to the current one",
          "[document][migration]") {
    std::vector<std::string> ran;
    const std::vector<Migration> table{
        [&](ArchiveContents& contents) {
            ran.emplace_back("1 to 2");
            contents.document["solids"] = contents.document["bodies"];
            contents.document.erase("bodies");
        },
        [&](ArchiveContents& contents) {
            ran.emplace_back("2 to 3");
            contents.document["count"] = contents.document["solids"].size();
        },
    };
    ArchiveContents contents;
    contents.manifest = {{"formatVersion", 1}};
    contents.document = {{"bodies", {"a", "b"}}};

    SECTION("all the way") {
        detail::migrate(contents, 1, 3, table);

        CHECK(ran == std::vector<std::string>{"1 to 2", "2 to 3"});
        CHECK(contents.manifest["formatVersion"] == 3);
        CHECK(contents.document["count"] == 2);
        CHECK_FALSE(contents.document.contains("bodies"));
    }

    SECTION("from a version in between") {
        contents.document = {{"solids", {"a"}}};
        detail::migrate(contents, 2, 3, table);

        CHECK(ran == std::vector<std::string>{"2 to 3"});
        CHECK(contents.document["count"] == 1);
    }

    SECTION("nothing to do when the file is current") {
        detail::migrate(contents, 3, 3, table);

        CHECK(ran.empty());
        CHECK(contents.document.contains("bodies"));
    }

    SECTION("a missing step is an error, not a silent skip") {
        CHECK_THROWS_AS(detail::migrate(contents, 1, 4, table), FileError);
        CHECK_THROWS_AS(detail::migrate(contents, 0, 3, table), FileError);
    }
}

TEST_CASE("LCAD has one migration for each format version before the current one",
          "[document][migration]") {
    CHECK(detail::migrations().size() == static_cast<std::size_t>(currentFormatVersion - 1));
}
