#include "eawr/script/pglua.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

namespace eawr::script {
namespace {

constexpr std::size_t max_input = 64U * 1024U * 1024U;
constexpr std::size_t max_string = 16U * 1024U * 1024U;
constexpr std::size_t max_depth = 128U;
constexpr std::size_t max_entries = 1'000'000U;
constexpr std::array<std::byte, 22> pglua_header{
    std::byte{0x1b}, std::byte{0x4c}, std::byte{0x75}, std::byte{0x70},
    std::byte{0x51}, std::byte{0x01}, std::byte{0x04}, std::byte{0x04},
    std::byte{0x04}, std::byte{0x06}, std::byte{0x08}, std::byte{0x09},
    std::byte{0x09}, std::byte{0x08}, std::byte{0xb6}, std::byte{0x09},
    std::byte{0x93}, std::byte{0x68}, std::byte{0xe7}, std::byte{0xf5},
    std::byte{0x7d}, std::byte{0x41},
};

struct StoredString {
    bool present{false};
    std::string bytes;
};

struct Local {
    StoredString name;
    std::int32_t start_pc{0};
    std::int32_t end_pc{0};
};

struct Constant {
    std::uint8_t tag{0};
    double number{0.0};
    StoredString string;
};

struct Prototype {
    StoredString source;
    std::int32_t first_line{0};
    std::int32_t persistence_id{0};
    std::uint8_t upvalues{0};
    std::uint8_t parameters{0};
    std::uint8_t vararg{0};
    std::uint8_t max_stack{0};
    std::vector<std::int32_t> lines;
    std::vector<Local> locals;
    std::vector<StoredString> upvalue_names;
    std::vector<Constant> constants;
    std::vector<Prototype> nested;
    std::vector<std::uint32_t> code;
};

[[nodiscard]] core::Diagnostic diagnostic(
    const std::string_view code,
    const std::string& source,
    const std::size_t offset,
    std::string message
) {
    core::Diagnostic result;
    result.code = std::string(code);
    result.message = std::move(message);
    result.logical_path = source;
    result.column = static_cast<std::uint64_t>(offset);
    return result;
}

class Decoder final {
public:
    Decoder(std::span<const std::byte> bytes, std::string source)
        : bytes_(bytes), source_(std::move(source)) {}

    [[nodiscard]] core::Result<ConvertedChunk> run() {
        if (bytes_.size() > max_input) {
            return core::Result<ConvertedChunk>::failure(
                diagnostic(diagnostic_codes::resource_limit, source_, 0, "PGLua input exceeds 64 MiB")
            );
        }
        if (bytes_.size() < pglua_header.size() ||
            !std::equal(pglua_header.begin(), pglua_header.end(), bytes_.begin())) {
            return core::Result<ConvertedChunk>::failure(
                diagnostic(diagnostic_codes::load_parse, source_, 0, "invalid pinned PGLua header")
            );
        }
        offset_ = pglua_header.size();
        auto root = read_prototype({}, 0);
        if (!root) {
            return core::Result<ConvertedChunk>::failure(std::move(root).error());
        }
        if (offset_ != bytes_.size()) {
            return core::Result<ConvertedChunk>::failure(
                diagnostic(diagnostic_codes::load_parse, source_, offset_, "trailing data after root prototype")
            );
        }

        ConvertedChunk result;
        emit_header(result.bytes);
        emit_prototype(root.value(), result.bytes);
        std::sort(prototype_info_.begin(), prototype_info_.end(), [](const auto& left, const auto& right) {
            return left.persistence_id < right.persistence_id;
        });
        result.prototypes = std::move(prototype_info_);
        result.unsupported_execution = std::move(unsupported_execution_);
        return core::Result<ConvertedChunk>::success(std::move(result));
    }

private:
    template <typename T>
    [[nodiscard]] core::Result<T> fail(const std::size_t at, std::string message) const {
        return core::Result<T>::failure(
            diagnostic(diagnostic_codes::load_parse, source_, at, std::move(message))
        );
    }

    [[nodiscard]] bool available(const std::size_t count) const noexcept {
        return count <= bytes_.size() - offset_;
    }

