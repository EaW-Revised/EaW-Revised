#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eawr::presentation::godot_backend::detail {

struct ParticleSpawnRecord final {
    std::string effect, texture;
    std::uint64_t tick{}, resource{};
    std::size_t emitter{};
    std::array<double, 10> stages{};
    bool texture_cold{}, normal_cold{}, shader_cold{};
};

// Explicit diagnostic only. Records stay in memory until backend teardown, so
// create() performs no output or formatting. Record and text storage are bounded;
// ordinary performance tracing neither enables nor allocates this journal.
class ParticleSpawnJournal final {
public:
    using Sink = std::function<void(std::string_view)>;
    static constexpr std::size_t max_records = 32768;
    static constexpr std::size_t max_text_bytes = 8U * 1024U * 1024U;
    explicit ParticleSpawnJournal(bool enabled = false, Sink sink = {})
        : enabled_(enabled), sink_(std::move(sink)) {
        if (!enabled_) return;
        records_.reserve(max_records);
        if (!sink_) {
            stdout_sink_ = true;
            sink_ = [](const std::string_view text) { std::fwrite(text.data(), 1, text.size(), stdout); };
        }
    }
    ~ParticleSpawnJournal() { flush(); }
    ParticleSpawnJournal(const ParticleSpawnJournal&) = delete;
    ParticleSpawnJournal& operator=(const ParticleSpawnJournal&) = delete;
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] std::size_t records() const noexcept { return records_.size(); }
    [[nodiscard]] std::size_t dropped() const noexcept { return dropped_; }
    void append(ParticleSpawnRecord&& record) {
        if (!enabled_) return;
        const auto effect = record.effect.capacity(), texture = record.texture.capacity();
        if (records_.size() >= max_records || effect > max_text_bytes - 2
            || texture > max_text_bytes - 2 - effect
            || text_bytes_ > max_text_bytes - 2 - effect - texture) {
            ++dropped_;
            return;
        }
        text_bytes_ += effect + texture + 2;
        records_.push_back(std::move(record));
    }
    void flush() {
        if (!enabled_ || (records_.empty() && dropped_ == 0)) return;
        for (const auto& record : records_) {
            std::ostringstream row;
            row << std::setprecision(9) << "EAWR_PARTICLE_SPAWN {\"effect\":" << std::quoted(record.effect)
                << ",\"tick\":" << record.tick << ",\"texture\":" << std::quoted(record.texture)
                << ",\"emitter\":" << record.emitter << ",\"texture_cold\":" << record.texture_cold
                << ",\"normal_cold\":" << record.normal_cold << ",\"shader_cold\":" << record.shader_cold
                << ",\"resource\":" << record.resource << ",\"stages_ms\":[";
            for (std::size_t i = 0; i < record.stages.size(); ++i) row << (i ? "," : "") << record.stages[i];
            row << "]}\n";
            sink_(row.str());
        }
        sink_("EAWR_PARTICLE_SPAWN_SUMMARY {\"records\":" + std::to_string(records_.size())
            + ",\"dropped\":" + std::to_string(dropped_) + "}\n");
        if (stdout_sink_) std::fflush(stdout);
        records_.clear();
        text_bytes_ = 0;
        dropped_ = 0;
    }
private:
    bool enabled_{}, stdout_sink_{};
    Sink sink_;
    std::vector<ParticleSpawnRecord> records_;
    std::size_t text_bytes_{}, dropped_{};
};

// Caller-thread API intervals, not GPU execution times. Cache-miss flags name
// resource creation; they do not count first-draw pipeline compilations.
class ParticleSpawnTrace final {
public:
    enum Stage : std::size_t {
        validation, texture_resolve, texture_setup_upload, normal_resolve,
        normal_setup_upload, shader_compile, material_setup, mesh_instance_create,
        fog_registration, bookkeeping, count
    };
    ParticleSpawnTrace(ParticleSpawnJournal* journal, std::string_view effect, std::uint64_t tick,
        std::string_view texture, std::size_t emitter) : journal_(journal) {
        if (journal_ == nullptr) return;
        record_.effect = effect;
        record_.texture = texture;
        record_.tick = tick;
        record_.emitter = emitter;
        last_ = Clock::now();
    }
    [[nodiscard]] bool enabled() const noexcept { return journal_ != nullptr; }
    void lap(Stage stage) {
        if (journal_ == nullptr) return;
        const auto now = Clock::now();
        record_.stages[stage] += std::chrono::duration<double, std::milli>(now - last_).count();
        last_ = now;
    }
    ~ParticleSpawnTrace() {
        if (journal_ == nullptr) return;
        lap(bookkeeping);
        record_.texture_cold = texture_cold;
        record_.normal_cold = normal_cold;
        record_.shader_cold = shader_cold;
        record_.resource = resource;
        journal_->append(std::move(record_));
    }
    bool texture_cold{}, normal_cold{}, shader_cold{};
    std::uint64_t resource{};
private:
    using Clock = std::chrono::steady_clock;
    ParticleSpawnJournal* journal_;
    ParticleSpawnRecord record_;
    Clock::time_point last_{};
};

} // namespace eawr::presentation::godot_backend::detail
