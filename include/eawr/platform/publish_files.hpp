#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace eawr::platform {

// One whole file to publish. `kind` names it in messages ("hash output").
struct PublishedFile {
    std::string kind;
    std::string path;
    std::string text;
};

struct PublishIssue {
    std::string path;
    std::string message;
};

struct PublishReport {
    // Why nothing was published. Every target then keeps what it held before the call,
    // unless `leftovers` says otherwise.
    std::optional<PublishIssue> failure;
    // Files the call could not restore or remove. Each message names what is left behind,
    // including a backup that still holds a target's previous contents.
    std::vector<PublishIssue> leftovers;
};

// Test seam: called before each file-system step ("stage", "write", "backup", "publish",
// "restore", "remove") with the path it acts on. A nonzero result fails that step.
using PublishFault = std::function<std::error_code(std::string_view step, const std::string& path)>;

// Publishes every file or none. Each file is staged in a new directory beside its target
// (`<target>.eawr-<random>.tmp`, created exclusively, so no existing file is written or
// removed), an existing target is kept there as a hard link or copy, and only when all are
// staged are they renamed into place. A failed step renames the backups back, removes the
// targets that had none and removes only what the call created.
[[nodiscard]] PublishReport publish_files(
    const std::vector<PublishedFile>& files, const PublishFault& fault = {});

} // namespace eawr::platform
