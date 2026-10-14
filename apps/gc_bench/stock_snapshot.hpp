#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

int stock_snapshot_benchmark(std::span<const std::size_t> workers, std::uint64_t samples,
    std::uint64_t warmup);
