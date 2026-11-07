#pragma once
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace eawr::loadcheck {
inline constexpr std::size_t records_line_limit = 1024 * 1024;
inline void records_emit(std::string text) {
    if (text.size() > records_line_limit) throw std::runtime_error("records answer exceeded the line limit");
    text += '\n';
#ifdef _WIN32
    std::string_view bytes(text);
    while (!bytes.empty()) {
        DWORD written{};
        if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || !written)
            throw std::runtime_error("records output closed");
        bytes.remove_prefix(written);
    }
#else
    std::cout << text << std::flush;
#endif
}
class RecordsReader {
public:
    bool next(std::string& line) {
        for (;;) {
            const auto end = pending_.find('\n');
            if (end != std::string::npos) {
                if (end > records_line_limit) throw std::runtime_error("records request exceeded the line limit");
                line = pending_.substr(0, end); pending_.erase(0, end + 1);
                return true;
            }
            if (pending_.size() > records_line_limit) throw std::runtime_error("records request exceeded the line limit");
            char bytes[4096];
#ifdef _WIN32
            DWORD got{};
            if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), bytes, sizeof(bytes), &got, nullptr) || !got) return false;
#else
            std::cin.read(bytes, sizeof(bytes));
            const auto got = std::cin.gcount();
            if (!got) return false;
#endif
            pending_.append(bytes, static_cast<std::size_t>(got));
        }
    }
private:
    std::string pending_;
};
}
