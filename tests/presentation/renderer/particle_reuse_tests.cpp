#include "particle_reuse.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>

namespace {
using namespace eawr::presentation::godot_backend::detail;
using Cache = ParticleReuseCache<std::unique_ptr<int>>;

void check(const bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(EXIT_FAILURE); }
}

void matching_and_ownership() {
    Cache cache;
    const ParticleReusePolicy policy{"colour", "normal", "program", 8};
    auto first = std::make_unique<int>(17);
    auto second = std::make_unique<int>(23);
    check(cache.retain(policy, 10, first, 0) && !first, "retention transfers ownership");
    check(cache.retain(policy, 20, second, 0) && !second, "duplicate policy retains a second emitter");
    const std::array different{
        ParticleReusePolicy{"other", "normal", "program", 8},
        ParticleReusePolicy{"colour", "other", "program", 8},
        ParticleReusePolicy{"colour", "normal", "other", 8},
        ParticleReusePolicy{"colour", "normal", "program", 9}};
    for (const auto& other : different) check(!cache.take(other), "every material field qualifies reuse");
    check(cache.bytes() == 30 && cache.size() == 2, "misses preserve retained resources");
    auto taken = cache.take(policy);
    check(taken && **taken == 17 && cache.bytes() == 20, "oldest matching resource reused first");
    taken = cache.take(policy);
    check(taken && **taken == 23 && cache.bytes() == 0 && cache.size() == 0, "take releases its charge");
    check(cache.hits() == 2 && cache.misses() == 4, "deterministic reuse work counters");
}

void bounds_and_teardown() {
    Cache cache;
    const ParticleReusePolicy policy{"colour", {}, "program", 0};
    auto value = std::make_unique<int>(5);
    check(!cache.retain(policy, std::numeric_limits<std::uint64_t>::max(), value, 0)
        && value && *value == 5, "oversized refusal preserves caller ownership");
    check(cache.retain(policy, Cache::max_bytes, value, 0), "exact byte limit admitted");
    value = std::make_unique<int>(6);
    check(!cache.retain(policy, 1, value, 0) && value, "byte overflow falls back without eviction");
    std::size_t released{};
    cache.drain([&](auto& held) { check(*held == 5, "teardown owns retained value"); held.reset(); ++released; });
    check(released == 1 && cache.size() == 0 && cache.bytes() == 0, "teardown drains ownership and charge");
    cache.drain([&](auto&) { ++released; });
    check(released == 1, "teardown is idempotent");
    for (std::size_t index = 0; index < Cache::max_emitters; ++index) {
        value = std::make_unique<int>(7);
        check(cache.retain(policy, 0, value, 0), "entry limit independent of byte charge");
    }
    value = std::make_unique<int>(8);
    check(!cache.retain(policy, 0, value, 0) && value, "entry cap bounds empty surfaces too");
    check(cache.peak_emitters() == Cache::max_emitters && cache.peak_bytes() == Cache::max_bytes,
        "high-water marks retain bounded lifetime evidence");
    auto taken = cache.take(policy);
    check(taken && cache.retain(policy, 1, value, 0), "reuse restores entry capacity");
    cache.drain([&](auto& held) { held.reset(); ++released; });
    check(released == Cache::max_emitters + 1 && cache.bytes() == 0, "every cached resource released once");
}

void charge_contracts() {
    check(particle_reuse_charge(100, 12, 12, 150, 2, 1024, 2048) == 5772,
        "GPU vertex attribute index and both texture allocations charged");
    constexpr auto limit = std::numeric_limits<std::uint64_t>::max();
    check(particle_reuse_charge(limit, 2, 0, 0, 0, 0, 0) == limit, "product overflow saturates");
    check(particle_reuse_charge(1, 1, 0, 0, 0, limit, 0) == limit, "sum overflow saturates");
    check(particle_reuse_charge(0, limit, limit, 0, limit, 0, 0) == 0, "empty geometry charge is zero");
}

