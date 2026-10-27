#include "line_framer.hpp"

namespace eawr::mods_service {

bool LineFramer::emit(std::string_view line, const Sink& sink) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.size() > limit_) return sink(FramedLine{true, {}, line.size()});
    return sink(FramedLine{false, line, line.size()});
}

bool LineFramer::emit_oversize(const Sink& sink) {
    const std::size_t bytes = discarded_ - (discarded_cr_ ? 1 : 0);
    discarding_ = false;
    discarded_ = 0;
    discarded_cr_ = false;
    return sink(FramedLine{true, {}, bytes});
}

bool LineFramer::feed(std::string_view chunk, const Sink& sink) {
    while (!chunk.empty()) {
        const std::size_t end = chunk.find('\n');
        const std::string_view head = chunk.substr(0, end);
        if (discarding_) {
            discarded_ += head.size();
            if (!head.empty()) discarded_cr_ = head.back() == '\r';
            if (end == std::string_view::npos) return true;
            if (!emit_oversize(sink)) return false;
        } else if (end == std::string_view::npos) {
            pending_.append(head);
            // limit + 1 leaves room for a CR that may still come before the LF.
            if (pending_.size() > limit_ + 1) {
                discarding_ = true;
                discarded_ = pending_.size();
                discarded_cr_ = pending_.back() == '\r';
                pending_.clear();
                pending_.shrink_to_fit();
            }
            return true;
        } else if (pending_.empty()) {
            if (!emit(head, sink)) return false;
        } else {
            pending_.append(head);
            const std::string line = std::move(pending_);
            pending_.clear();
            if (!emit(line, sink)) return false;
        }
        chunk.remove_prefix(end + 1);
    }
    return true;
}

bool LineFramer::finish(const Sink& sink) {
    if (discarding_) return emit_oversize(sink);
    if (pending_.empty()) return true;
    const std::string line = std::move(pending_);
    pending_.clear();
    return emit(line, sink);
}

} // namespace eawr::mods_service
