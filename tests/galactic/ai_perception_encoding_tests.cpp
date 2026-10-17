#include "galactic_support.hpp"
#include "galactic_ai_perception_state.hpp"
#include "eawr/core/record_digest.hpp"

#include <atomic>
#include <cstdlib>
#include <new>
#include <span>
#include <stdexcept>
#include <thread>

// Test-only allocation probe, armed solely around the encoders under test.
namespace encode_alloc_probe {
std::atomic<bool> armed{false};
std::atomic<std::size_t> calls{0};
}
void* operator new(std::size_t size) {
    if (encode_alloc_probe::armed.load(std::memory_order_relaxed))
        encode_alloc_probe::calls.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace {
namespace ai = eawr::script::foc::ai;
using galactic_test::expect;
using Bytes = std::vector<std::uint8_t>;

ai::Real bits(std::uint64_t repr) { ai::Real value; value.repr = repr; return value; }

Bytes from_hex(const std::string& text) {
    Bytes out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
    return out;
}
// Domain "eawr-galactic-perception" (length 24), version 3.
const std::string head = "18000000" "65617772" "2d" "67616c6163746963" "2d" "70657263657074696f6e" "03000000";
const std::string zero_word = "0000000000000000";

struct CacheRecord {
    ai::GalacticCacheKey key;
    ai::Real value{}, interval{};
    std::uint64_t frame{};
};
CacheRecord rich_cache() {
    CacheRecord record;
    record.key.player = 3; record.key.enemy = 2; record.key.human = std::nullopt; record.key.planet = 41;
    record.key.tokens = {"GROUND", "STARBASE_LEVEL"};
    record.key.bindings = {{"PARAMETER_CATEGORY", bits(0x8000000000000000ULL)}, {"PARAMETER_TYPE", std::string("FRIGATE")},
        {"PARAMETER_X", bits(0x3ff0000000000000ULL)}};
    record.value = bits(0x4008000000000000ULL); record.interval = bits(0x3ff0000000000000ULL); record.frame = 1234567;
    return record;
}
CacheRecord bare_cache() {
    CacheRecord record;
    record.key.player = 1; record.key.human = 1; record.key.planet = 0;
    record.value = bits(0); record.interval = bits(0x8000000000000000ULL); record.frame = 0;
    return record;
}
CacheRecord pinned_cache() {
    CacheRecord record;
    record.key.player = 1; record.key.planet = 5; record.key.tokens = {"T"}; record.key.bindings = {{"P", bits(0x0102030405060708ULL)}};
    record.value = bits(0x1112131415161718ULL); record.interval = bits(0x2122232425262728ULL); record.frame = 9;
    return record;
}
ai::PerceptionKeyView view_of(const CacheRecord& record) {
    const auto& key = record.key;
    return {key.player, key.enemy, key.human, key.planet, &key.tokens, &key.bindings};
}
Bytes reference_cache(const CacheRecord& record) {
    ai::GalacticPerceptionState state;
    state.cache.push_back({record.key, record.value, record.interval, record.frame});
    return ai::encode_perception_state(state, {});
}
Bytes fast_cache(const CacheRecord& record, Bytes& buffer) {
    ai::encode_cache_record(buffer, view_of(record), record.value, record.interval, record.frame);
    return buffer;
}
Bytes reference_visibility(const ai::GalacticVisibility& row) {
    ai::GalacticPerceptionState state; state.visibility.push_back(row); return ai::encode_perception_state(state, {});
}
Bytes reference_flag(const ai::GalacticStoryFlag& row) {
    ai::GalacticPerceptionState state; state.flags.push_back(row); return ai::encode_perception_state(state, {});
}
Bytes reference_hint(const ai::GalacticHint& row) {
    ai::GalacticPerceptionState state; state.hints.push_back(row); return ai::encode_perception_state(state, {});
}

const ai::GalacticVisibility visibility_row{2, 9, true, 77, 5};
const ai::GalacticVisibility hidden_row{4, 0, false, 0, 0};
const ai::GalacticStoryFlag flag_row{2, 7, "AB"};
const ai::GalacticStoryFlag flag_untargeted{1, 0, ""};
const ai::GalacticHint hint_row{3, "ALPHA", "SCOUTED", bits(0x4000000000000000ULL)};

void equals_reference() {
    Bytes buffer;
    // Dirty the buffer first: a long record must not leak into a short one.
    static_cast<void>(fast_cache(rich_cache(), buffer));
    for (const auto& record : {rich_cache(), bare_cache(), pinned_cache(), rich_cache(), bare_cache()})
        expect(fast_cache(record, buffer) == reference_cache(record), "cache record bytes equal the whole-state encoder");
    for (const auto& row : {visibility_row, hidden_row, visibility_row}) {
        ai::encode_visibility_record(buffer, row);
        expect(buffer == reference_visibility(row), "visibility record bytes equal the whole-state encoder");
    }
    for (const auto& row : {flag_row, flag_untargeted, flag_row}) {
        ai::encode_flag_record(buffer, row);
        expect(buffer == reference_flag(row), "flag record bytes equal the whole-state encoder");
    }
    ai::encode_hint_record(buffer, hint_row);
    expect(buffer == reference_hint(hint_row), "hint record bytes equal the whole-state encoder");
}

// Layouts spelled from the format itself, independent of both encoders.
void pinned_bytes() {
    Bytes buffer;
    ai::encode_flag_record(buffer, flag_row);
    expect(buffer == from_hex(head + zero_word + zero_word + "0100000000000000" + "02000000" + "0700000000000000"
        + "02000000" "4142" + zero_word + zero_word), "flag record bytes are pinned");
    ai::encode_visibility_record(buffer, visibility_row);
    expect(buffer == from_hex(head + zero_word + "0100000000000000" + "02000000" + "0900000000000000" + "01"
        + "4d00000000000000" + "05000000" + zero_word + zero_word + zero_word), "visibility record bytes are pinned");
    ai::encode_hint_record(buffer, hint_row);
    expect(buffer == from_hex(head + zero_word + zero_word + zero_word + "0100000000000000" + "03000000"
        + "05000000" "414c504841" + "07000000" "53434f55544544" + "0000000000000040" + zero_word), "hint record bytes are pinned");
    const auto record = pinned_cache();
    ai::encode_cache_record(buffer, view_of(record), record.value, record.interval, record.frame);
    expect(buffer == from_hex(head + "0100000000000000" + "01000000" "00" "00" "0500000000000000"
        + "0100000000000000" "01000000" "54" + "0100000000000000" + "01000000" "50" "00" "0807060504030201"
        + "1817161514131211" "2827262524232221" "0900000000000000" + zero_word + zero_word + zero_word + zero_word),
        "cache record bytes are pinned");
}

// Warm buffer: the encoders themselves allocate nothing. The whole-state path
// is probed too, so a probe that never counts would fail the test.
void allocations() {
    Bytes buffer;
    const auto rich = rich_cache();
    ai::encode_cache_record(buffer, view_of(rich), rich.value, rich.interval, rich.frame);
    ai::encode_visibility_record(buffer, visibility_row);
    ai::encode_flag_record(buffer, flag_row);
    ai::encode_hint_record(buffer, hint_row);
    encode_alloc_probe::calls = 0; encode_alloc_probe::armed = true;
    for (int i = 0; i < 100; ++i) {
        ai::encode_cache_record(buffer, view_of(rich), rich.value, rich.interval, rich.frame);
        ai::encode_visibility_record(buffer, visibility_row);
        ai::encode_flag_record(buffer, flag_row);
        ai::encode_hint_record(buffer, hint_row);
    }
    encode_alloc_probe::armed = false;
    expect(encode_alloc_probe::calls == 0, "warm single-record encoders allocate nothing");
    encode_alloc_probe::calls = 0; encode_alloc_probe::armed = true;
    static_cast<void>(reference_cache(rich));
    encode_alloc_probe::armed = false;
    expect(encode_alloc_probe::calls > 0, "allocation probe counts whole-state encodes");
}

// Representative record-digest traffic: word slots, span slots of every length
// class (empty, 3, 8 and 16 bytes), payloads below and above the 32-byte
// fingerprint stripe, overwrites, a relocation and erases that empty a shard.
// Expected values come from an independent byte-level model of the framing
// (player, kind, payload -> two XXH64 seeds, per-player sums, SHA-256 of the
// shard list), not from RecordDigest itself.
Bytes payload(std::size_t size, unsigned seed) {
    Bytes out(size);
    for (std::size_t i = 0; i < size; ++i) out[i] = static_cast<std::uint8_t>(seed + 7 * i);
    return out;
}
Bytes word_bytes(std::uint64_t value) { Bytes out; eawr::core::RecordDigest::append(out, value); return out; }
void digest_traffic(eawr::core::RecordDigest& digest) {
    using Slot = std::span<const std::uint8_t>;
    const Bytes three{'a', 'b', 'c'}, sixteen = payload(16, 0);
    digest.set(1, 0, 5, payload(3, 1));
    digest.set(1, 0, 6, payload(40, 2));
    digest.set(2, 1, 0, Bytes{});
    digest.set(2, 1, Slot{}, payload(5, 3));
    digest.set(2, 1, Slot(three), payload(9, 4));
    digest.set(2, 1, Slot(sixteen), payload(70, 5));
    digest.set(1, 0, Slot(word_bytes(5)), payload(11, 6));
    digest.set(1, 0, 5, payload(3, 1));
    digest.set(3, 2, 9, payload(1, 8));
    digest.set(3, 2, 10, payload(2, 9));
    digest.relocate(3, 2, 10, 4);
    digest.set(3, 2, 10, payload(6, 10));
    digest.erase(3, 2, 9);
    digest.erase(3, 2, 4); digest.erase(3, 2, Slot(word_bytes(10)));
    digest.erase(9, 9, 9);
    digest.erase(2, 1, Slot(three));
}
const char* const traffic_digest = "1246648b128aa536a13afb8ecc4d230af0e7bba527665a72a87e5fd01aa38c90";
const char* const empty_digest = "4e8ea0ea8e408acc1db946fd11d81aa2ac13ed904aa1d3c29476057a9b001ea7";

void record_digest_values() {
    eawr::core::RecordDigest digest;
    expect(digest.digest("record-digest-pin") == empty_digest, "empty record digest value is pinned");
    digest_traffic(digest);
    expect(digest.digest("record-digest-pin") == traffic_digest, "record digest value after mixed traffic is pinned");
    expect(digest.byte_size() == 118 && digest.writes() == 15, "record digest byte size and write count are pinned");
    const auto& shards = digest.shards();
    expect(shards.size() == 2 && !shards.contains(3), "an emptied shard disappears");
    expect(shards.at(1) == eawr::core::RecordDigest::Sum{0x328a2e6377c3e42eULL, 0x9ca2e3d2535050bfULL, 2}, "player 1 sums are pinned");
    expect(shards.at(2) == eawr::core::RecordDigest::Sum{0xa9279c0730493b16ULL, 0x4830a2e4566800b2ULL, 3}, "player 2 sums are pinned");
    // A word slot and its eight little-endian bytes name the same record.
    eawr::core::RecordDigest by_word, by_bytes;
    by_word.set(4, 1, 0x0102030405060708ULL, payload(4, 1));
    by_bytes.set(4, 1, std::span<const std::uint8_t>(word_bytes(0x0102030405060708ULL)), payload(4, 1));
    expect(by_word.digest("x") == by_bytes.digest("x"), "word and byte spellings of a slot share a digest");
    by_word.erase(4, 1, std::span<const std::uint8_t>(word_bytes(0x0102030405060708ULL)));
    expect(by_word.shards().empty() && by_word.byte_size() == 0, "a byte-spelled erase removes the word-slot record");
    bool refused = false;
    try { by_bytes.relocate(4, 1, 0x0102030405060708ULL, 0x0102030405060708ULL + 1); by_bytes.relocate(4, 1, 99, 100); }
    catch (const std::logic_error&) { refused = true; }
    expect(refused, "relocating an absent slot is refused");
    // Insertion order never reaches the digest.
    eawr::core::RecordDigest forward, reverse;
    for (unsigned i = 0; i < 6; ++i) forward.set(i % 2, 1, i, payload(i + 1, i));
    for (unsigned i = 6; i-- > 0;) reverse.set(i % 2, 1, i, payload(i + 1, i));
    expect(forward.digest("order") == reverse.digest("order"), "record digest ignores insertion order");
}

// The digest's own cost: updating a record it already holds allocates nothing
// once the thread's framing buffer is warm, and a new record costs one map node.
void record_digest_allocations() {
    eawr::core::RecordDigest digest;
    Bytes buffer;
    const auto rich = rich_cache();
    const Bytes wide_slot{1, 2, 3};
    ai::encode_cache_record(buffer, view_of(rich), rich.value, rich.interval, rich.frame);
    digest.set(3, 1, 7, buffer);
    ai::encode_visibility_record(buffer, visibility_row); digest.set(2, 2, 7, buffer);
    ai::encode_flag_record(buffer, flag_row); digest.set(2, 3, 7, buffer);
    ai::encode_hint_record(buffer, hint_row); digest.set(3, 4, 7, buffer);
    digest.set(3, 4, std::span<const std::uint8_t>(wide_slot), buffer);
    encode_alloc_probe::calls = 0; encode_alloc_probe::armed = true;
    for (int i = 0; i < 100; ++i) {
        ai::encode_cache_record(buffer, view_of(rich), rich.value, rich.interval, rich.frame); digest.set(3, 1, 7, buffer);
        ai::encode_visibility_record(buffer, visibility_row); digest.set(2, 2, 7, buffer);
        ai::encode_flag_record(buffer, flag_row); digest.set(2, 3, 7, buffer);
        ai::encode_hint_record(buffer, hint_row); digest.set(3, 4, 7, buffer);
        digest.set(3, 4, std::span<const std::uint8_t>(wide_slot), buffer);
        digest.erase(8, 8, 8);
    }
    encode_alloc_probe::armed = false;
    expect(encode_alloc_probe::calls == 0, "a warm record update (encode and digest set) allocates nothing");
    encode_alloc_probe::calls = 0; encode_alloc_probe::armed = true;
    for (unsigned i = 0; i < 50; ++i) digest.set(5, 1, 1000 + i, buffer);
    encode_alloc_probe::armed = false;
    expect(encode_alloc_probe::calls <= 51, "a new word-slot record costs one map node (plus the new shard once)");
    encode_alloc_probe::calls = 0; encode_alloc_probe::armed = true;
    for (unsigned i = 0; i < 50; ++i) digest.erase(5, 1, 1000 + i);
    digest.relocate(3, 1, 7, 8); digest.relocate(3, 1, 8, 7);
    encode_alloc_probe::armed = false;
    expect(encode_alloc_probe::calls == 0, "erase and relocation allocate nothing");
}

// Per-thread framing buffers: the same digest from 1/2/4/8 concurrent owners.
void record_digest_workers() {
    for (const unsigned workers : {1U, 2U, 4U, 8U}) {
        std::atomic<unsigned> mismatches{0};
        std::vector<std::thread> threads;
        for (unsigned w = 0; w < workers; ++w) threads.emplace_back([&] {
            for (int i = 0; i < 50; ++i) {
                eawr::core::RecordDigest digest;
                digest_traffic(digest);
                if (digest.digest("record-digest-pin") != traffic_digest) mismatches.fetch_add(1);
            }
        });
        for (auto& thread : threads) thread.join();
        expect(mismatches == 0, "record digest equal on 1/2/4/8 concurrent owners");
    }
}

// Per-thread buffers: the same bytes on 1/2/4/8 concurrent encoders.
void concurrent_workers() {
    const auto rich = rich_cache();
    const auto expected = reference_cache(rich);
    for (const unsigned workers : {1U, 2U, 4U, 8U}) {
        std::atomic<unsigned> mismatches{0};
        std::vector<std::thread> threads;
        for (unsigned w = 0; w < workers; ++w) threads.emplace_back([&] {
            Bytes buffer;
            for (int i = 0; i < 200; ++i) if (fast_cache(rich, buffer) != expected) mismatches.fetch_add(1);
        });
        for (auto& thread : threads) thread.join();
        expect(mismatches == 0, "record bytes equal on 1/2/4/8 concurrent encoders");
    }
}
} // namespace

int main() {
    try {
        equals_reference(); pinned_bytes(); allocations(); concurrent_workers();
        record_digest_values(); record_digest_allocations(); record_digest_workers();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
