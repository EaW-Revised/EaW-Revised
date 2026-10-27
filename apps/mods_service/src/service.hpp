#pragma once
// The request side of eawr_mods_service: one protocol line in, the response lines out, no I/O
// (docs/mods-service-protocol.md). main.cpp owns the pipes and the transcript file.
#include "model_state.hpp"
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::mods_service {
class LoadCheck;

inline constexpr std::size_t kLineLimit = 1024 * 1024;
inline constexpr int kProtocolMajor = 1;
inline constexpr int kProtocolMinor = 3;

struct ServiceInfo {
    std::string service_version;
    std::string engine_build;
    std::string game_release;
};

class Service {
public:
    using Log = std::function<void(std::string_view)>;

    Service(ServiceInfo info, Log log);
    ~Service();
    LoadCheck& loadcheck();

    // One complete line without its line end. Appends the lines to write (each without LF) to `out`.
    void handle_line(std::string_view line, std::vector<std::string>& out);
    // A line over kLineLimit; `bytes` is its length.
    void handle_oversize(std::size_t bytes, std::vector<std::string>& out);
    // True once `shutdown` was answered: stop reading and exit 0.
    [[nodiscard]] bool finished() const noexcept { return finished_; }

    ModelState model;

    // Session state the method handlers use.
    [[nodiscard]] const ServiceInfo& info() const noexcept { return info_; }
    [[nodiscard]] bool open() const noexcept { return open_; }
    void set_open() noexcept { open_ = true; }
    void set_finished() noexcept { finished_ = true; }
    void log(std::string_view text) const;

private:
    ServiceInfo info_;
    Log log_;
    bool open_ = false;
    bool finished_ = false;
    std::unique_ptr<LoadCheck> loadcheck_;
};

// The implemented method names, sorted (the `capabilities` of `hello`).
[[nodiscard]] std::vector<std::string_view> capabilities();

// The transcript line for one request line (docs/mods-service-protocol.md, section 9).
[[nodiscard]] std::string transcript_request(std::string_view line);
[[nodiscard]] std::string transcript_oversize(std::size_t bytes);
[[nodiscard]] std::string transcript_response(std::string_view line);
[[nodiscard]] std::string transcript_meta();

} // namespace eawr::mods_service
