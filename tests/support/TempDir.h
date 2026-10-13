// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <random>
#include <string>

namespace lcad::test {

/// A fresh directory under the system's temporary directory, removed with everything in it
/// when the object goes out of scope.
class TempDir {
public:
    TempDir() {
        std::random_device device;
        m_path = std::filesystem::temp_directory_path() /
                 ("lcad-test-" + std::to_string(device()) + "-" + std::to_string(device()));
        std::filesystem::create_directories(m_path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return m_path; }
    [[nodiscard]] std::filesystem::path file(const std::string& name) const {
        return m_path / name;
    }

private:
    std::filesystem::path m_path;
};

} // namespace lcad::test
