#pragma once

// Little-endian fixed-width writer and bounds-checked reader of the script save
// format (#248, docs/lua-persistence.md). The reader never reads past its
// input: an underflow sets a sticky failure and yields zeros, so callers check
// ok() before trusting a count or a length. Decoded elements grow one at a time
// against a budget proportional to the input, so a forged count never drives
// an allocation larger than the bytes behind it.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::script::persist {

// Memory the elements decoded for counts may take per input byte. The densest
// real encoding, a closed nil upvalue (3 bytes for one graph object), needs
// about 60.
inline constexpr std::size_t decode_budget_per_input_byte = 128;

class ByteWriter {
public:
    void u8(std::uint8_t value) { out_.push_back(static_cast<char>(value)); }
    void u32(std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) out_.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
    void u64(std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) out_.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
    void i32(std::int32_t value) { u32(static_cast<std::uint32_t>(value)); }
    void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }
    // u32 length, then the bytes.
    void text(std::string_view value) {
        u32(static_cast<std::uint32_t>(value.size()));
        out_.append(value.data(), value.size());
    }
    void raw(std::string_view value) { out_.append(value.data(), value.size()); }
    // A value the reader would reject: the bytes are not a save.
    void fail() noexcept { failed_ = true; }
    [[nodiscard]] bool ok() const noexcept { return !failed_; }

    [[nodiscard]] std::string& bytes() noexcept { return out_; }

private:
    std::string out_;
    bool failed_{};
};

class ByteReader {
public:
    explicit ByteReader(std::string_view bytes) noexcept
        : bytes_(bytes), budget_(bytes.size() * decode_budget_per_input_byte) {}

    [[nodiscard]] bool ok() const noexcept { return !failed_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - position_; }
    [[nodiscard]] bool at_end() const noexcept { return position_ == bytes_.size(); }
    void fail() noexcept { failed_ = true; }

    std::uint8_t u8() noexcept { return static_cast<std::uint8_t>(little(1)); }
    std::uint32_t u32() noexcept { return static_cast<std::uint32_t>(little(4)); }
    std::uint64_t u64() noexcept { return little(8); }
    std::int32_t i32() noexcept { return static_cast<std::int32_t>(u32()); }
    std::int64_t i64() noexcept { return static_cast<std::int64_t>(u64()); }

    // u32 length, then at most `limit` bytes.
    std::string text(std::size_t limit = SIZE_MAX) {
        const std::uint32_t size = u32();
        if (failed_ || size > limit || size > remaining()) {
            failed_ = true;
            return {};
        }
        std::string out(bytes_.substr(position_, size));
        position_ += size;
        return out;
    }

    // Reads exactly `expected`; fails otherwise.
    void expect(std::string_view expected) noexcept {
        if (failed_ || remaining() < expected.size() || bytes_.substr(position_, expected.size()) != expected) {
            failed_ = true;
            return;
        }
        position_ += expected.size();
    }

    // A count of at most `limit` items that each take at least `min_item_bytes`:
    // fails when it exceeds its limit (a quota or format bound) or the input
    // cannot hold the items.
    std::uint32_t count(std::size_t min_item_bytes, std::uint32_t limit = UINT32_MAX) noexcept {
        const std::uint32_t value = u32();
        if (!failed_ && (value > limit || (min_item_bytes != 0 && value > remaining() / min_item_bytes))) failed_ = true;
        return failed_ ? 0 : value;
    }

    // Takes `size` bytes of decoded memory from the budget; fails when spent.
    bool charge(std::size_t size) noexcept {
        if (failed_ || size > budget_) {
            failed_ = true;
            return false;
        }
        budget_ -= size;
        return true;
    }

    // Appends one default item after charging it: nullptr once the reader has
    // failed or the budget is spent. Items are appended as they decode, never
    // reserved for a count.
    template <class T>
    T* append(std::vector<T>& items) {
        if (!charge(sizeof(T))) return nullptr;
        return &items.emplace_back();
    }

private:
    std::uint64_t little(std::size_t size) noexcept {
        if (failed_ || remaining() < size) {
            failed_ = true;
            return 0;
        }
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < size; ++index) {
            value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes_[position_ + index])) << (8 * index);
        }
        position_ += size;
        return value;
    }

    std::string_view bytes_;
    std::size_t position_{};
    std::size_t budget_;
    bool failed_{};
};

} // namespace eawr::script::persist
