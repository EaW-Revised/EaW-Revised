// eawr_mods_service: the mod manager's local service process (docs/mods-service-protocol.md).
// JSON lines on stdin and stdout, a log on stderr. On Windows a GUI-subsystem program, so it never opens a console;
// it reads and writes the pipes its parent hands it.
//
// Usage: eawr_mods_service [--user-data <folder>] [--transcript <file>]
//   --transcript  test-only: also write every exchanged line to <file> (section 9 of the protocol doc).
#include "line_framer.hpp"
#include "service.hpp"
#include "loadcheck.hpp"

#include "eawr/core/build_identity.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <mutex>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

#ifndef EAWR_MODS_SERVICE_VERSION
#error "EAWR_MODS_SERVICE_VERSION must be defined by the build"
#endif

namespace {

namespace fs = std::filesystem;
using eawr::mods_service::FramedLine;

constexpr std::size_t kReadChunk = 64 * 1024;

#ifdef _WIN32
HANDLE std_handle(DWORD which) {
    HANDLE handle = GetStdHandle(which);
    return handle == INVALID_HANDLE_VALUE ? nullptr : handle;
}

bool write_all(HANDLE handle, std::string_view bytes) {
    if (handle == nullptr) return false;
    while (!bytes.empty()) {
        const DWORD part = bytes.size() > 1u << 20 ? 1u << 20 : static_cast<DWORD>(bytes.size());
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data(), part, &written, nullptr) || written == 0) return false;
        bytes.remove_prefix(written);
    }
    return true;
}

// Bytes read, 0 at end of input.
std::size_t read_some(char* buffer, std::size_t size) {
    HANDLE in = std_handle(STD_INPUT_HANDLE);
    if (in == nullptr) return 0;
    const bool pipe = GetFileType(in) == FILE_TYPE_PIPE;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(in, buffer, static_cast<DWORD>(size), &got, nullptr)) return 0; // ERROR_BROKEN_PIPE: closed
        if (got > 0 || !pipe) return got;
        // A zero-byte write on a pipe is not the end of input.
    }
}

bool write_out(std::string_view bytes) { return write_all(std_handle(STD_OUTPUT_HANDLE), bytes); }
void write_err(std::string_view bytes) { (void)write_all(std_handle(STD_ERROR_HANDLE), bytes); }
#else
bool write_fd(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        const ssize_t written = ::write(fd, bytes.data(), bytes.size());
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        bytes.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}

std::size_t read_some(char* buffer, std::size_t size) {
    for (;;) {
        const ssize_t got = ::read(0, buffer, size);
        if (got < 0 && errno == EINTR) continue;
        return got > 0 ? static_cast<std::size_t>(got) : 0;
    }
}

bool write_out(std::string_view bytes) { return write_fd(1, bytes); }
void write_err(std::string_view bytes) { (void)write_fd(2, bytes); }
#endif

void log_line(std::string_view text) {
    // Bound the whole session, including the prefix and LF, below a default Windows pipe buffer.
    constexpr std::size_t kLogBudget = 2048;
    constexpr std::string_view kSuppressed = "eawr_mods_service: log suppressed\n";
    static std::size_t written = 0;
    static bool suppressed = false;
    if (suppressed) return;
    std::string line = "eawr_mods_service: ";
    line += text;
    line += '\n';
    if (line.size() > kLogBudget - kSuppressed.size() - written) {
        write_err(kSuppressed);
        suppressed = true;
        return;
    }
    written += line.size();
    write_err(line);
}

class Transcript {
public:
    bool open(const fs::path& path) {
        file_.open(path, std::ios::binary | std::ios::trunc);
        return static_cast<bool>(file_);
    }
    void write(const std::string& line) {
        if (!file_.is_open()) return;
        file_ << line << '\n';
        file_.flush(); // a killed service still leaves what it exchanged
    }

private:
    std::ofstream file_;
};

int run(const std::vector<fs::path>& args) {
    std::optional<fs::path> transcript_path;
    fs::path user_data;
    fs::path helper;
    for (std::size_t index = 0; index < args.size(); ++index) {
        if (args[index] == fs::path("--transcript") && index + 1 < args.size()) {
            transcript_path = args[++index];
        } else if (args[index] == fs::path("--user-data") && index + 1 < args.size()) {
            user_data = fs::absolute(args[++index]);
        } else if (args[index] == fs::path("--loadcheck-helper") && index + 1 < args.size()) {
            helper = fs::absolute(args[++index]);
        } else {
            log_line("usage: eawr_mods_service [--user-data <folder>] [--transcript <file>]");
            return 2;
        }
    }
    Transcript transcript;
    if (transcript_path) {
        if (!transcript.open(*transcript_path)) {
            log_line("cannot write the transcript file");
            return 2;
        }
        transcript.write(eawr::mods_service::transcript_meta());
    }

    const std::string version = EAWR_MODS_SERVICE_VERSION;
    std::mutex output_mutex;
    std::atomic<bool> pipe_failed{false};
    const auto send = [&](std::string response) {
        std::lock_guard lock(output_mutex);
        transcript.write(eawr::mods_service::transcript_response(response));
        response += '\n';
        if (!write_out(response)) pipe_failed = true;
    };
    eawr::mods_service::Service service(
        {version, std::string(eawr::core::build_identity()), version}, [](std::string_view text) { log_line(text); });
    if (!user_data.empty()) service.model.user_data = user_data;
    service.loadcheck().executable = helper;
    service.loadcheck().emit = send;
    eawr::mods_service::LineFramer framer(eawr::mods_service::kLineLimit);
    std::vector<std::string> out;

    const auto handle = [&](const FramedLine& line) {
        out.clear();
        if (line.oversize) {
            { std::lock_guard lock(output_mutex); transcript.write(eawr::mods_service::transcript_oversize(line.bytes)); }
            service.handle_oversize(line.bytes, out);
        } else {
            if (line.text.find_first_not_of(" \t") != std::string_view::npos) { // blank lines are ignored
                std::lock_guard lock(output_mutex);
                transcript.write(eawr::mods_service::transcript_request(line.text));
            }
            service.handle_line(line.text, out);
        }
        for (const auto& response : out) send(response);
        return !pipe_failed && !service.finished();
    };

    std::vector<char> buffer(kReadChunk);
    for (;;) {
        const std::size_t got = read_some(buffer.data(), buffer.size());
        if (got == 0) {
            framer.finish(handle);
            break;
        }
        if (!framer.feed(std::string_view(buffer.data(), got), handle)) break;
    }
    service.loadcheck().stop();
    if (pipe_failed) {
        log_line("stdout closed; exiting");
        return 1;
    }
    return 0;
}

} // namespace

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<fs::path> args;
    for (int index = 1; argv != nullptr && index < count; ++index) args.emplace_back(argv[index]);
    if (argv != nullptr) LocalFree(argv);
    return run(args);
}
#else
int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN); // a closed stdout becomes a write error, then exit 1
    std::vector<fs::path> args;
    for (int index = 1; index < argc; ++index) args.emplace_back(argv[index]);
    return run(args);
}
#endif
