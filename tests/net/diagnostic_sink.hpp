#pragma once

#include "eawr/net/desync_monitor.hpp"
#include "eawr/platform/publish_files.hpp"

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

namespace eawr::testing {

// Platform-backed exporter used by the integration contracts. Production callers
// choose their user-data root and supply the same DiagnosticSink contract.
inline net::DiagnosticSink platform_diagnostic_sink(std::filesystem::path root) {
    return [root = std::move(root)](const net::DiagnosticBundle& bundle) -> core::Result<std::string> {
        const auto fail = [](std::string message) {
            core::Diagnostic error;
            error.code = "EAWR-NET-0402";
            error.severity = core::Severity::error;
            error.message = std::move(message);
            return core::Result<std::string>::failure(std::move(error));
        };
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        if (ec) return fail("cannot create diagnostic root: " + ec.message());
        std::filesystem::path location;
        for (unsigned index = 0; index < 10000; ++index) {
            location = root / (bundle.id + "-" + std::to_string(index));
            if (std::filesystem::create_directory(location, ec)) break;
            if (ec || index == 9999) return fail("cannot create unique diagnostic directory: " + ec.message());
        }
        std::vector<platform::PublishedFile> files;
        for (const auto& file : bundle.files) {
            files.push_back({file.name, (location / file.name).string(), {file.bytes.begin(), file.bytes.end()}});
        }
        const auto published = platform::publish_files(files);
        if (published.failure) {
            auto message = published.failure->message;
            for (const auto& leftover : published.leftovers) message += "; " + leftover.message;
            return fail(std::move(message));
        }
        return core::Result<std::string>::success(location.string());
    };
}

} // namespace eawr::testing
