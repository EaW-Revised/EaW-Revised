#include "eawr/net/galactic_checkpoint_worker.hpp"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <deque>
#include <thread>
#include <system_error>
#include <utility>

namespace eawr::net {
std::recursive_mutex& checkpoint_io_mutex() { static std::recursive_mutex mutex; return mutex; }
namespace {
std::uint64_t clock_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
core::Diagnostic worker_error(std::string message) {
    core::Diagnostic error; error.code = "EAWR-NET-1300"; error.message = std::move(message); return error;
}
// Lock only individual native operations. Protocol hashes and snapshots never hold this lock.
class NativeAccess final : public MessageTransport {
public:
    explicit NativeAccess(MessageTransport& native) : native_(native) {}
    core::Result<void> send(PeerIndex peer, std::span<const std::uint8_t> bytes) override {
        const std::lock_guard lock(checkpoint_io_mutex()); return native_.send(peer, bytes);
    }
    std::optional<Received> poll() override {
        const std::lock_guard lock(checkpoint_io_mutex()); return native_.poll();
    }
    LinkState link(PeerIndex peer) const override {
        const std::lock_guard lock(checkpoint_io_mutex()); return native_.link(peer);
    }
    PeerIndex self() const override {
        const std::lock_guard lock(checkpoint_io_mutex()); return native_.self();
    }
    std::uint32_t peer_count() const override {
        const std::lock_guard lock(checkpoint_io_mutex()); return native_.peer_count();
    }
private:
    MessageTransport& native_;
};
}
class GalacticCheckpointWorker::Impl {
public:
    GalacticCheckpointTransfer protocol;
    std::jthread thread;
    std::atomic<bool> attached{};
    mutable std::mutex mutex;
    enum class Kind { select, cancel, reset, receive };
    struct Command {
        Kind kind{}; SaveManifest manifest{}; std::shared_ptr<const Bytes> bytes{};
        std::string reason{}; Received frame{};
    };
    struct RestoreRequest {
        SaveManifest manifest; std::shared_ptr<const Bytes> bytes;
        std::optional<core::Result<bool>> completion;
    };
    std::deque<Command> commands;
    std::deque<Received> foreground;
    std::size_t foreground_bytes{};
    std::deque<std::pair<std::string, std::string>> traces;
    GalacticCheckpointTransfer::Trace trace;
    SaveTransferView snapshot;
    std::shared_ptr<const SaveManifest> selected;
    std::shared_ptr<const Bytes> restored;
    std::shared_ptr<RestoreRequest> restore;
    std::optional<core::Diagnostic> error;
    std::optional<std::uint64_t> cleared_epoch;
    std::uint64_t requested{}; // foreground-owned selection deduplication