    [[nodiscard]] core::Result<std::uint8_t> read_u8() {
        if (!available(1)) {
            return fail<std::uint8_t>(offset_, "truncated byte field");
        }
        return core::Result<std::uint8_t>::success(std::to_integer<std::uint8_t>(bytes_[offset_++]));
    }

    [[nodiscard]] core::Result<std::uint32_t> read_u32() {
        const auto start = offset_;
        if (!available(4)) {
            return fail<std::uint32_t>(start, "truncated 32-bit field");
        }
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes_[offset_++])) << shift;
        }
        return core::Result<std::uint32_t>::success(value);
    }

    [[nodiscard]] core::Result<std::int32_t> read_i32() {
        auto value = read_u32();
        if (!value) {
            return core::Result<std::int32_t>::failure(std::move(value).error());
        }
        return core::Result<std::int32_t>::success(std::bit_cast<std::int32_t>(value.value()));
    }

    [[nodiscard]] core::Result<double> read_number() {
        const auto start = offset_;
        if (!available(sizeof(double))) {
            return fail<double>(start, "truncated binary64 constant");
        }
        std::uint64_t bits = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) {
            bits |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes_[offset_++])) << shift;
        }
        return core::Result<double>::success(std::bit_cast<double>(bits));
    }

    [[nodiscard]] core::Result<StoredString> read_string() {
        const auto start = offset_;
        auto count_result = read_u32();
        if (!count_result) {
            return core::Result<StoredString>::failure(std::move(count_result).error());
        }
        const auto count = static_cast<std::size_t>(count_result.value());
        if (count == 0) {
            return core::Result<StoredString>::success({});
        }
        if (count > max_string) {
            return core::Result<StoredString>::failure(
                diagnostic(diagnostic_codes::resource_limit, source_, start, "PGLua string exceeds 16 MiB")
            );
        }
        if (!available(count)) {
            return fail<StoredString>(start, "truncated PGLua string");
        }
        if (bytes_[offset_ + count - 1] != std::byte{0}) {
            return fail<StoredString>(start, "PGLua string is missing its trailing zero") ;
        }
        StoredString result;
        result.present = true;
        result.bytes.resize(count - 1);
        std::memcpy(result.bytes.data(), bytes_.data() + offset_, count - 1);
        offset_ += count;
        return core::Result<StoredString>::success(std::move(result));
    }

    [[nodiscard]] core::Result<std::size_t> read_count(
        std::size_t& aggregate,
        const std::string_view field
    ) {
        const auto start = offset_;
        auto signed_count = read_i32();
        if (!signed_count) {
            return core::Result<std::size_t>::failure(std::move(signed_count).error());
        }
        if (signed_count.value() < 0) {
            return fail<std::size_t>(start, std::string(field) + " count is negative");
        }
        const auto count = static_cast<std::size_t>(signed_count.value());
        if (count > max_entries || aggregate > max_entries - count) {
            return core::Result<std::size_t>::failure(
                diagnostic(
                    diagnostic_codes::resource_limit,
                    source_,
                    start,
                    std::string(field) + " aggregate entry limit exceeded"
                )
            );
        }
        aggregate += count;
        return core::Result<std::size_t>::success(count);
    }

    [[nodiscard]] core::Result<Prototype> read_prototype(
        std::vector<std::uint32_t> path,
        const std::size_t depth
    ) {
        if (depth > max_depth) {
            return core::Result<Prototype>::failure(
                diagnostic(diagnostic_codes::resource_limit, source_, offset_, "prototype depth exceeds 128")
            );
        }
        Prototype result;
        auto source = read_string();
        auto first_line = read_i32();
        auto persistence_id = read_i32();
        auto upvalues = read_u8();
        auto parameters = read_u8();
        auto vararg = read_u8();
        auto max_stack = read_u8();
        if (!source || !first_line || !persistence_id || !upvalues || !parameters || !vararg || !max_stack) {
            const core::Diagnostic* error = nullptr;
            if (!source) error = &source.error();
            else if (!first_line) error = &first_line.error();
            else if (!persistence_id) error = &persistence_id.error();
            else if (!upvalues) error = &upvalues.error();
            else if (!parameters) error = &parameters.error();
            else if (!vararg) error = &vararg.error();
            else error = &max_stack.error();
            return core::Result<Prototype>::failure(*error);
        }
        if (persistence_id.value() <= 0 || persistence_id.value() != next_persistence_id_) {
            return fail<Prototype>(offset_ - 8, "persistence identifier is not the positive depth-first sequence");
        }
        ++next_persistence_id_;
        result.source = std::move(source).value();
        result.first_line = first_line.value();
        result.persistence_id = persistence_id.value();
        result.upvalues = upvalues.value();
        result.parameters = parameters.value();
        result.vararg = vararg.value();
        result.max_stack = max_stack.value();
        if (result.max_stack < 2) {
            return fail<Prototype>(offset_ - 1, "prototype maximum stack is below Lua 5.0.2 minimum") ;
        }

        auto line_count = read_count(line_entries_, "line-info");
        if (!line_count) return core::Result<Prototype>::failure(std::move(line_count).error());
        result.lines.reserve(line_count.value());
        for (std::size_t i = 0; i < line_count.value(); ++i) {
            auto value = read_i32();
            if (!value) return core::Result<Prototype>::failure(std::move(value).error());
            if (value.value() < 0) return fail<Prototype>(offset_ - 4, "negative source line");
            result.lines.push_back(value.value());
        }

        auto local_count = read_count(local_entries_, "local-variable");
        if (!local_count) return core::Result<Prototype>::failure(std::move(local_count).error());
        result.locals.reserve(local_count.value());
        for (std::size_t i = 0; i < local_count.value(); ++i) {
            auto name = read_string();
            auto start = read_i32();
            auto end = read_i32();
            if (!name || !start || !end) {
                if (!name) return core::Result<Prototype>::failure(std::move(name).error());
                if (!start) return core::Result<Prototype>::failure(std::move(start).error());
                return core::Result<Prototype>::failure(std::move(end).error());
            }
            if (start.value() < 0 || end.value() < start.value()) {
                return fail<Prototype>(offset_ - 8, "invalid local-variable PC range");
            }
            result.locals.push_back({std::move(name).value(), start.value(), end.value()});
        }

        auto upvalue_count = read_count(upvalue_name_entries_, "upvalue-name");
        if (!upvalue_count) return core::Result<Prototype>::failure(std::move(upvalue_count).error());
        result.upvalue_names.reserve(upvalue_count.value());
        for (std::size_t i = 0; i < upvalue_count.value(); ++i) {
            auto name = read_string();
            if (!name) return core::Result<Prototype>::failure(std::move(name).error());
            result.upvalue_names.push_back(std::move(name).value());
        }

        auto constant_count = read_count(constant_entries_, "constant");
        if (!constant_count) return core::Result<Prototype>::failure(std::move(constant_count).error());
        result.constants.reserve(constant_count.value());
        for (std::size_t i = 0; i < constant_count.value(); ++i) {
            auto tag = read_u8();
            if (!tag) return core::Result<Prototype>::failure(std::move(tag).error());
            Constant constant;
            constant.tag = tag.value();
            if (constant.tag == 3) {
                auto number = read_number();
                if (!number) return core::Result<Prototype>::failure(std::move(number).error());
                constant.number = number.value();
            } else if (constant.tag == 4) {
                auto string = read_string();
                if (!string) return core::Result<Prototype>::failure(std::move(string).error());
                constant.string = std::move(string).value();
            } else if (constant.tag != 0) {
                return fail<Prototype>(offset_ - 1, "unknown PGLua constant tag");
            }
            result.constants.push_back(std::move(constant));
        }

        auto nested_count = read_count(prototype_entries_, "prototype");
        if (!nested_count) return core::Result<Prototype>::failure(std::move(nested_count).error());
        result.nested.reserve(nested_count.value());
        for (std::size_t i = 0; i < nested_count.value(); ++i) {
            auto child_path = path;
            child_path.push_back(static_cast<std::uint32_t>(i));
            auto child = read_prototype(std::move(child_path), depth + 1);
            if (!child) return core::Result<Prototype>::failure(std::move(child).error());
            result.nested.push_back(std::move(child).value());
        }

        auto instruction_count = read_count(instruction_entries_, "instruction");
        if (!instruction_count) return core::Result<Prototype>::failure(std::move(instruction_count).error());
        result.code.reserve(instruction_count.value());
        for (std::size_t i = 0; i < instruction_count.value(); ++i) {
            auto instruction = read_u32();
            if (!instruction) return core::Result<Prototype>::failure(std::move(instruction).error());
            result.code.push_back(instruction.value());
        }
        auto valid = validate_code(result);
        if (!valid) return core::Result<Prototype>::failure(std::move(valid).error());
        prototype_info_.push_back({
            result.persistence_id,
            std::move(path),
            static_cast<std::uint32_t>(result.code.size()),
        });
        return core::Result<Prototype>::success(std::move(result));
    }

    [[nodiscard]] core::Result<void> validate_code(const Prototype& prototype) {
        if (prototype.code.empty() || (prototype.code.back() & 0x3fU) != 27U) {
            return core::Result<void>::failure(
                diagnostic(diagnostic_codes::load_parse, source_, offset_, "prototype does not end in RETURN")
            );
        }
        const auto register_ok = [&](const std::uint32_t value) {
            return value < prototype.max_stack;
        };
        const auto rk_ok = [&](const std::uint32_t value) {
            return value < 250U ? register_ok(value) : value - 250U < prototype.constants.size();
        };
        for (std::size_t pc = 0; pc < prototype.code.size(); ++pc) {
            const auto word = prototype.code[pc];
            const auto opcode = word & 0x3fU;
            const auto a = word >> 24U;
            const auto c = (word >> 6U) & 0x1ffU;
            const auto b = (word >> 15U) & 0x1ffU;
            const auto bx = (word >> 6U) & 0x3ffffU;
            if (opcode > 34U) {
                return core::Result<void>::failure(
                    diagnostic(diagnostic_codes::unsupported_feature, source_, pc, "unknown PGLua opcode")
                );
            }
            if (opcode == 28U || opcode == 32U) {
                unsupported_execution_.push_back({
                    prototype.persistence_id,
                    static_cast<std::uint32_t>(pc),
                });
            }
            if (opcode != 20U && opcode != 21U && opcode != 22U && opcode != 23U &&
                !register_ok(a)) {
                return core::Result<void>::failure(
                    diagnostic(diagnostic_codes::load_parse, source_, pc, "instruction A register is out of range")
                );
            }
            if ((opcode == 1U || opcode == 5U || opcode == 7U) && bx >= prototype.constants.size()) {
                return core::Result<void>::failure(
                    diagnostic(diagnostic_codes::load_parse, source_, pc, "instruction constant index is out of range")
                );
            }
            if (opcode == 34U && bx >= prototype.nested.size()) {
                return core::Result<void>::failure(
                    diagnostic(diagnostic_codes::load_parse, source_, pc, "instruction prototype index is out of range")
                );
            }
            if ((opcode == 6U && (!register_ok(b) || !rk_ok(c))) ||
                (opcode == 9U && (!rk_ok(b) || !rk_ok(c))) ||
                ((opcode >= 12U && opcode <= 15U) && (!rk_ok(b) || !rk_ok(c))) ||
                ((opcode >= 21U && opcode <= 23U) && (!rk_ok(b) || !rk_ok(c)))) {
                return core::Result<void>::failure(
                    diagnostic(diagnostic_codes::load_parse, source_, pc, "instruction register/constant operand is out of range")
                );
            }
            if (opcode == 20U || opcode == 28U || opcode == 30U) {
                const auto target = static_cast<std::int64_t>(pc) + 1 +
                    static_cast<std::int64_t>(bx) - 131071;
                if (target < 0 || target >= static_cast<std::int64_t>(prototype.code.size())) {
                    return core::Result<void>::failure(
                        diagnostic(diagnostic_codes::load_parse, source_, pc, "jump target is out of range")
                    );
                }
            }
        }
        return core::Result<void>::success();
    }

    static void append_u8(std::vector<std::byte>& output, const std::uint8_t value) {
        output.push_back(static_cast<std::byte>(value));
    }

    template <typename T>
    static void append_native(std::vector<std::byte>& output, const T value) {
        const auto old_size = output.size();
        output.resize(old_size + sizeof(T));
        std::memcpy(output.data() + old_size, &value, sizeof(T));
    }

    static void emit_header(std::vector<std::byte>& output) {
        const std::array<std::byte, 14> prefix{
            std::byte{0x1b}, std::byte{0x4c}, std::byte{0x75}, std::byte{0x61},
            std::byte{0x50}, std::byte{0x01}, std::byte{0x04},
            static_cast<std::byte>(sizeof(std::size_t)), std::byte{0x04},
            std::byte{0x06}, std::byte{0x08}, std::byte{0x09},
            std::byte{0x09}, std::byte{0x08},
        };
        output.insert(output.end(), prefix.begin(), prefix.end());
        append_native(output, 31'415'926.535897933);
    }

    static void emit_string(const StoredString& string, std::vector<std::byte>& output) {
        if (!string.present) {
            append_native<std::size_t>(output, 0);
            return;
        }
        append_native(output, string.bytes.size() + 1U);
        const auto old_size = output.size();
        output.resize(old_size + string.bytes.size() + 1U);
        std::memcpy(output.data() + old_size, string.bytes.data(), string.bytes.size());
        output.back() = std::byte{0};
    }

    static void emit_prototype(const Prototype& prototype, std::vector<std::byte>& output) {
        emit_string(prototype.source, output);
        append_native(output, prototype.first_line);
        append_u8(output, prototype.upvalues);
        append_u8(output, prototype.parameters);
        append_u8(output, prototype.vararg);
        append_u8(output, prototype.max_stack);
        append_native(output, static_cast<std::int32_t>(prototype.lines.size()));
        for (const auto line : prototype.lines) append_native(output, line);
        append_native(output, static_cast<std::int32_t>(prototype.locals.size()));
        for (const auto& local : prototype.locals) {
            emit_string(local.name, output);
            append_native(output, local.start_pc);
            append_native(output, local.end_pc);
        }
        append_native(output, static_cast<std::int32_t>(prototype.upvalue_names.size()));
        for (const auto& name : prototype.upvalue_names) emit_string(name, output);
        append_native(output, static_cast<std::int32_t>(prototype.constants.size()));
        for (const auto& constant : prototype.constants) {
            append_u8(output, constant.tag);
            if (constant.tag == 3) append_native(output, constant.number);
            else if (constant.tag == 4) emit_string(constant.string, output);
        }
        append_native(output, static_cast<std::int32_t>(prototype.nested.size()));
        for (const auto& child : prototype.nested) emit_prototype(child, output);
        append_native(output, static_cast<std::int32_t>(prototype.code.size()));
        for (const auto instruction : prototype.code) append_native(output, instruction);
    }

    std::span<const std::byte> bytes_;
    std::string source_;
    std::size_t offset_{0};
    std::int32_t next_persistence_id_{1};
    std::size_t line_entries_{0};
    std::size_t local_entries_{0};
    std::size_t upvalue_name_entries_{0};
    std::size_t constant_entries_{0};
    std::size_t prototype_entries_{1};
    std::size_t instruction_entries_{0};
    std::vector<PgluaPrototypeInfo> prototype_info_;
    std::vector<PgluaLocation> unsupported_execution_;
};

