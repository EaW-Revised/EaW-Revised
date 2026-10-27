#pragma once

#include "eawr/presentation/godot/renderer.hpp"
#include "viewer_path.hpp"
#include "startup_trace.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <system_error>
#include <cstdint>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/os.hpp>

namespace eawr::presentation::godot_backend {

// #2856: GPU memory samples at the points that tell a load-time footprint
// from growth: the setup screen, the battle's first frame, presented ticks
// 300 / 1800 / 18000 / 54000 (10 s, 1, 10 and 30 minutes at 30 Hz), the
// battle's release and the setup screen after it. Samples are persisted to a
// journal, so a run that never writes its report retains diagnostic history.
// Periodic records use a native file worker; console flushes stay off the frame.
// Engine counters and all Godot calls remain on the main thread.
class GpuMemoryTrace final {
public:
    struct Sample final {
        std::string label;
        double tick{-1.0};
        GodotRenderer::GpuMemory memory;
        double query_ms{}, log_ms{};
        bool console_printed{};
    };
    static constexpr std::array<double, 4> tick_marks{300.0, 1800.0, 18000.0, 54000.0};

    ~GpuMemoryTrace() { shutdown_journal(); }

    void shutdown_journal() {
        {
            std::lock_guard lock(log_mutex_);
            log_stopping_ = true;
        }
        log_ready_.notify_all();
        if (log_writer_.joinable()) log_writer_.join();
    }

    void sample(std::string label, const double tick = -1.0) {
        record(std::move(label), tick, true);
    }
    // A new battle restarts the tick marks.
    void battle_started() { next_mark_ = 0; memory_tick_pending_ = true; }
    // Godot frees released resources a few frames later; sample again once the setup
    // screen has drawn `settle_frames` frames after the battle went.
    static constexpr std::uint32_t settle_frames = 30;
    void battle_released() {
        sample("battle_released");
        settle_ = settle_frames;
    }
    void setup_frame() {
        if (settle_ != 0 && --settle_ == 0) sample("setup_settled");
    }
    void at_tick(const double tick) {
        if (memory_tick_pending_ && tick >= 1200.0) {
            memory_tick_pending_ = false;
            startup_trace.memory_tick_1200();
        }
        while (next_mark_ < tick_marks.size() && tick >= tick_marks[next_mark_]) {
            record("battle_tick_" + std::to_string(static_cast<std::uint64_t>(tick_marks[next_mark_])), tick, false);
            ++next_mark_;
        }
    }
    void write(std::ostream& output) const {
        bool journal_failed;
        {
            std::unique_lock lock(log_mutex_);
            log_ready_.wait(lock, [this] { return log_queue_.empty() && !log_active_; });
            journal_failed = log_failed_;
        }
        output << "  \"gpu_memory\":{\"units\":\"bytes\",\"journal_path\":" << quoted(log_path_text_)
               << ",\"journal_failed\":" << (journal_failed ? "true" : "false") << ",\"current\":"
               << json(Sample{"report", -1.0, GodotRenderer::gpu_memory(), 0.0, 0.0, false}) << ",\"samples\":[";
        for (std::size_t i = 0; i < samples_.size(); ++i) output << (i ? "," : "") << json(samples_[i]);
        output << "]},\n";
    }

private:
    bool memory_tick_pending_{true};
    void ensure_journal() {
        if (!log_path_text_.empty()) return;
        auto* os = godot::OS::get_singleton();
        const auto arguments = os->get_cmdline_user_args();
        for (std::int64_t index = 0; index + 1 < arguments.size(); ++index) {
            if (godot::String(arguments[index]) == godot::String("--eawr-report")) {
                log_path_text_ = std::string(godot::String(arguments[index + 1]).utf8().get_data()) + ".gpu-memory.jsonl";
                break;
            }
        }
        if (log_path_text_.empty()) {
            log_path_text_ = std::string(os->get_user_data_dir().utf8().get_data())
                + "/logs/gpu-memory-" + std::to_string(os->get_process_id()) + ".jsonl";
        }
        log_path_ = ViewerPath(log_path_text_).native();
        godot::UtilityFunctions::print(godot::String::utf8(("EAWR GPU memory journal: " + log_path_text_).c_str()));
        try {
            log_writer_ = std::thread([this] { journal_loop(); });
        } catch (const std::system_error&) {
            std::lock_guard lock(log_mutex_);
            log_failed_ = true;
        }
    }

    void record(std::string label, const double tick, const bool console) {
        if (samples_.size() >= 64) return;
        ensure_journal();
        using Clock = std::chrono::steady_clock;
        const auto query_start = Clock::now();
        auto memory = GodotRenderer::gpu_memory();
        const double query_ms = std::chrono::duration<double, std::milli>(Clock::now() - query_start).count();
        samples_.push_back({std::move(label), tick, std::move(memory), query_ms, 0.0, console});
        const auto log_start = Clock::now();
        const std::string line = json(samples_.back(), false);
        if (log_writer_.joinable()) {
            {
                std::lock_guard lock(log_mutex_);
                if (!log_stopping_) log_queue_.push_back(line);
            }
            log_ready_.notify_one();
        }
        if (console) {
            godot::UtilityFunctions::print(godot::String::utf8(("EAWR gpu memory " + line).c_str()));
        }
        samples_.back().log_ms = std::chrono::duration<double, std::milli>(Clock::now() - log_start).count();
    }

