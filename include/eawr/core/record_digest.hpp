#pragma once

#include "eawr/core/sync_digest.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <vector>

namespace eawr::core {
// Replicated record bytes include their semantic key. The index below is only
// owner-local bookkeeping; neither its layout nor its insertion order is hashed.
class RecordDigest {
public:
    using Bytes = std::vector<std::uint8_t>;
    struct Sum {
        std::uint64_t first{}, second{}, count{};
        bool operator==(const Sum&) const = default;
    };
    using Shards = std::map<std::uint32_t, Sum>;
    void set(std::uint32_t player, std::uint32_t kind, std::uint64_t slot, std::span<const std::uint8_t> bytes) {
        set_at(KeyView::of_word(player, kind, slot), bytes);
    }
    void set(std::uint32_t player, std::uint32_t kind, std::span<const std::uint8_t> slot, std::span<const std::uint8_t> bytes) {
        set_at(KeyView::of_bytes(player, kind, slot), bytes);
    }
    void erase(std::uint32_t player, std::uint32_t kind, std::uint64_t slot) {
        erase_at(KeyView::of_word(player, kind, slot));
    }
    void erase(std::uint32_t player, std::uint32_t kind, std::span<const std::uint8_t> slot) {
        erase_at(KeyView::of_bytes(player, kind, slot));
    }
    [[nodiscard]] std::size_t byte_size() const noexcept { return byte_size_; }
    // Slots are local storage positions, not part of the hashed payload. A
    // stable compaction can move the retained hash without encoding it again.
    void relocate(std::uint32_t player, std::uint32_t kind, std::uint64_t from, std::uint64_t to) {
        if (from == to) return;
        const auto at = records_.find(KeyView::of_word(player, kind, from));
        if (at == records_.end() || records_.contains(KeyView::of_word(player, kind, to)))
            throw std::logic_error("record digest storage relocation is invalid");
        auto node = records_.extract(at);
        node.key() = Key::from(KeyView::of_word(player, kind, to));
        records_.insert(std::move(node));
    }
    [[nodiscard]] const Shards& shards() const noexcept { return shards_; }
    [[nodiscard]] std::uint64_t writes() const noexcept { return writes_; }
    [[nodiscard]] std::string digest(std::string_view domain) const { return digest_of(domain, shards_); }
    // Digests that each hold some owners' records (one per owner shard) combine by owner: the sums of one owner
    // add, and the result equals the digest of a single RecordDigest holding every record.
    static void merge_shards(Shards& into, const Shards& from) {
        for (const auto& [player, sum] : from) {
            auto& total = into[player];
            total.first += sum.first; total.second += sum.second; total.count += sum.count;
        }
    }
    [[nodiscard]] static std::string digest_of(std::string_view domain, const Shards& shards) {
        Bytes bytes;
        bytes.reserve(16 + domain.size() + shards.size() * 32);
        append(bytes, domain.size()); bytes.insert(bytes.end(), domain.begin(), domain.end());
        append(bytes, shards.size());
        for (const auto& [player, sum] : shards) {
            append(bytes, player); append(bytes, sum.count); append(bytes, sum.first); append(bytes, sum.second);
        }
        return sha256_hex(bytes);
    }
    static void append(Bytes& bytes, std::uint64_t value) {
        std::array<std::uint8_t, 8> word;
        for (unsigned i = 0; i < 8; ++i) word[i] = static_cast<std::uint8_t>(value >> (8U * i));
        bytes.insert(bytes.end(), word.begin(), word.end());
    }
private:
    struct Entry { Sum sum; std::size_t bytes{}; };
    // A slot is its little-endian bytes. Eight-byte slots (every word slot) are
    // kept as the word itself, so a lookup or an insert copies nothing; other
    // lengths keep their bytes. The split is canonical: one slot, one form.
    struct KeyView {
        std::uint32_t player{}, kind{};
        bool wide{};
        std::uint64_t word{};
        std::span<const std::uint8_t> bytes;
        static KeyView of_word(std::uint32_t owner, std::uint32_t type, std::uint64_t value) noexcept {
            return {owner, type, false, value, {}};
        }
        static KeyView of_bytes(std::uint32_t owner, std::uint32_t type, std::span<const std::uint8_t> raw) noexcept {
            if (raw.size() != 8) return {owner, type, true, 0, raw};
            std::uint64_t value{};
            for (unsigned i = 0; i < 8; ++i) value |= std::uint64_t(raw[i]) << (8U * i);
            return of_word(owner, type, value);
        }
    };
    struct Key {
        std::uint32_t player{}, kind{};
        bool wide{};
        std::uint64_t word{};
        Bytes bytes;
        static Key from(const KeyView& v) {
            return {v.player, v.kind, v.wide, v.word, Bytes(v.bytes.begin(), v.bytes.end())};
        }
        [[nodiscard]] KeyView view() const noexcept { return {player, kind, wide, word, bytes}; }
    };
    struct KeyLess {
        using is_transparent = void;
        bool operator()(const Key& a, const Key& b) const noexcept { return less(a.view(), b.view()); }
        bool operator()(const Key& a, const KeyView& b) const noexcept { return less(a.view(), b); }
        bool operator()(const KeyView& a, const Key& b) const noexcept { return less(a, b.view()); }
        bool operator()(const KeyView& a, const KeyView& b) const noexcept { return less(a, b); }
    private:
        static bool less(const KeyView& a, const KeyView& b) noexcept {
            if (a.player != b.player) return a.player < b.player;
            if (a.kind != b.kind) return a.kind < b.kind;
            if (a.wide != b.wide) return b.wide;
            if (!a.wide) return a.word < b.word;
            return std::lexicographical_compare(a.bytes.begin(), a.bytes.end(), b.bytes.begin(), b.bytes.end());
        }
    };
    void set_at(const KeyView& key, std::span<const std::uint8_t> bytes) {
        // Each thread keeps its framing buffer: capacity survives between sets,
        // so an update of an existing record allocates nothing.
        thread_local Bytes framed;
        framed.clear();
        append(framed, key.player); append(framed, key.kind);
        framed.insert(framed.end(), bytes.begin(), bytes.end());
        const Entry next{{sync_fingerprint(framed), sync_fingerprint(framed, 0x454157522d53594eULL), 1}, bytes.size()};
        if (framed.capacity() > retained_framing_capacity) Bytes().swap(framed);
        auto at = records_.lower_bound(key);
        const bool inserted = at == records_.end() || records_.key_comp()(key, at->first);
        if (inserted) at = records_.emplace_hint(at, Key::from(key), next);
        auto& sum = shards_[key.player];
        if (!inserted) { sum.first -= at->second.sum.first; sum.second -= at->second.sum.second; --sum.count; byte_size_ -= at->second.bytes; }
        at->second = next;
        sum.first += next.sum.first; sum.second += next.sum.second; ++sum.count; byte_size_ += next.bytes;
        ++writes_;
    }
    void erase_at(const KeyView& key) {
        const auto at = records_.find(key);
        if (at == records_.end()) return;
        auto& sum = shards_.at(key.player);
        sum.first -= at->second.sum.first; sum.second -= at->second.sum.second; --sum.count; byte_size_ -= at->second.bytes;
        if (!sum.count) shards_.erase(key.player);
        records_.erase(at); ++writes_;
    }
    static constexpr std::size_t retained_framing_capacity = std::size_t{1} << 20;
    std::map<Key, Entry, KeyLess> records_;
    std::size_t byte_size_{};
    Shards shards_;
    std::uint64_t writes_{};
};
} // namespace eawr::core
