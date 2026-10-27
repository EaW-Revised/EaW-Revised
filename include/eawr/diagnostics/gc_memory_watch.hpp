#pragma once

#include "eawr/diagnostics/gc_memory.hpp"
#include <functional>
#include <utility>

namespace eawr::diagnostics {
// Diagnostic observers never extend a service input's lifetime. Constructing a
// watch with diagnostics disabled records no callbacks and visits no payloads.
class GcMemoryWatch final {
public:
    template<class T> std::shared_ptr<T> watch(std::string name, std::shared_ptr<T> input) {
        if (GcMemory::enabled()) {
            readers_.emplace_back([name = std::move(name), weak = std::weak_ptr<T>(input)](GcMemory& memory) {
                if (const auto owner = weak.lock()) memory.row(name, owner);
            });
        }
        return input;
    }
    void read(GcMemory& memory) const {
        for (const auto& reader : readers_) reader(memory);
    }
private:
    std::vector<std::function<void(GcMemory&)>> readers_;
};
} // namespace eawr::diagnostics
