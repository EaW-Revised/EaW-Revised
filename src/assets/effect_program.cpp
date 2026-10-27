#include "effect_program_internal.hpp"
#include "asset_internal.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <utility>

namespace eawr::assets {
namespace {
struct ProgramFailure final { std::string_view code; const char* message; std::size_t offset; };
class ProgramReader final {
    std::span<const std::byte> bytes_;
    std::size_t base_;
    std::size_t& metadata_left_;
    std::size_t& work_left_;
    EffectProgram result_;
    const Source& source_;
    std::span<const std::byte> table_;
    std::size_t table_base_{};
    struct CachedType { std::size_t index{}; bool done{}; };
    std::map<std::size_t, CachedType> types_;
    std::map<std::size_t, std::string_view> strings_;
    [[noreturn]] static void fail(std::string_view code, const char* message, std::size_t at) {
        throw ProgramFailure{code, message, at};
    }
    void work(std::size_t count, std::size_t at) {
        if (count > work_left_) fail(diagnostic_codes::limit, "shader metadata work budget exceeded", at);
        work_left_ -= count;
    }
    template<class T> void allocate(std::size_t count, std::size_t at) {
        if (count > metadata_left_ / sizeof(T))
            fail(diagnostic_codes::limit, "shader metadata budget exceeded", at);
        metadata_left_ -= count * sizeof(T);
    }
    static void range(std::span<const std::byte> b, std::size_t at, std::size_t size, std::size_t base) {
        if (at > b.size() || size > b.size() - at)
            fail(diagnostic_codes::bounds, "shader metadata range exceeds enclosing span", base + std::min(at, b.size()));
    }
    std::uint32_t integer(std::span<const std::byte> b, std::size_t at, unsigned size, std::size_t base) {
        work(1U, base + at); range(b, at, size, base); std::uint32_t value{};
        for (unsigned i = 0U; i < size; ++i)
            value |= std::uint32_t(std::to_integer<unsigned char>(b[at + i])) << (8U * i);
        return value;
    }
    std::uint32_t word(std::size_t at) { return integer(bytes_, at, 4U, base_); }
    std::uint32_t field(std::size_t at) { return integer(table_, at, 4U, table_base_); }
    std::uint16_t half(std::size_t at) { return static_cast<std::uint16_t>(integer(table_, at, 2U, table_base_)); }
    std::string_view text(std::size_t at) {
        if (!at) return {};
        if (auto found = strings_.find(at); found != strings_.end()) return found->second;
        range(table_, at, 1U, table_base_); auto end = at;
        while (end < table_.size()) {
            work(1U, table_base_ + end);
            if (table_[end] == std::byte{}) break;
            ++end;
        }
        if (end == table_.size()) fail(diagnostic_codes::structure, "shader metadata string is unterminated", table_base_ + at);
        const std::string_view value{reinterpret_cast<const char*>(table_.data() + at), end - at};
        allocate<std::byte>(64U, table_base_ + at); strings_.emplace(at, value); return value;
    }
    std::size_t type(std::size_t at) {
        struct Frame { std::size_t offset{}, index{}, members{}, member_table{}, next{}; };
        std::vector<Frame> stack;
        const auto enter = [&](std::size_t offset) {
            range(table_, offset, 16U, table_base_);
            EffectConstantType item;
            item.value_class = half(offset); item.kind = half(offset + 2U);
            item.rows = half(offset + 4U); item.columns = half(offset + 6U); item.elements = half(offset + 8U);
            const auto members = half(offset + 10U); const auto member_table = field(offset + 12U);
            if (item.value_class > 5U || item.kind > 19U)
                fail(diagnostic_codes::unsupported, "shader constant type is unknown", table_base_ + offset);
            if (item.value_class <= 3U && (!item.rows || !item.columns))
                fail(diagnostic_codes::structure, "shader constant numeric shape is empty", table_base_ + offset);
            if (members && item.value_class != 5U)
                fail(diagnostic_codes::structure, "shader non-struct type has members", table_base_ + offset);
            range(table_, member_table, std::size_t{members} * 8U, table_base_);
            allocate<EffectConstantType>(1U, table_base_ + offset);
            allocate<Frame>(1U, table_base_ + offset); allocate<std::byte>(64U, table_base_ + offset);
            const auto index = result_.constant_types.size(); result_.constant_types.push_back(std::move(item));
            types_.emplace(offset, CachedType{index, false}); stack.push_back({offset, index, members, member_table, 0U});
        };
        if (const auto found = types_.find(at); found != types_.end()) {
            if (!found->second.done) fail(diagnostic_codes::hierarchy_cycle, "shader constant type cycle", table_base_ + at);
            return found->second.index;
        }
        enter(at);
        while (!stack.empty()) {
            auto& frame = stack.back(); work(1U, table_base_ + frame.offset);
            if (frame.next == frame.members) { types_.at(frame.offset).done = true; stack.pop_back(); continue; }
            const auto record = frame.member_table + frame.next * 8U;
            const auto child = field(record + 4U);
            const auto found = types_.find(child);
            if (found == types_.end()) { enter(child); continue; }
            if (!found->second.done) fail(diagnostic_codes::hierarchy_cycle, "shader constant type cycle", table_base_ + child);
            const auto name = text(field(record));
            allocate<EffectConstantMember>(1U, table_base_ + record);
            result_.constant_types[frame.index].members.push_back({name, found->second.index}); ++frame.next;
        }
        return types_.at(at).index;
    }
    void constants(std::span<const std::byte> bytes, std::size_t base) {
        table_ = bytes; table_base_ = base;
        range(table_, 0U, 28U, base);
        if (field(0U) != 28U) fail(diagnostic_codes::unsupported, "shader constant table header size is unsupported", base);
        if (field(8U) != word(0U)) fail(diagnostic_codes::structure, "shader constant table profile differs from program", base + 8U);
        result_.creator = text(field(4U)); result_.target = text(field(24U)); result_.compiler_flags = field(20U);
        const auto count = field(12U), offset = field(16U);
        if (offset > table_.size() || count > (table_.size() - offset) / 20U)
            fail(diagnostic_codes::bounds, "shader constant count exceeds table", base + 12U);
        allocate<EffectConstant>(count, base + offset);
        for (std::size_t i = 0U; i < count; ++i) {
            const auto at = offset + i * 20U; EffectConstant item;
            item.name = text(field(at)); const auto set = half(at + 4U);
            if (set > 3U) fail(diagnostic_codes::unsupported, "shader constant register set is unknown", base + at + 4U);
            item.register_set = static_cast<EffectRegisterSet>(set);
            item.register_index = half(at + 6U); item.register_count = half(at + 8U);
            item.type = type(field(at + 12U));
            const auto defaults = field(at + 16U);
            if (defaults) {
                if (set == 3U) fail(diagnostic_codes::unsupported, "shader sampler register defaults are unsupported", base + at + 16U);
                const auto size = std::size_t{item.register_count} * (set == 0U ? 4U : 16U);
                range(table_, defaults, size, base); item.defaults = table_.subspan(defaults, size);
            }
            result_.constants.push_back(item);
        }
    }
    // EFX-08: shader-model-one instructions have no encoded operand count.
    // Counts follow the documented instruction signatures, not installed data.
    std::size_t legacy_operands(std::uint32_t op, std::size_t at) const {
        switch (op) {
        case 0U: case 0xfffdU: return 0U; // nop, phase
        case 1U: case 6U: case 7U: case 14U: case 15U: case 16U: case 19U:
        case 35U: case 36U: case 46U: case 67U: case 68U: case 69U: case 70U:
        case 71U: case 72U: case 73U: case 74U: case 77U: case 78U: case 79U:
        case 82U: case 83U: case 84U: case 85U: case 86U: return 2U;
        case 2U: case 3U: case 5U: case 8U: case 9U: case 10U: case 11U:
        case 12U: case 13U: case 17U: case 20U: case 21U: case 22U: case 23U:
        case 24U: case 32U: case 33U: case 76U: case 89U: return 3U;
        case 4U: case 18U: case 34U: case 80U: case 88U: case 90U: return 4U;
        case 31U: return 2U;
        case 81U: return 5U;
        case 65U: case 87U: return 1U;
        case 64U: case 66U: return result_.stage == EffectProgramStage::pixel && result_.minor == 4U ? 2U : 1U;
        default: fail(diagnostic_codes::unsupported, "shader-model-one opcode framing is unknown", base_ + at);
        }
    }
    static EffectCommentKind comment_kind(std::uint32_t tag) {
        switch (tag) {
        case 0x42415443U: return EffectCommentKind::constant_table;
        case 0x53455250U: return EffectCommentKind::preshader;
        case 0x54494c43U: return EffectCommentKind::literal_table;
        case 0x434c5846U: return EffectCommentKind::expression_code;
        default: return EffectCommentKind::unknown;
        }
    }
public:
    ProgramReader(std::span<const std::byte> bytes, const Source& source, std::size_t base,
                  std::size_t& metadata, std::size_t& work)
        : bytes_(bytes), base_(base), metadata_left_(metadata), work_left_(work), source_(source) {}
    EffectExpression expression() {
        range(bytes_, 0U, 8U, base_);
        if (bytes_.size() % 4U) fail(diagnostic_codes::structure, "effect expression size is unaligned", base_);
        const auto version = word(0U);
        if (version != 0x46580200U && version != 0x46580201U)
            fail(diagnostic_codes::unsupported, "effect expression version is unsupported", base_);
        EffectExpression item; item.version = version; item.byte_offset = base_; item.bytes = bytes_;
        bool table = false;
        for (std::size_t at = 4U; at < bytes_.size();) {
            const auto token = word(at);
            if (token == 0xffffU) {
                if (at + 4U != bytes_.size()) fail(diagnostic_codes::structure, "bytes follow effect expression END", base_ + at + 4U);
                item.constant_types = std::move(result_.constant_types); item.inputs = std::move(result_.constants);
                item.creator = result_.creator; item.target = result_.target; item.compiler_flags = result_.compiler_flags;
                return item;
            }
            if ((token & 0x8000ffffU) != 0xfffeU)
                fail(diagnostic_codes::unsupported, "effect expression section framing is unsupported", base_ + at);
            const auto count = std::size_t{(token >> 16U) & 0x7fffU};
            range(bytes_, at + 4U, count * 4U, base_);
            const auto tag = count ? word(at + 4U) : 0U; const auto kind = comment_kind(tag);
            allocate<EffectProgramComment>(1U, base_ + at);
            item.sections.push_back({kind, tag, base_ + at + 4U, bytes_.subspan(at + 4U, count * 4U)});
            if (kind == EffectCommentKind::constant_table) {
                if (table) fail(diagnostic_codes::structure, "effect expression has duplicate constant tables", base_ + at);
                table = true; constants(bytes_.subspan(at + 8U, (count - 1U) * 4U), base_ + at + 8U);
            }
            at += (count + 1U) * 4U;
        }
        fail(diagnostic_codes::structure, "effect expression END is missing", base_ + bytes_.size());
    }
    EffectProgram read() {
        range(bytes_, 0U, 8U, base_);
        if (bytes_.size() % 4U) fail(diagnostic_codes::structure, "shader bytecode size is unaligned", base_);
        const auto version = word(0U); const auto stage = version >> 16U;
        result_.major = (version >> 8U) & 255U; result_.minor = version & 255U;
        if ((stage != 0xfffeU && stage != 0xffffU) || result_.major < 1U || result_.major > 3U ||
            (result_.major >= 2U && result_.minor != 0U) ||
            (result_.major == 1U && (stage == 0xfffeU ? result_.minor != 1U : result_.minor < 1U || result_.minor > 4U)))
            fail(diagnostic_codes::unsupported, "shader profile is unsupported", base_);
        result_.stage = stage == 0xfffeU ? EffectProgramStage::vertex : EffectProgramStage::pixel;
        result_.byte_offset = base_; result_.bytes = bytes_;
        bool has_table = false;
        for (std::size_t at = 4U; at < bytes_.size();) {
            const auto token = word(at), op = token & 0xffffU;
            if (token == 0xffffU) {
                if (at + 4U != bytes_.size()) fail(diagnostic_codes::structure, "shader bytes follow END token", base_ + at + 4U);
                return std::move(result_);
            }
            if (token & 0x80000000U) fail(diagnostic_codes::structure, "shader instruction token has operand marker", base_ + at);
            if (op == 0xfffeU) {
                const auto count = std::size_t{(token >> 16U) & 0x7fffU};
                range(bytes_, at + 4U, count * 4U, base_);
                const auto tag = count ? word(at + 4U) : 0U;
                const auto kind = comment_kind(tag);
                allocate<EffectProgramComment>(1U, base_ + at);
                result_.comments.push_back({kind, tag, base_ + at + 4U, bytes_.subspan(at + 4U, count * 4U)});
                if (kind == EffectCommentKind::constant_table) {
                    if (has_table) fail(diagnostic_codes::structure, "shader has duplicate constant tables", base_ + at);
                    has_table = true; constants(bytes_.subspan(at + 8U, (count - 1U) * 4U), base_ + at + 8U);
                }
                // A tag-only PRES comment is an empty marker, not an expression.
                // Nonempty bodies still require the complete expression envelope.
                if (kind == EffectCommentKind::preshader && count > 1U) {
                    allocate<EffectExpression>(1U, base_ + at);
                    // No recursive PRES parsing: expressions contain only framed
                    // sections; even an unknown section is retained as bytes.
                    auto parsed = ProgramReader(bytes_.subspan(at + 8U, (count - 1U) * 4U), source_,
                        base_ + at + 8U, metadata_left_, work_left_).expression();
                    result_.preshaders.push_back(std::move(parsed));
                }
                at += (count + 1U) * 4U; continue;
            }
            if (op > 96U && op != 0xfffdU) fail(diagnostic_codes::unsupported, "shader opcode is unknown", base_ + at);
            const auto operands = result_.major >= 2U ? std::size_t{(token >> 24U) & 15U} : legacy_operands(op, at);
            range(bytes_, at + 4U, operands * 4U, base_);
            if (op == 31U) {
                if (operands != 2U) fail(diagnostic_codes::structure, "shader declaration has invalid token count", base_ + at);
                const auto usage = word(at + 4U), reg = word(at + 8U);
                allocate<EffectDeclaration>(1U, base_ + at);
                result_.declarations.push_back({usage & 15U, (usage >> 16U) & 15U,
                    ((reg >> 28U) & 7U) | ((reg >> 8U) & 24U), reg & 0x7ffU,
                    (usage >> 27U) & 15U, (reg >> 16U) & 15U});
            }
            at += (operands + 1U) * 4U;
        }
        fail(diagnostic_codes::structure, "shader END token is missing", base_ + bytes_.size());
    }
};
}

core::Result<EffectProgram> detail::read_effect_program(std::span<const std::byte> bytes, const Source& source,
    std::size_t byte_offset, std::size_t& metadata_left, std::size_t& work_left) {
    try {
        auto result = ProgramReader(bytes, source, byte_offset, metadata_left, work_left).read();
        result.source = source;
        return core::Result<EffectProgram>::success(std::move(result));
    } catch (const ProgramFailure& error) {
        return core::Result<EffectProgram>::failure(detail::error(source, error.code, error.message, error.offset));
    }
}
core::Result<EffectExpression> detail::read_effect_expression(std::span<const std::byte> bytes, const Source& source,
    std::size_t byte_offset, std::size_t& metadata_left, std::size_t& work_left) {
    try {
        return core::Result<EffectExpression>::success(ProgramReader(bytes, source, byte_offset, metadata_left, work_left).expression());
    } catch (const ProgramFailure& error) {
        return core::Result<EffectExpression>::failure(detail::error(source, error.code, error.message, error.offset));
    }
}
core::Result<EffectProgram> read_shader_program(std::span<const std::byte> bytes, Source source, EffectReadLimits limits) {
    if (source.stored_size && source.stored_size != bytes.size())
        return core::Result<EffectProgram>::failure(detail::error(source, diagnostic_codes::source_mismatch,
            "shader provenance size differs from its byte span", 0U));
    auto work = bytes.size() > std::numeric_limits<std::size_t>::max() / 32U
        ? std::numeric_limits<std::size_t>::max() : bytes.size() * 32U;
    return detail::read_effect_program(bytes, source, 0U, limits.metadata_bytes, work);
}
}
