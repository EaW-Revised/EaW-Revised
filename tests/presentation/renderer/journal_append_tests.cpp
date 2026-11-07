#include "../../support/check.hpp"
#include "journal_append.hpp"

#include <filesystem>
#include <iostream>
#include <iterator>

namespace {
std::string slurp(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
} // namespace

int main() {
    using eawr::presentation::godot_backend::append_journal;
    eawr::testing::Checker checks;
    const auto check = [&](const bool value, const char* message) { checks.check(value, message, std::cerr); };
    const std::filesystem::path path = "journal_append_contract.jsonl";
    std::filesystem::remove(path);
    {
        // A previous run left lines behind under the same path.
        std::ofstream stale(path, std::ios::binary);
        stale << "stale-1\nstale-2\n";
    }
    bool first = true;
    check(append_journal(path, "a\n", first), "the first write must succeed");
    check(slurp(path) == "a\n", "the first write of a process must replace a previous run's lines");
    check(!first, "a successful first write must clear the flag");
    check(append_journal(path, "b\nc\n", first), "a later write must succeed");
    check(slurp(path) == "a\nb\nc\n", "later writes must append");
    bool second_process = true;
    check(append_journal(path, "z\n", second_process) && slurp(path) == "z\n",
        "a new process (fresh flag) must start the journal again");
    bool refused = true;
    check(!append_journal(std::filesystem::path("no_such_directory_for_journal") / "x.jsonl", "q\n", refused),
        "an unwritable path must report failure");
    check(refused, "a failed open must leave the truncate flag set for the next attempt");
    std::filesystem::remove(path);
    return checks.finish();
}
