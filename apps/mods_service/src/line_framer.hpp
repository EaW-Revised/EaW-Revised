#pragma once
// Splits the service's stdin byte stream into protocol lines (docs/mods-service-protocol.md, sections 1 and 5).
// A line ends with LF; one CR before the LF is dropped. A line longer than the limit is never held in memory:
// its bytes are counted and discarded up to the next LF.
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace eawr::mods_service {

struct FramedLine {
    bool oversize = false;
    std::string_view text; // the line without its line end; empty when oversize
    std::size_t bytes = 0; // the line's length without its line end
};

class LineFramer {
public:
    // The sink returns false to stop: the rest of the chunk is left unread.
    using Sink = std::function<bool(const FramedLine&)>;

    explicit LineFramer(std::size_t limit) : limit_(limit) {}

    // Returns false when the sink asked to stop.
    bool feed(std::string_view chunk, const Sink& sink);
    // End of input: a last line without LF still counts as a line.
    bool finish(const Sink& sink);

private:
    bool emit(std::string_view line, const Sink& sink);
    bool emit_oversize(const Sink& sink);

    std::size_t limit_;
    std::string pending_;
    bool discarding_ = false;
    std::size_t discarded_ = 0;
    bool discarded_cr_ = false; // the last discarded byte was a CR
};

} // namespace eawr::mods_service