void repeated_retirement_work() {
    Cache cache;
    const ParticleReusePolicy policy{"colour", {}, "program", 0};
    std::size_t creations{}, destructions{};
    for (std::size_t frame = 0; frame < 1000; ++frame) {
        std::array<std::unique_ptr<int>, 8> active;
        for (auto& resource : active) {
            if (auto retained = cache.take(policy)) resource = std::move(*retained);
            else { resource = std::make_unique<int>(1); ++creations; }
        }
        for (auto& resource : active) {
            if (!cache.retain(policy, 4096, resource, 0)) { resource.reset(); ++destructions; }
        }
        check(cache.bytes() == 8 * 4096, "repeated retirement has constant retained storage");
    }
    check(creations == 8 && destructions == 0 && cache.hits() == 999 * 8,
        "repeated lifecycle work needs only the initial resource creations");
    cache.drain([&](auto& resource) { resource.reset(); ++destructions; });
    check(creations == destructions && cache.size() == 0, "final teardown destroys each resource once");
}

void cold_retirement() {
    Cache cache;
    const ParticleReusePolicy policy{"colour", {}, "program", 0};
    for (int index = 0; index < 20; ++index) {
        auto value = std::make_unique<int>(index);
        check(cache.retain(policy, 16, value, 10), "cold retention succeeds");
    }
    std::size_t released{};
    const auto release = [&](auto& value) { value.reset(); ++released; };
    cache.expire(129, release);
    check(released == 0 && cache.size() == 20, "hot pool remains reusable before the boundary");
    auto reused = cache.take(policy);
    check(reused && **reused == 0, "hot reuse preserves oldest-match order");
    cache.expire(130, release);
    check(released == 8 && cache.size() == 11 && cache.bytes() == 176,
        "cold cleanup is bounded to eight resources and removes their byte charge");
    cache.expire(131, release);
    cache.expire(132, release);
    check(released == 19 && cache.size() == 0 && cache.bytes() == 0,
        "cold pool converges to zero without affecting the reused resource");
}

void restore_failure_backoff() {
    ParticleRestoreRetry retry;
    std::size_t attempts{}, diagnostics{};
    for (std::uint64_t frame = 0; frame < 600; ++frame) {
        if (!retry.admits(frame)) continue;
        ++attempts;
        diagnostics += retry.failed(frame) ? 1U : 0U;
    }
    check(attempts == 11 && diagnostics == 1, "persistent restore failure backs off and emits one diagnostic");
    check(!retry.admits(599), "retry does not allocate on every frame after a persistent failure");
    retry.succeeded();
    check(retry.admits(0) && retry.failed(0), "a successful restore resets both backoff and diagnostic lifetime");
}

void acquisition_scope_negative_control() {
    ParticleAssetWork work;
    work.acquired();
    check(work.total() == 1 && work.restores() == 0, "cold allocation counts outside restore");
    {
        [[maybe_unused]] const auto restore = work.restoring();
        work.acquired();
        {
            [[maybe_unused]] const auto nested = work.restoring();
            work.acquired();
        }
        check(work.restores() == 2, "negative control detects acquisitions in a restore scope");
    }
    work.acquired();
    check(work.total() == 4 && work.restores() == 2, "scope teardown restores ordinary acquisition accounting");
}

void empty_residency_lifetime() {
    using Policy = ParticleResidencyPolicy;
    check(!Policy::immediate(false, false, false), "resumable emptiness receives the bounded dwell");
    check(Policy::keep_identity(false, true), "opaque dwell preserves sorting identities");
    check(Policy::live_rids(0, 0, 1, 2) == 5,
        "a suspended opaque owner still owns three identity RIDs plus two shared RIDs");
    check(Policy::immediate(true, false, false) && !Policy::keep_identity(true, true),
        "opaque terminal upgrade releases the retained sorting identities");
    check(Policy::live_rids(0, 0, 0, 2) == 2 && Policy::live_rids(0, 0, 0, 0) == 0,
        "terminal retirement leaves shared pins and owner release leaves no RIDs");
    check(!Policy::immediate(true, true, true),
        "configured transparent terminal reuse receives the dwell before owner release");
    check(Policy::immediate(true, true, false) && Policy::immediate(true, false, true),
        "opaque eligibility and fog-disabled reuse both keep immediate terminal retirement");
    check(!Policy::keep_identity(false, false) && !Policy::keep_identity(true, false),
        "nonopaque retirement releases identities after its dwell");
}
} // namespace

int main() {
    matching_and_ownership();
    bounds_and_teardown();
    charge_contracts();
    repeated_retirement_work();
    cold_retirement();
    restore_failure_backoff();
    acquisition_scope_negative_control();
    empty_residency_lifetime();
}
