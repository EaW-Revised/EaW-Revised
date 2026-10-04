#pragma once

#include <cstddef>
#include <cstdint>

// Scoped extension-owned allocation probe. Rendering-server internals are outside this
// counter; persistent geometry rebuilds have their own deterministic HUD work counter.
namespace eawr::presentation::godot_backend::production_allocation {
inline thread_local std::size_t* counter{};
inline thread_local std::size_t* buffer_work{};
// Packed array storage belongs to Godot, rather than the C++ heap probe. Count its
// construction and writable access separately so a redraw cannot hide a native
// buffer allocation or copy-on-write behind a zero C++ allocation count.
template <typename Array, typename Value>
struct PackedBuffer : Array {
    static void record() noexcept { if (buffer_work) ++*buffer_work; }
    PackedBuffer() { record(); }
    void set(const std::int64_t index, const Value& value) { record(); Array::set(index, value); }
    bool push_back(const Value& value) { record(); return Array::push_back(value); }
    std::int64_t resize(const std::int64_t size) { record(); return Array::resize(size); }
    Value* ptrw() { record(); return Array::ptrw(); }
    Value& operator[](const std::int64_t index) { record(); return Array::operator[](index); }
    const Value& operator[](const std::int64_t index) const { return Array::operator[](index); }
};
struct Scope {
    std::size_t* previous{counter};
    std::size_t* previous_buffer_work{buffer_work};
    Scope(std::size_t& value, std::size_t& buffers) noexcept { counter = &value; buffer_work = &buffers; }
    ~Scope() { counter = previous; buffer_work = previous_buffer_work; }
};
} // namespace eawr::presentation::godot_backend::production_allocation
