#pragma once

#include <cstdint>
#include <optional>

namespace eawr::core::host {

// Questions about the machine the process runs on, for load-time tuning and diagnostics only. The answers never feed
// simulation state, hashes or replay bytes. They live here, outside the simulation-boundary directories, because they
// read the clock and the operating system.

// A monotonic clock in nanoseconds: only differences of two reads mean anything.
[[nodiscard]] std::int64_t monotonic_ns() noexcept;

// Physical memory in bytes; 0 when the host cannot say.
[[nodiscard]] std::uint64_t physical_memory_bytes() noexcept;

// Whether the volume of the drive letter (A-Z) reports a seek penalty (a spinning disk); empty when the host cannot say,
// and always empty off Windows.
[[nodiscard]] std::optional<bool> volume_seek_penalty(wchar_t drive_letter) noexcept;

} // namespace eawr::core::host
