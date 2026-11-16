#pragma once

#include <cstddef>
#include <string>

namespace eawr::core {

// Cuts `text` to at most `limit` bytes without splitting a UTF-8 character and appends "...[truncated]".
// Returns true when it cut. `limit` must be larger than the marker.
bool cut_text(std::string& text, std::size_t limit);

// Replaces the first absolute path in `text` (a drive path or a rooted path that follows a space, quote, '=' or '(')
// and everything after it with "[redacted-path]". Diagnostics must not carry user paths.
void redact_paths(std::string& text);

} // namespace eawr::core
