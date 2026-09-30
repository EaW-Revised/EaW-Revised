#include "eawr/platform/publish_files.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <utility>

namespace eawr::platform {
namespace {

namespace fs = std::filesystem;

constexpr int staging_attempts = 16;

struct Entry {
    fs::path directory;
    fs::path staged;
    fs::path backup;
    bool staging = false;   // `directory` was created by this call
    bool backed_up = false; // `backup` holds the target's previous contents
    bool published = false;
    bool keep = false;      // a failed restore: the backup and its directory stay
};

[[nodiscard]] std::error_code inject(
    const PublishFault& fault, const std::string_view step, const fs::path& path) {
    return fault ? fault(step, path.string()) : std::error_code{};
}

[[nodiscard]] std::string random_token() {
    constexpr std::string_view digits = "0123456789abcdef";
    std::random_device device;
    std::string token;
    for (int word = 0; word < 2; ++word) {
        auto value = static_cast<std::uint32_t>(device());
        for (int digit = 0; digit < 8; ++digit) {
            token.push_back(digits[value & 0x0fU]);
            value >>= 4U;
        }
    }
    return token;
}

// A new directory beside the target. A name that something else already holds is skipped,
// never reused.
[[nodiscard]] std::error_code create_staging(
    const fs::path& target, const PublishFault& fault, Entry& entry) {
    for (int attempt = 0; attempt < staging_attempts; ++attempt) {
        auto candidate = target;
        candidate += ".eawr-" + random_token() + ".tmp";
        if (const auto error = inject(fault, "stage", candidate)) {
            return error;
        }
        std::error_code error;
        if (fs::create_directory(candidate, error)) {
            entry.directory = std::move(candidate);
            entry.staging = true;
            return {};
        }
        if (error && error != std::errc::file_exists) {
            return error;
        }
    }
    return std::make_error_code(std::errc::file_exists);
}

[[nodiscard]] bool write_text(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    return static_cast<bool>(output);
}

// Keeps an existing target's contents in the staging directory. A directory in the way is
// left for the rename to refuse.
[[nodiscard]] std::error_code back_up(
    const fs::path& target, const PublishFault& fault, Entry& entry) {
    std::error_code error;
    const auto status = fs::symlink_status(target, error);
    if (status.type() == fs::file_type::not_found || status.type() == fs::file_type::directory) {
        return {};
    }
    if (status.type() == fs::file_type::none) {
        return error ? error : std::make_error_code(std::errc::io_error);
    }
    entry.backup = entry.directory / "previous";
    if (const auto injected = inject(fault, "backup", entry.backup)) {
        return injected;
    }
    error.clear();
    fs::create_hard_link(target, entry.backup, error);
    if (error) {
        // Some file systems have no hard links; a copy keeps the contents.
        error.clear();
        fs::copy_file(target, entry.backup, error);
    }
    entry.backed_up = !error;
    return error;
}

[[nodiscard]] std::error_code remove_path(const fs::path& path, const PublishFault& fault) {
    if (auto error = inject(fault, "remove", path)) {
        return error;
    }
    std::error_code error;
    fs::remove(path, error);
    return error;
}

// Gives each published target back what it held: its previous file, or nothing.
void roll_back(
    const std::vector<PublishedFile>& files,
    std::vector<Entry>& entries,
    const PublishFault& fault,
    PublishReport& report) {
    for (std::size_t index = entries.size(); index-- > 0;) {
        auto& entry = entries[index];
        const auto& file = files[index];
        const fs::path target(file.path);
        if (!entry.published) {
            continue;
        }
        if (!entry.backed_up) {
            if (const auto error = remove_path(target, fault)) {
                report.leftovers.push_back({file.path,
                    "could not remove the new " + file.kind + " after the failure: " + error.message()});
            }
            continue;
        }
        auto error = inject(fault, "restore", target);
        if (!error) {
            fs::rename(entry.backup, target, error);
        }
        if (error) {
            entry.keep = true;
            report.leftovers.push_back({entry.backup.string(),
                "could not restore the previous " + file.kind + " to " + file.path
                    + ", which keeps the new one; the previous one is kept here: " + error.message()});
        } else {
            entry.backup.clear();
        }
    }
}

// Removes what the call created and no longer needs: staged files, backups, directories.
void clean_up(
    const std::vector<PublishedFile>& files,
    std::vector<Entry>& entries,
    const PublishFault& fault,
    PublishReport& report) {
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        const auto& kind = files[index].kind;
        if (!entry.staging || entry.keep) {
            continue;
        }
        bool emptied = true;
        if (!entry.staged.empty()) {
            if (const auto error = remove_path(entry.staged, fault)) {
                report.leftovers.push_back({entry.staged.string(),
                    "could not remove the staged copy of the " + kind + ": " + error.message()});
                emptied = false;
            }
        }
        if (!entry.backup.empty()) {
            if (const auto error = remove_path(entry.backup, fault)) {
                report.leftovers.push_back({entry.backup.string(),
                    "could not remove the backup of the previous " + kind + ": " + error.message()});
                emptied = false;
            }
        }
        if (emptied) {
            if (const auto error = remove_path(entry.directory, fault)) {
                report.leftovers.push_back({entry.directory.string(),
                    "could not remove the staging directory of the " + kind + ": " + error.message()});
            }
        }
    }
}

} // namespace

PublishReport publish_files(const std::vector<PublishedFile>& files, const PublishFault& fault) {
    PublishReport report;
    std::vector<Entry> entries(files.size());
    for (std::size_t index = 0; index < files.size(); ++index) {
        const auto& file = files[index];
        auto& entry = entries[index];
        if (const auto error = create_staging(fs::path(file.path), fault, entry)) {
            report.failure =
                PublishIssue{file.path, "could not stage " + file.kind + ": " + error.message()};
            break;
        }
        entry.staged = entry.directory / "new";
        if (inject(fault, "write", entry.staged) || !write_text(entry.staged, file.text)) {
            report.failure = PublishIssue{file.path, "could not write complete " + file.kind};
            break;
        }
        if (const auto error = back_up(fs::path(file.path), fault, entry)) {
            report.failure = PublishIssue{
                file.path, "could not keep the previous " + file.kind + ": " + error.message()};
            break;
        }
    }
    for (std::size_t index = 0; index < files.size() && !report.failure; ++index) {
        const auto& file = files[index];
        auto& entry = entries[index];
        const fs::path target(file.path);
        auto error = inject(fault, "publish", target);
        if (!error) {
            fs::rename(entry.staged, target, error);
        }
        if (error) {
            report.failure =
                PublishIssue{file.path, "could not publish " + file.kind + ": " + error.message()};
            break;
        }
        entry.staged.clear();
        entry.published = true;
    }
    if (report.failure) {
        roll_back(files, entries, fault, report);
    }
    clean_up(files, entries, fault, report);
    return report;
}

} // namespace eawr::platform
