#include "../support/temp_dir.hpp"
#include "../support/check.hpp"
#include "eawr/platform/publish_files.hpp"

#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using eawr::platform::PublishedFile;
using eawr::platform::PublishFault;
using eawr::platform::publish_files;
using eawr::platform::publish_path_files;

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void expect(const bool condition, const std::string_view message) {
    if (!condition) local_checker.check(condition, message, std::cerr, "FAIL: ");
}

void write(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

// Every path under root with its contents; a directory maps to "<dir>".
[[nodiscard]] std::map<std::string, std::string> tree(const fs::path& root) {
    std::map<std::string, std::string> entries;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        const auto name = fs::relative(entry.path(), root).generic_string();
        if (entry.is_directory()) {
            entries[name] = "<dir>";
        } else {
            std::ifstream input(entry.path(), std::ios::binary);
            entries[name] = {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        }
    }
    return entries;
}

// A fresh directory with an existing first target and an unrelated file named like the
// staging files of an earlier sim_headless.
[[nodiscard]] fs::path fresh(const fs::path& base, const std::string& name) {
    const auto root = base / name;
    fs::create_directories(root);
    write(root / "first.csv", "previous first\n");
    write(root / "first.csv.sim_headless-0.tmp", "unrelated\n");
    return root;
}

[[nodiscard]] std::vector<PublishedFile> files(const fs::path& root) {
    return {
        {"first output", (root / "first.csv").string(), "new first\n"},
        {"second output", (root / "second.csv").string(), "new second\n"},
    };
}

// Fails `step` on paths that belong to `target` (the target itself or its staging files).
[[nodiscard]] PublishFault fail_on(
    std::vector<std::pair<std::string, std::string>> steps) {
    return [steps = std::move(steps)](const std::string_view step, const std::string& path) {
        const auto text = fs::path(path).generic_string();
        for (const auto& [failing_step, target] : steps) {
            if (step == failing_step && text.find("/" + target) != std::string::npos) {
                return std::make_error_code(std::errc::permission_denied);
            }
        }
        return std::error_code{};
    };
}

[[nodiscard]] bool mentions(
    const std::vector<eawr::platform::PublishIssue>& issues, const std::string_view text) {
    for (const auto& issue : issues) {
        if (issue.message.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void test_success(const fs::path& base) {
    const auto root = fresh(base, "success");
    const auto report = publish_files(files(root));
    expect(!report.failure && report.leftovers.empty(), "a clean publication reports nothing");
    const std::map<std::string, std::string> expected{
        {"first.csv", "new first\n"},
        {"first.csv.sim_headless-0.tmp", "unrelated\n"},
        {"second.csv", "new second\n"},
    };
    expect(tree(root) == expected, "a publication leaves exactly the new files and unrelated ones");
}

void test_failed_steps_keep_targets(const fs::path& base) {
    for (const auto& [step, target] : std::vector<std::pair<std::string, std::string>>{
             {"stage", "second.csv"},
             {"write", "second.csv"},
             {"backup", "first.csv"},
             {"publish", "second.csv"},
         }) {
        const auto root = fresh(base, "failed-" + step);
        const auto before = tree(root);
        const auto report = publish_files(files(root), fail_on({{step, target}}));
        expect(report.failure.has_value(), "a failed " + step + " is reported");
        expect(report.leftovers.empty(), "a failed " + step + " cleans up completely");
        expect(tree(root) == before, "a failed " + step + " leaves every target as it was");
    }
}

void test_unicode_publication(const fs::path& base) {
    const auto root = base / fs::path(std::u8string(u8"\u03a3\u03b1\u03b2-\U0001f600"));
    fs::create_directories(root);
    const auto target = root / "save.eawrsave";
    const auto encoded = target.u8string();
    const std::string name(encoded.begin(), encoded.end());
    auto report = publish_path_files({{"save", target, "first"}});
    expect(!report.failure && fs::exists(target), "UTF-8 publication names the intended file");
    report = publish_path_files({{"save", target, "second"}}, [&](std::string_view step, const std::string& path) {
        expect(path.find(name) == 0, "fault paths preserve UTF-8");
        return step == "publish" ? std::make_error_code(std::errc::permission_denied) : std::error_code{};
    });
    expect(report.failure && report.failure->path == name && report.leftovers.empty(), "UTF-8 failure keeps its path and rolls back");
    std::ifstream input(target, std::ios::binary);
    const std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    expect(contents == "first", "failed Unicode overwrite preserves the previous file");
}

void test_native_narrow_publication(const fs::path& base) {
    const auto root = fresh(base, "native-narrow");
    const auto target = root / "ansi-output.csv";
    const auto name = target.string();
    const auto report = publish_files({{"native output", name, "native caller"}},
        [&](std::string_view step, const std::string& path) {
            expect(path.find(name) == 0, "native caller fault paths retain native separators and encoding");
            return step == "publish" ? std::make_error_code(std::errc::permission_denied) : std::error_code{};
        });
    expect(report.failure && report.failure->path == name && report.leftovers.empty(),
        "native caller failure retains its original narrow path");
    expect(!fs::exists(target), "failed native publication leaves an absent target absent");
    const auto success = publish_files({{"native output", name, "native caller"}});
    expect(!success.failure && fs::exists(target), "native narrow caller still publishes an ASCII target");
}

// Finding 4: a failed restore or removal is reported, and a backup that could not be put
// back is never removed.
void test_cleanup_failures_are_reported(const fs::path& base) {
    {
        const auto root = fresh(base, "failed-restore");
        const auto report = publish_files(
            files(root), fail_on({{"publish", "second.csv"}, {"restore", "first.csv"}}));
        expect(report.failure.has_value(), "the publish failure is still reported");
        expect(report.leftovers.size() == 1 && mentions(report.leftovers, "could not restore"),
            "a failed restore is reported as a leftover");
        const auto kept = report.leftovers.empty() ? std::string{} : report.leftovers.front().path;
        std::ifstream input(kept, std::ios::binary);
        const std::string contents{
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        expect(contents == "previous first\n", "the reported backup still holds the previous file");
        expect(!fs::exists(root / "second.csv"), "the unpublished target stays absent");
    }
    {
        const auto root = fresh(base, "failed-remove-new");
        fs::remove(root / "first.csv");
        const auto report = publish_files(
            files(root), fail_on({{"publish", "second.csv"}, {"remove", "first.csv"}}));
        expect(report.failure.has_value() && mentions(report.leftovers, "could not remove the new"),
            "a published target that cannot be removed again is reported");
    }
    {
        const auto root = fresh(base, "failed-remove-backup");
        const auto report = publish_files(files(root), fail_on({{"remove", "first.csv.eawr-"}}));
        expect(!report.failure, "the publication itself succeeded");
        expect(mentions(report.leftovers, "could not remove the backup"),
            "a backup that cannot be removed after success is reported");
        expect(tree(root).at("first.csv") == "new first\n", "the new file is in place");
    }
}

} // namespace

int main() {
    std::random_device device;
    const auto base = fs::temp_directory_path()
                      / ("eawr-publish-files-tests-" + std::to_string(device()));
    fs::create_directories(base);
    test_success(base);
    test_unicode_publication(base);
    test_native_narrow_publication(base);
    test_failed_steps_keep_targets(base);
    test_cleanup_failures_are_reported(base);
    eawr::testing::remove_temp_directory(base);
    if (failures != 0) {
        std::cerr << failures << " publish_files test(s) failed\n";
        return 1;
    }
    std::cout << "publish_files tests passed\n";
    return 0;
}
