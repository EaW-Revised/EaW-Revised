#include "eawr/core/diagnostic.hpp"

#include <sstream>

namespace eawr::core {

std::string format_diagnostic(const Diagnostic& diagnostic) {
    std::ostringstream output;
    output << diagnostic.code << " [" << to_string(diagnostic.severity) << "]";
    if (diagnostic.logical_path) {
        output << ' ' << *diagnostic.logical_path;
        if (diagnostic.line) {
            output << ':' << *diagnostic.line;
            if (diagnostic.column) {
                output << ':' << *diagnostic.column;
            }
        }
    }
    if (diagnostic.source_id) {
        output << " (source " << *diagnostic.source_id << ')';
    }
    output << ": " << diagnostic.message;
    return output.str();
}

} // namespace eawr::core
