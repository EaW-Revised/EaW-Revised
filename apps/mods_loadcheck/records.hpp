#pragma once
#include <filesystem>
#include <vector>
namespace eawr::loadcheck {
int records(const std::vector<std::filesystem::path>& args);
}
