#include "eawr/assets/shader_source.hpp"
#include "eawr/vfs/vfs.hpp"
#include <algorithm>

namespace eawr::assets {
namespace {
core::Diagnostic problem(std::string_view code, std::string_view logical) {
    return {std::string(code), core::Severity::error, "shader source lookup refused",
            std::string(logical), {}, {}, {}};
}
core::Result<std::string> normalize(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.front() == '\\' ||
        path.find(':') != path.npos || path.size() > 4096)
        return core::Result<std::string>::failure(problem(vfs::diagnostic_codes::invalid_path, {}));
    std::string value(path);
    std::replace(value.begin(), value.end(), '\\', '/');
    std::vector<std::string_view> parts;
    for (std::size_t at = 0; at <= value.size();) {
        const auto end = value.find('/', at);
        const auto part = std::string_view(value).substr(at, end == value.npos ? value.size() - at : end - at);
        if (part == "..") {
            if (parts.empty()) return core::Result<std::string>::failure(problem(vfs::diagnostic_codes::invalid_path, {}));
            parts.pop_back();
        } else if (!part.empty() && part != ".") parts.push_back(part);
        if (end == value.npos) break;
        at = end + 1;
    }
    std::string joined;
    for (const auto part : parts) { if (!joined.empty()) joined += '/'; joined += part; }
    return vfs::canonicalize(joined);
}
}
ShaderSourceProvider::ShaderSourceProvider(const vfs::Vfs& mounted, std::string logical_root)
    : mounted_(mounted) {
    const auto root = vfs::canonicalize(logical_root);
    if (root) root_ = root.value();
}
core::Result<ShaderSource> ShaderSourceProvider::read(std::string_view logical, std::size_t max_bytes) const {
    const auto path = normalize(logical);
    if (!path) return core::Result<ShaderSource>::failure(path.error());
    if (root_.empty() || !path.value().starts_with(root_ + '/'))
        return core::Result<ShaderSource>::failure(problem(vfs::diagnostic_codes::invalid_path, path.value()));
    const auto record = mounted_.stat(path.value());
    if (!record) return core::Result<ShaderSource>::failure(record.error());
    if (record.value().size > max_bytes)
        return core::Result<ShaderSource>::failure(problem(shader_source_diagnostic_codes::budget, path.value()));
    auto bytes = mounted_.open(record.value());
    if (!bytes) return core::Result<ShaderSource>::failure(bytes.error());
    if (bytes.value().size() > max_bytes)
        return core::Result<ShaderSource>::failure(problem(shader_source_diagnostic_codes::budget, path.value()));
    return core::Result<ShaderSource>::success({source_from(record.value()), std::move(bytes).value()});
}
core::Result<ShaderSource> ShaderSourceProvider::include(std::string_view parent,
    std::string_view request, bool local, std::size_t max_bytes) const {
    // Reject absolute/drive requests before joining; allow bounded ../ inside root.
    if (request.empty() || request.front() == '/' || request.front() == '\\' ||
        request.find(':') != request.npos || request.size() > 4096)
        return core::Result<ShaderSource>::failure(problem(vfs::diagnostic_codes::invalid_path, {}));
    if (local) {
        const auto parent_path = normalize(parent);
        if (!parent_path || !parent_path.value().starts_with(root_ + '/'))
            return core::Result<ShaderSource>::failure(problem(vfs::diagnostic_codes::invalid_path, {}));
        auto found = read(parent_path.value().substr(0, parent_path.value().rfind('/') + 1) + std::string(request), max_bytes);
        if (found || found.error().code != vfs::diagnostic_codes::not_found) return found;
    }
    return read(root_ + '/' + std::string(request), max_bytes);
}
} // namespace eawr::assets
