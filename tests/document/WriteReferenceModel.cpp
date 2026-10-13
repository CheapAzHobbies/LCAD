// SPDX-License-Identifier: GPL-3.0-or-later

#include "ReferenceModel.h"

#include <lcad/document/File.h>

#include <cstdio>
#include <exception>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <output.lcad>\n", argv[0]);
        return 2;
    }
    try {
        lcad::document::saveDocument(*lcad::test::buildReferenceDocument(), argv[1]);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
