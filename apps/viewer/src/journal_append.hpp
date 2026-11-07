#pragma once

#include <filesystem>
#include <fstream>
#include <ios>
#include <string>

namespace eawr::presentation::godot_backend {

// Appends text to a journal file, closing it again so earlier lines survive a stopped process. The first
// successful write per `first_write` flag truncates, so a rerun that reuses a journal path does not mix its
// lines with a previous run's; later writes append. A failed open leaves the flag set.
inline bool append_journal(const std::filesystem::path& path, const std::string& text, bool& first_write) {
    std::ofstream output(path, std::ios::binary | (first_write ? std::ios::trunc : std::ios::app));
    if (!output) return false;
    first_write = false;
    output << text;
    output.flush();
    return static_cast<bool>(output);
}

} // namespace eawr::presentation::godot_backend