    Impl() {
        protocol.set_trace([this](std::string_view event, const std::string& detail) {
            const std::lock_guard lock(mutex);
            // Bounded even when the UI is suspended throughout a maximum-size transfer.
            if (traces.size() == 4096) traces.pop_front();
            traces.emplace_back(event, detail);
        });
    }
    void publish() {
        const auto next = protocol.view();
        const auto* manifest = protocol.manifest();
        auto bytes = protocol.restored_bytes();
        const std::lock_guard lock(mutex);
        snapshot = next; restored = std::move(bytes);
        if (restore && (next.refusal || !manifest || restore->manifest.epoch != manifest->epoch)) restore.reset();
        if (!manifest) selected.reset();
        else if (!selected || selected->epoch != manifest->epoch) selected = std::make_shared<const SaveManifest>(*manifest);
    }
    void drain_traces() {
        std::deque<std::pair<std::string, std::string>> pending;
        { const std::lock_guard lock(mutex); pending.swap(traces); }
        if (trace) for (const auto& [event, detail] : pending) trace(event, detail);
    }
    bool enqueue(Command command) {
        const std::lock_guard lock(mutex);
        std::size_t queued_bytes = command.bytes ? command.bytes->size() : command.frame.frame.size();
        for (const auto& queued : commands) queued_bytes += queued.bytes ? queued.bytes->size() : queued.frame.frame.size();
        if (commands.size() >= 64 || queued_bytes > 2 * save_transfer_limit) {
            error = worker_error("Checkpoint command queue is full."); return false;
        }
        commands.push_back(std::move(command)); return true;
    }
    void discard_controls() {
        // New selections/cancellation supersede controls, never already-consumed wire frames.
        const std::lock_guard lock(mutex);
        std::erase_if(commands, [](const Command& command) { return command.kind != Kind::receive; });
    }
    core::Result<bool> request_restore(const SaveManifest& manifest, std::shared_ptr<const Bytes> bytes) {
        const std::lock_guard lock(mutex);
        if (!restore || restore->manifest.epoch != manifest.epoch || restore->bytes != bytes)
            restore = std::make_shared<RestoreRequest>(RestoreRequest{manifest, std::move(bytes), {}});
        return restore->completion ? *restore->completion : core::Result<bool>::success(false);
    }
    void clear_restore() { const std::lock_guard lock(mutex); restore.reset(); }
    void receive_one(MessageTransport& transport, const Received& frame,
        const GalacticCheckpointTransfer::AsyncRestore& callback, std::uint64_t now) {
        const auto result = protocol.receive_async(transport, frame, callback, now);
        if (!result && frame.from == 0 && !frame.frame.empty() && frame.frame.front() == save_cancel_kind
            && result.error().message == "checkpoint transfer: selection_cleared") {
            const auto epoch = protocol.view().epoch;
            clear_restore(); protocol.reset(); // retains the authenticated epoch high-water mark
            const std::lock_guard lock(mutex);
            if (!cleared_epoch || *cleared_epoch < epoch) cleared_epoch = epoch;
        }
    }
    void apply(MessageTransport& transport, Command command,
        const GalacticCheckpointTransfer::AsyncRestore& callback, std::uint64_t now) {
        switch (command.kind) {
        case Kind::select: {
            clear_restore();
            const auto result = protocol.select(transport, std::move(command.manifest), std::move(command.bytes),
                [](const auto&, auto) { return core::Result<void>::success(); }, now);
            if (!result) protocol.cancel(transport, result.error().message);
            break;
        }
        case Kind::cancel: clear_restore(); protocol.cancel(transport, std::move(command.reason)); break;
        case Kind::reset: clear_restore(); protocol.reset(); break;
        case Kind::receive: receive_one(transport, command.frame, callback, now); break;
        }
    }
    void drain_stopped(MessageTransport& transport, const GalacticCheckpointTransfer::AsyncRestore& callback,
        std::uint64_t now) {
        // Preserve the reliable stream's order before a direct receive or pump during handover.
        for (;;) {
            Command command;
            {
                const std::lock_guard lock(mutex);
                if (commands.empty()) break;
                command = std::move(commands.front()); commands.pop_front();
            }
            apply(transport, std::move(command), callback, now);
        }
    }
    void run(MessageTransport& native, std::stop_token stop) {
        NativeAccess transport(native);
        const auto restore_callback = [this](const auto& manifest, auto bytes) {
            return request_restore(manifest, std::move(bytes));
        };
        try {
            while (!stop.stop_requested()) {
                // Leave unprocessed commands in the bounded queue across stop/restart.
                // Once removed, finish that individual command before observing stop.
                for (unsigned i = 0; i < 64 && !stop.stop_requested(); ++i) {
                    Command command;
                    {
                        const std::lock_guard lock(mutex);
                        if (commands.empty()) break;
                        command = std::move(commands.front()); commands.pop_front();
                    }
                    apply(transport, std::move(command), restore_callback, clock_ms());
                }
                // Unrelated messages remain ordered for the foreground owner. Both count and bytes are bounded.
                for (unsigned i = 0; i < 256 && !stop.stop_requested(); ++i) {
                    { const std::lock_guard lock(mutex); if (foreground.size() >= 256 || foreground_bytes >= 8U * 1024U * 1024U) break; }
                    auto frame = transport.poll();
                    if (!frame) break;
                    if (GalacticCheckpointTransfer::handles(frame->frame))
                        receive_one(transport, *frame, restore_callback, clock_ms());
                    else {
                        const std::lock_guard lock(mutex);
                        foreground_bytes += frame->frame.size(); foreground.push_back(std::move(*frame));
                    }
                }
                const auto progress = protocol.view();
                if (progress.epoch && !progress.refusal && !progress.restored && transport.self() != 0
                    && transport.link(0) == LinkState::closed) {
                    clear_restore(); protocol.cancel(transport, "peer_disconnected");
                }
                static_cast<void>(protocol.pump(transport, clock_ms()));
                publish();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            publish();
        } catch (const std::exception&) {
            const std::lock_guard lock(mutex); error = worker_error("Checkpoint worker failed.");
        }
        attached.store(false, std::memory_order_release);
    }
};
GalacticCheckpointWorker::GalacticCheckpointWorker() : impl_(std::make_unique<Impl>()) {}
GalacticCheckpointWorker::~GalacticCheckpointWorker() { stop(); }
void GalacticCheckpointWorker::start(MessageTransport& native) {
    if (running()) return;
    { const std::lock_guard lock(impl_->mutex); impl_->error.reset(); }
    impl_->publish();
    impl_->attached.store(true, std::memory_order_release);
    try { impl_->thread = std::jthread([this, &native](std::stop_token stop) { impl_->run(native, stop); }); }
    catch (const std::system_error&) {
        impl_->attached.store(false, std::memory_order_release);
        const std::lock_guard lock(impl_->mutex); impl_->error = worker_error("Could not start checkpoint worker.");
    }
}
void GalacticCheckpointWorker::stop() {
    if (impl_->thread.joinable()) { impl_->thread.request_stop(); impl_->thread.join(); }
    impl_->attached.store(false, std::memory_order_release);
}
bool GalacticCheckpointWorker::running() const { return impl_->attached.load(std::memory_order_acquire); }
std::optional<Received> GalacticCheckpointWorker::poll_foreground() {
    const std::lock_guard lock(impl_->mutex);
    if (impl_->foreground.empty()) return {};
    auto frame = std::move(impl_->foreground.front()); impl_->foreground.pop_front();
    impl_->foreground_bytes -= frame.frame.size(); return frame;
}
std::optional<std::uint64_t> GalacticCheckpointWorker::take_cleared_epoch() {
    const std::lock_guard lock(impl_->mutex); return std::exchange(impl_->cleared_epoch, {});
}
bool GalacticCheckpointWorker::pending_from(PeerIndex peer) const {
    const std::lock_guard lock(impl_->mutex);
    for (const auto& frame : impl_->foreground) if (frame.from == peer) return true;
    return false;
}
std::optional<core::Diagnostic> GalacticCheckpointWorker::failure() const {
    const std::lock_guard lock(impl_->mutex); return impl_->error;
}
core::Result<void> GalacticCheckpointWorker::select(MessageTransport& transport, SaveManifest manifest,
    std::shared_ptr<const Bytes> bytes, GalacticCheckpointTransfer::Restore restore, std::uint64_t now) {
    if (const auto error = failure()) return core::Result<void>::failure(*error);
    if (!running()) {
        impl_->discard_controls();
        auto result = impl_->protocol.select(transport, std::move(manifest), std::move(bytes), std::move(restore), now);
        impl_->requested = impl_->protocol.view().epoch; impl_->publish(); return result;
    }
    if (impl_->requested == manifest.epoch) return core::Result<void>::success();
    if (!bytes || bytes->size() > save_transfer_limit || !restore || manifest.epoch < impl_->requested) return core::Result<void>::failure(worker_error("Invalid checkpoint selection."));
    if (auto result = restore(manifest, *bytes); !result) return result;
    const auto epoch = manifest.epoch;
    Impl::Command command; command.kind = Impl::Kind::select; command.manifest = std::move(manifest); command.bytes = std::move(bytes);
    if (!impl_->enqueue(std::move(command))) return core::Result<void>::failure(*failure());
    impl_->requested = epoch; return core::Result<void>::success();
}
core::Result<void> GalacticCheckpointWorker::receive_async(MessageTransport& transport, const Received& frame,
    GalacticCheckpointTransfer::AsyncRestore restore, std::uint64_t now) {
    if (const auto error = failure()) return core::Result<void>::failure(*error);
    if (!running()) {
        impl_->drain_stopped(transport, restore, now);
        auto result = impl_->protocol.receive_async(transport, frame, std::move(restore), now);
        impl_->requested = impl_->protocol.view().epoch; impl_->publish(); impl_->drain_traces(); return result;
    }
    Impl::Command command; command.kind = Impl::Kind::receive; command.frame = frame;
    if (!impl_->enqueue(std::move(command))) return core::Result<void>::failure(*failure());
    service_restore(restore); return core::Result<void>::success();
}
core::Result<void> GalacticCheckpointWorker::pump(MessageTransport& transport, std::uint64_t now) {
    impl_->drain_traces();
    if (const auto error = failure()) return core::Result<void>::failure(*error);
    if (!running()) {
        impl_->drain_stopped(transport, [this](const auto& manifest, auto bytes) {
            return impl_->request_restore(manifest, std::move(bytes));
        }, now);
        auto result = impl_->protocol.pump(transport, now);
        impl_->requested = impl_->protocol.view().epoch; impl_->publish(); return result;
    }
    const auto progress = view();
    return progress.refusal ? core::Result<void>::failure(*progress.refusal) : core::Result<void>::success();
}
void GalacticCheckpointWorker::service_restore(const GalacticCheckpointTransfer::AsyncRestore& callback) {
    if (!running()) return;
    std::shared_ptr<Impl::RestoreRequest> request;
    {
        const std::lock_guard lock(impl_->mutex); request = impl_->restore;
        if (!request || request->completion) return;
    }
    auto result = callback(request->manifest, request->bytes);
    if (!result || result.value()) {
        const std::lock_guard lock(impl_->mutex);
        if (impl_->restore == request) request->completion = std::move(result);
    }
}
void GalacticCheckpointWorker::cancel(MessageTransport& transport, std::string reason) {
    if (failure()) return;
    if (!running()) {
        impl_->discard_controls(); impl_->protocol.cancel(transport, std::move(reason));
        impl_->requested = impl_->protocol.view().epoch; impl_->publish(); return;
    }
    Impl::Command command; command.kind = Impl::Kind::cancel; command.reason = std::move(reason);
    static_cast<void>(impl_->enqueue(std::move(command)));
}
void GalacticCheckpointWorker::reset() {
    impl_->requested = 0;
    if (!running()) {
        impl_->discard_controls(); impl_->clear_restore();
        impl_->protocol.reset(); impl_->publish(); return;
    }
    Impl::Command command; command.kind = Impl::Kind::reset; static_cast<void>(impl_->enqueue(std::move(command)));
}
void GalacticCheckpointWorker::set_trace(GalacticCheckpointTransfer::Trace trace) { impl_->trace = std::move(trace); }
SaveTransferView GalacticCheckpointWorker::view() const {
    if (!running()) return impl_->protocol.view();
    const std::lock_guard lock(impl_->mutex); return impl_->snapshot;
}
std::uint64_t GalacticCheckpointWorker::requested_epoch() const { return running() ? impl_->requested : impl_->protocol.view().epoch; }
std::shared_ptr<const SaveManifest> GalacticCheckpointWorker::manifest() const {
    const std::lock_guard lock(impl_->mutex); return impl_->selected;
}
std::shared_ptr<const GalacticCheckpointWorker::Bytes> GalacticCheckpointWorker::restored_bytes() const noexcept {
    if (!running()) return impl_->protocol.restored_bytes();
    const std::lock_guard lock(impl_->mutex); return impl_->restored;
}
} // namespace eawr::net
