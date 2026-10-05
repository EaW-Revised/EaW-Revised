#pragma once

#include "eawr/script/pglua.hpp"
#include "eawr/script/script_host.hpp"
#include "eawr/vfs/vfs.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace host_test_support {


extern int failures;


struct TempTree;

void expect(const bool condition, const std::string_view message);
void write_text(const std::filesystem::path& path, const std::string_view text);
std::vector<std::byte> read_hex(const std::filesystem::path& path);
void test_host();

struct TempTree {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("eawr-script-tests-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()
        ));
    TempTree();
    ~TempTree();
};


} // namespace host_test_support