[[nodiscard]] bool valid_retail_identity(const RetailIdentity& identity) {
    if (identity.profile == "eaw") {
        return identity.archive_sha256 == "79774426a7d1442a2dd08eb87e16e3c9b39093f1224d862def7281d6587ccb35";
    }
    if (identity.profile == "foc") {
        return identity.archive_sha256 == "f9e1d517c3b0990d764843827a3cf42aa71cd91c2bdd8b0b15505b73ffcd9dc9";
    }
    return false;
}

} // namespace

core::Result<ConvertedChunk> convert_pglua(
    const std::span<const std::byte> input,
    std::string logical_source,
    const std::optional<RetailIdentity> retail_identity
) {
    if constexpr (std::endian::native != std::endian::little) {
        return core::Result<ConvertedChunk>::failure(
            diagnostic(
                diagnostic_codes::unsupported_feature,
                logical_source,
                0,
                "native-width PGLua conversion currently requires a little-endian target"
            )
        );
    }
    if (retail_identity && !valid_retail_identity(*retail_identity)) {
        return core::Result<ConvertedChunk>::failure(
            diagnostic(
                diagnostic_codes::load_parse,
                logical_source,
                0,
                "retail profile/archive identity does not match a pinned corpus"
            )
        );
    }
    return Decoder(input, std::move(logical_source)).run();
}

} // namespace eawr::script
