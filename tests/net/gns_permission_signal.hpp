#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

// Test-only edit of the pinned GNS public protobuf wire schema: rendezvous.reliable_messages (13), ice (1),
// auth (2). Add an unreachable candidate to that same ICE auth message, preserving reliable sequence numbers
// and all opaque crypto bytes. The real relay candidate still arrives normally in a later reliable message.
namespace permission_test {
using Bytes = std::vector<std::uint8_t>;
inline std::uint64_t varint(std::span<const std::uint8_t> input, std::size_t& offset) {
    std::uint64_t result{};
    for (unsigned shift = 0; shift < 64 && offset < input.size(); shift += 7) {
        const auto byte = input[offset++];
        result |= static_cast<std::uint64_t>(byte & 127u) << shift;
        if (!(byte & 128u)) return result;
    }
    throw std::runtime_error("Invalid fixture signal varint");
}
inline void put(Bytes& output, std::uint64_t value) {
    while (value >= 128) { output.push_back(static_cast<std::uint8_t>(value | 128u)); value >>= 7; }
    output.push_back(static_cast<std::uint8_t>(value));
}
inline void field(Bytes& output, unsigned number, std::span<const std::uint8_t> body) {
    put(output, (number << 3) | 2u); put(output, body.size());
    output.insert(output.end(), body.begin(), body.end());
}
inline Bytes inject(std::span<const std::uint8_t> input, unsigned depth = 0) {
    Bytes output;
    bool auth{};
    for (std::size_t offset = 0; offset < input.size();) {
        const auto begin = offset;
        const auto key = varint(input, offset), number = key >> 3, wire = key & 7;
        auth = auth || (depth == 2 && number == 2 && wire == 2);
        if (wire == 2) {
            const auto size = varint(input, offset);
            if (size > input.size() - offset) throw std::runtime_error("Invalid fixture signal field");
            const auto body = input.subspan(offset, static_cast<std::size_t>(size));
            offset += static_cast<std::size_t>(size);
            if ((depth == 0 && number == 13) || (depth == 1 && number == 1)) {
                const auto edited = inject(body, depth + 1);
                field(output, static_cast<unsigned>(number), edited);
                continue;
            }
        } else if (wire == 0) static_cast<void>(varint(input, offset));
        else if (wire == 1) offset += 8;
        else if (wire == 5) offset += 4;
        else throw std::runtime_error("Unsupported fixture signal wire type");
        if (offset > input.size()) throw std::runtime_error("Truncated fixture signal field");
        output.insert(output.end(), input.begin() + static_cast<std::ptrdiff_t>(begin),
            input.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    if (auth) {
        // CGNAT is misclassified as public by this GNS pin, reproducing a denied candidate beside a valid relay IP.
        constexpr std::string_view candidate = "candidate:denied 0 udp 1 100.64.0.1 9 typ host";
        Bytes nested;
        field(nested, 3, {reinterpret_cast<const std::uint8_t*>(candidate.data()), candidate.size()});
        field(output, 1, nested);
    }
    return output;
}
} // namespace permission_test
