#pragma once

#include "eawr/sim/math/fixed.hpp"

#include <bit>
#include <array>
#include <concepts>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <tuple>
#include <vector>

namespace eawr::sim::tactical::checkpoint_detail {

inline constexpr std::size_t byte_limit = 64U * 1024U * 1024U;
struct Invalid : std::runtime_error { using std::runtime_error::runtime_error; };
struct Access;

// Explicit field lists below define the wire format; object memory and pointers never do.
template <class T> struct Fields;

class Writer {
public:
    static constexpr bool reading = false;
    std::vector<std::uint8_t> bytes;
    template <class... T> void operator()(const T&... values) { (value(values), ...); }
    template <std::integral T> void value(T input) {
        if (bytes.size() > byte_limit - sizeof(T)) throw Invalid("state exceeds byte budget");
        if constexpr (std::same_as<T, bool>) bytes.push_back(input ? 1U : 0U);
        else {
            auto bits = static_cast<std::uint64_t>(static_cast<std::make_unsigned_t<T>>(input));
            for (std::size_t i = 0; i < sizeof(T); ++i) {
                bytes.push_back(static_cast<std::uint8_t>(bits & 255U));
                bits >>= 8U;
            }
        }
    }
    template <class T> requires std::is_enum_v<T> void value(T input) { value(static_cast<std::underlying_type_t<T>>(input)); }
    void value(math::Fixed input) { value(input.raw()); }
    template <class T> void value(const std::optional<T>& input) { value(input.has_value()); if (input) value(*input); }
    template <class T> void value(const std::shared_ptr<T>& input) { value(static_cast<bool>(input)); if (input) value(*input); }
    template <class... T> void value(const std::tuple<T...>& input) { std::apply([this](const auto&... v) { (*this)(v...); }, input); }
    template <class A, class B> void value(const std::pair<A, B>& input) { (*this)(input.first, input.second); }
    template <class T, std::size_t N> void value(const std::array<T, N>& input) { for (const auto& v : input) value(v); }
    template <class T, std::size_t N> void value(const T (&input)[N]) { for (const auto& v : input) value(v); }
    template <class T> void value(const std::vector<T>& input) { value(static_cast<std::uint64_t>(input.size())); for (const auto& v : input) value(v); }
    template <class K, class V> void value(const std::map<K, V>& input) { value(static_cast<std::uint64_t>(input.size())); for (const auto& v : input) value(v); }
    template <class T> void value(const std::set<T>& input) { value(static_cast<std::uint64_t>(input.size())); for (const auto& v : input) value(v); }
    template <class... T> void value(const std::variant<T...>& input) { value(static_cast<std::uint32_t>(input.index())); std::visit([this](const auto& v) { value(v); }, input); }
    void value(const std::string& input) { value(static_cast<std::uint64_t>(input.size())); for (const auto c : input) value(static_cast<std::uint8_t>(c)); }
    template <class T> requires (!std::integral<T> && !std::is_enum_v<T>) void value(const T& input) { Fields<T>::apply(*this, input); }
};

class Reader {
public:
    static constexpr bool reading = true;
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) { if (bytes.size() > byte_limit) throw Invalid("state exceeds byte budget"); }
    template <class... T> void operator()(T&... values) { (value(values), ...); }
    [[nodiscard]] std::size_t remaining() const { return bytes_.size() - cursor_; }
    template <std::integral T> void value(T& output) {
        if (remaining() < sizeof(T)) throw Invalid("truncated state field");
        if constexpr (std::same_as<T, bool>) {
            const auto bit = bytes_[cursor_++];
            if (bit > 1U) throw Invalid("noncanonical boolean");
            output = bit != 0U;
        } else {
            std::uint64_t bits = 0;
            for (std::size_t i = 0; i < sizeof(T); ++i) bits |= static_cast<std::uint64_t>(bytes_[cursor_++]) << (8U * i);
            output = std::bit_cast<T>(static_cast<std::make_unsigned_t<T>>(bits));
        }
    }
    template <class T> requires std::is_enum_v<T> void value(T& output) { std::underlying_type_t<T> bits{}; value(bits); output = static_cast<T>(bits); }
    void value(math::Fixed& output) { std::int64_t bits{}; value(bits); output = math::Fixed::from_raw(bits); }
    template <class T> void value(std::optional<T>& output) { bool present{}; value(present); if (present) { output.emplace(T{}); value(*output); } else output.reset(); }
    template <class T> void value(std::shared_ptr<T>& output) { bool present{}; value(present); if (present) { allocation(1U, sizeof(T) + 32U); auto v = std::make_shared<std::remove_const_t<T>>(); value(*v); output = std::move(v); } else output.reset(); }
    template <class... T> void value(std::tuple<T...>& output) { std::apply([this](auto&... v) { (*this)(v...); }, output); }
    template <class A, class B> void value(std::pair<A, B>& output) { (*this)(output.first, output.second); }
    template <class T, std::size_t N> void value(std::array<T, N>& output) { for (auto& v : output) value(v); }
    template <class T, std::size_t N> void value(T (&output)[N]) { for (auto& v : output) value(v); }
    std::uint64_t count() { std::uint64_t n{}; value(n); if (n > remaining() || n > byte_limit / 8U) throw Invalid("state count exceeds remaining byte budget"); return n; }
    template <class T> void value(std::vector<T>& output) { const auto n = count(); allocation(n, sizeof(T)); output.clear(); for (std::uint64_t i = 0; i < n; ++i) { T v{}; value(v); output.push_back(std::move(v)); } }
    template <class K, class V> void value(std::map<K, V>& output) { const auto n = count(); allocation(n, sizeof(K) + sizeof(V) + 32U); output.clear(); for (std::uint64_t i = 0; i < n; ++i) { K k{}; V v{}; (*this)(k, v); if (!output.empty() && !(output.rbegin()->first < k)) throw Invalid("unordered state map"); output.emplace(std::move(k), std::move(v)); } }
    template <class T> void value(std::set<T>& output) { const auto n = count(); allocation(n, sizeof(T) + 32U); output.clear(); for (std::uint64_t i = 0; i < n; ++i) { T v{}; value(v); if (!output.empty() && !(*output.rbegin() < v)) throw Invalid("unordered state set"); output.insert(std::move(v)); } }
    template <std::size_t I = 0, class... T> void alternative(std::uint32_t tag, std::variant<T...>& output) { if constexpr (I < sizeof...(T)) { if (tag == I) { output.template emplace<I>(); value(std::get<I>(output)); } else alternative<I + 1>(tag, output); } else throw Invalid("unknown state variant"); }
    template <class... T> void value(std::variant<T...>& output) { std::uint32_t tag{}; value(tag); alternative(tag, output); }
    void value(std::string& output) { const auto n = count(); allocation(n, sizeof(char)); output.clear(); for (std::uint64_t i = 0; i < n; ++i) { std::uint8_t c{}; value(c); output.push_back(static_cast<char>(c)); } }
    template <class T> requires (!std::integral<T> && !std::is_enum_v<T>) void value(T& output) {
        if (++depth_ > 64U) throw Invalid("state nesting exceeds depth budget");
        Fields<T>::apply(*this, output);
        --depth_;
    }
private:
    void allocation(std::uint64_t count, std::size_t width) {
        if (count > (4U * byte_limit - allocated_) / width) throw Invalid("decoded state exceeds memory budget");
        allocated_ += static_cast<std::size_t>(count) * width;
    }
    std::span<const std::uint8_t> bytes_;
    std::size_t cursor_{};
    std::size_t allocated_{}, depth_{};
};

} // namespace eawr::sim::tactical::checkpoint_detail
