#pragma once

// An in-memory skirmish::SetupMapFactsStore that counts what a setup list asks of it (#6948). Shared by the space-list and
// land-list tests.

#include "eawr/skirmish/setup_facts.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace setup_facts_test {
// An in-memory SetupMapFactsStore that counts what the list asks of it.
class MemoryFacts final : public eawr::skirmish::SetupMapFactsStore {
public:
    struct Record { eawr::skirmish::SetupFactsStamp stamp; std::vector<std::uint8_t> body; };
    std::optional<std::vector<std::uint8_t>> lookup(std::string_view kind, std::string_view context, std::string_view path,
        const eawr::skirmish::SetupFactsStamp& stamp) override {
        const std::lock_guard lock(mutex);
        ++lookups[std::string(kind)];
        const auto found = records.find(key(kind, context, path));
        if (found == records.end() || !(found->second.stamp == stamp)) return std::nullopt;
        ++hits[std::string(kind)];
        if (corrupt && kind != eawr::skirmish::setup_facts_header_kind) return std::vector<std::uint8_t>{1, 2, 3};
        return found->second.body;
    }
    void put(std::string_view kind, std::string_view context, std::string_view path, const eawr::skirmish::SetupFactsStamp& stamp,
        std::span<const std::uint8_t> body) override {
        const std::lock_guard lock(mutex);
        ++puts[std::string(kind)];
        records[key(kind, context, path)] = Record{stamp, std::vector<std::uint8_t>(body.begin(), body.end())};
    }
    void flush() override { const std::lock_guard lock(mutex); ++flushes; }
    std::size_t total_puts() const { std::size_t total = 0; for (const auto& [kind, count] : puts) { static_cast<void>(kind); total += count; } return total; }
    std::mutex mutex;
    std::map<std::string, Record> records;
    std::map<std::string, std::size_t> lookups, hits, puts;
    std::size_t flushes{};
    bool corrupt{};
    // A view whose context fingerprint differs (a changed XML or text record, another catalog): every non-empty context is
    // suffixed, so entries stored under the old context miss while context-free header facts still hit.
    std::string context_salt;
    // A map file whose stamp changed: every stored record of this kind keeps its old stamp, so the live stamp no longer matches.
    void age_stamps(const std::string_view kind) {
        const std::lock_guard lock(mutex);
        const auto prefix = std::string(kind) + '\n';
        for (auto& [name, record] : records)
            if (name.compare(0, prefix.size(), prefix) == 0) ++record.stamp.container_mtime;
    }
private:
    std::string key(std::string_view kind, std::string_view context, std::string_view path) const {
        return std::string(kind) + '\n' + (context.empty() ? std::string() : std::string(context) + context_salt) + '\n' + std::string(path);
    }
};
} // namespace setup_facts_test