    // Native I/O only: no engine objects, GPU queries, or mutable samples cross
    // the thread boundary. The queue is bounded by the existing 64-record cap.
    void journal_loop() {
        try {
            std::error_code error;
            if (!log_path_.parent_path().empty()) std::filesystem::create_directories(log_path_.parent_path(), error);
            std::ofstream output(log_path_, std::ios::binary | std::ios::trunc);
            {
                std::lock_guard lock(log_mutex_);
                log_failed_ = static_cast<bool>(error) || !output;
            }
            for (;;) {
                std::string line;
                {
                    std::unique_lock lock(log_mutex_);
                    log_ready_.wait(lock, [this] { return log_stopping_ || !log_queue_.empty(); });
                    if (log_queue_.empty()) break;
                    line = std::move(log_queue_.front());
                    log_queue_.pop_front();
                    log_active_ = true;
                }
                output << line << '\n';
                output.flush();
                {
                    std::lock_guard lock(log_mutex_);
                    log_failed_ = log_failed_ || !output;
                    log_active_ = false;
                }
                log_ready_.notify_all();
            }
        } catch (...) {
            {
                std::lock_guard lock(log_mutex_);
                log_failed_ = true;
                log_stopping_ = true;
                log_active_ = false;
                log_queue_.clear();
            }
            log_ready_.notify_all();
        }
    }

    [[nodiscard]] static std::string quoted(const std::string& text) {
        std::string result{"\""};
        for (const char raw : text) {
            const auto c = static_cast<unsigned char>(raw);
            if (c == '"' || c == '\\') result += '\\';
            if (c < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                result += "\\u00";
                result += hex[c >> 4];
                result += hex[c & 15];
            } else result += static_cast<char>(c);
        }
        return result + '"';
    }

    [[nodiscard]] static std::string json(const GodotRenderer::GpuMemoryCategory& item) {
        std::ostringstream output;
        output << "{\"live\":" << item.live << ",\"live_bytes\":" << item.live_bytes << ",\"unique\":" << item.unique
               << ",\"unique_bytes\":" << item.unique_bytes << ",\"duplicate_bytes\":"
               << item.live_bytes - item.unique_bytes << ",\"compressed_bytes\":" << item.compressed_bytes
               << ",\"uncompressed_bytes\":" << item.uncompressed_bytes << ",\"uploads\":" << item.uploads
               << ",\"reuses\":" << item.reuses << '}';
        return output.str();
    }
    [[nodiscard]] static std::string json(const GodotRenderer::TextureBudgetFacts& budget) {
        std::ostringstream output;
        std::string source;
        for (const char c : budget.source) {
            if (c == '"' || c == '\\') source += '\\';
            if (static_cast<unsigned char>(c) >= 0x20) source += c;
        }
        output << "{\"constrained\":" << (budget.constrained ? "true" : "false")
               << ",\"device_bytes\":" << budget.device_bytes << ",\"source\":\"" << source
               << "\",\"texture_budget\":" << budget.texture_budget << ",\"other_bytes\":" << budget.other_bytes
               << ",\"resident_bytes\":"
               << budget.resident_bytes << ",\"mip_drop\":" << budget.drop << ",\"mip_drop_floor\":"
               << budget.minimum_drop << ",\"downsized\":" << budget.downsized << ",\"over_budget\":"
               << budget.over_budget << ",\"rebuild_failures\":" << budget.rebuild_failures << '}';
        return output.str();
    }
    [[nodiscard]] static std::string json(const Sample& sample, const bool timings = true) {
        const GodotRenderer::GpuMemory& memory = sample.memory;
        std::ostringstream output;
        output << "{\"label\":\"" << sample.label << "\",\"tick\":" << sample.tick
               << ",\"device\":{\"textures\":" << memory.device_textures << ",\"buffers\":" << memory.device_buffers
               << ",\"total\":" << memory.device_total << "},\"textures\":" << json(memory.textures)
               << ",\"texture_arrays\":" << json(memory.texture_arrays) << ",\"meshes\":" << json(memory.meshes)
               << ",\"budget\":" << json(memory.budget)
               << ",\"residency\":{\"deferred\":" << memory.deferred << ",\"admissions\":" << memory.admissions
               << ",\"evictions\":" << memory.evictions << ",\"dormant\":" << memory.dormant
               << ",\"admission_failures\":" << memory.admission_failures << "}"
               << ",\"console_printed\":" << (sample.console_printed ? "true" : "false");
        if (timings) output << ",\"cost_ms\":{\"query\":" << sample.query_ms << ",\"log\":" << sample.log_ms << '}';
        output << '}';
        return output.str();
    }

    std::vector<Sample> samples_;
    std::size_t next_mark_{};
    std::uint32_t settle_{};
    std::string log_path_text_;
    std::filesystem::path log_path_;
    mutable std::mutex log_mutex_;
    mutable std::condition_variable log_ready_;
    std::deque<std::string> log_queue_;
    bool log_stopping_{}, log_active_{}, log_failed_{};
    std::thread log_writer_;
};
inline GpuMemoryTrace gpu_memory_trace;

} // namespace eawr::presentation::godot_backend
