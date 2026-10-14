#include "eawr/checkpoint/checkpoint.hpp"

#include "encoding_internal.hpp"
#include "eawr/core/sha256.hpp"

#include <algorithm>
#include <string>

namespace eawr::checkpoint {

namespace detail {

core::Diagnostic diagnostic(const std::string_view code, std::string message, const std::string_view logical_path) {
    core::Diagnostic result;
    result.code = std::string(code);
    result.message = std::move(message);
    if (!logical_path.empty()) result.logical_path = std::string(logical_path);
    result.source_id = std::string("checkpoint");
    return result;
}

std::string hex(const Digest& digest) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2U);
    for (const auto byte : digest) {
        result.push_back(digits[static_cast<std::size_t>(byte >> 4U)]);
        result.push_back(digits[static_cast<std::size_t>(byte & 0x0fU)]);
    }
    return result;
}

bool from_hex(const std::string_view text, Digest& digest) noexcept {
    if (text.size() != digest.size() * 2U) return false;
    const auto nibble = [](const char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < digest.size(); ++index) {
        const auto high = nibble(text[2U * index]);
        const auto low = nibble(text[2U * index + 1U]);
        if (high < 0 || low < 0) return false;
        digest[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

} // namespace detail

namespace {

using detail::diagnostic;

template <typename T>
[[nodiscard]] core::Result<T> fail(const std::string_view code, std::string message, const std::string_view path) {
    return core::Result<T>::failure(diagnostic(code, std::move(message), path));
}

void write_identities(detail::Writer& out, const Identities& identities) {
    out.u32(identities.rules_version);
    out.u32(identities.state_encoding_version);
    out.u32(0); // reserved
    out.raw(identities.numeric_abi);
    out.raw(identities.lua_save);
    out.raw(identities.content);
    out.raw(identities.campaign);
    out.raw(identities.mod_list);
}

} // namespace

std::string_view section_name(const std::uint16_t id) noexcept {
    switch (static_cast<SectionId>(id)) {
    case SectionId::session: return "session";
    case SectionId::seats: return "seats";
    case SectionId::players: return "players";
    case SectionId::planets: return "planets";
    case SectionId::fleets: return "fleets";
    case SectionId::production: return "production";
    case SectionId::conflicts: return "conflicts";
    case SectionId::victory: return "victory";
    case SectionId::scripts: return "scripts";
    case SectionId::child_log: return "child-log";
    case SectionId::child_state: return "child-state";
    case SectionId::mod_list: return "mod-list";
    case SectionId::tactical_return: return "tactical-return";
    case SectionId::galactic_ai: return "galactic-ai";
    case SectionId::galactic_abilities: return "galactic-abilities";
    case SectionId::campaign_scoring: return "campaign-scoring";
    case SectionId::galactic_owner: return "galactic-owner";
    case SectionId::presentation_hints: return "presentation-hints";
    case SectionId::campaign_identity: return "campaign-identity";
    }
    return "unknown";
}

bool known_section(const std::uint16_t id) noexcept {
    return section_name(id) != "unknown";
}

const Section* Checkpoint::find(const SectionId id) const noexcept {
    const auto value = static_cast<std::uint16_t>(id);
    const auto found = std::find_if(sections.begin(), sections.end(),
        [value](const Section& section) { return section.id == value; });
    return found == sections.end() ? nullptr : &*found;
}

core::Result<std::vector<std::uint8_t>> write_checkpoint(const Checkpoint& checkpoint) {
    using Bytes = std::vector<std::uint8_t>;
    const auto& header = checkpoint.header;
    if ((header.flags & ~known_flags) != 0U) {
        return fail<Bytes>(diagnostic_codes::invalid_save, "header flags hold an unknown bit", {});
    }
    if ((header.flags & flag_frontier) == 0U && header.frontier != InputFrontier{}) {
        return fail<Bytes>(diagnostic_codes::invalid_save, "a frontier without the frontier flag", {});
    }
    if (checkpoint.sections.size() > max_sections) {
        return fail<Bytes>(diagnostic_codes::resource_limit,
            std::to_string(checkpoint.sections.size()) + " sections exceed the limit of " + std::to_string(max_sections), {});
    }
    std::uint64_t payload_size = 0;
    for (std::size_t index = 0; index < checkpoint.sections.size(); ++index) {
        const auto& section = checkpoint.sections[index];
        if (index > 0 && checkpoint.sections[index - 1].id >= section.id) {
            return fail<Bytes>(diagnostic_codes::section_order, "section " + std::string(section_name(section.id))
                + " (" + std::to_string(section.id) + ") does not follow its predecessor in ascending order", {});
        }
        if (section.bytes.size() > max_bytes) {
            return fail<Bytes>(diagnostic_codes::resource_limit,
                "section " + std::string(section_name(section.id)) + " exceeds the checkpoint limit", {});
        }
        payload_size += section_header_size + section.bytes.size();
    }
    if (payload_size > max_bytes - header_size) {
        return fail<Bytes>(diagnostic_codes::resource_limit, "the checkpoint needs " + std::to_string(header_size + payload_size)
            + " bytes, over the limit of " + std::to_string(max_bytes), {});
    }

    detail::Writer payload;
    payload.bytes().reserve(static_cast<std::size_t>(payload_size));
    for (const auto& section : checkpoint.sections) {
        payload.u16(section.id);
        payload.u16(section.version);
        payload.u16(section.required ? section_flag_required : 0U);
        payload.u16(0); // reserved
        payload.u64(section.bytes.size());
        payload.raw(section.bytes);
    }
    const auto payload_digest = core::sha256(payload.bytes());

    detail::Writer out;
    out.bytes().reserve(header_size + payload.bytes().size());
    out.raw(magic);
    out.u16(format_version);
    out.u16(header.flags);
    write_identities(out, header.identities);
    out.u64(header.seed);
    out.u64(header.completed_tick);
    out.u64(header.galactic_frame);
    out.u64(header.frontier.tick);
    out.u32(header.frontier.seat);
    out.u32(static_cast<std::uint32_t>(checkpoint.sections.size()));
    out.u64(header.frontier.sequence);
    out.raw(header.frontier.input_digest);
    out.u64(payload.bytes().size());
    out.raw(payload_digest);
    out.raw(payload.bytes());
    return core::Result<Bytes>::success(std::move(out.bytes()));
}

core::Result<Checkpoint> parse_checkpoint(const std::span<const std::uint8_t> bytes, const std::string_view path) {
    if (bytes.size() > max_bytes) {
        return fail<Checkpoint>(diagnostic_codes::resource_limit,
            std::to_string(bytes.size()) + " bytes exceed the checkpoint limit of " + std::to_string(max_bytes), path);
    }
    if (bytes.size() < header_size) {
        return fail<Checkpoint>(diagnostic_codes::malformed, "truncated header: " + std::to_string(bytes.size())
            + " of " + std::to_string(header_size) + " bytes", path);
    }
    detail::Reader in(bytes);
    const auto read_magic = in.take(magic.size());
    if (!std::equal(read_magic.begin(), read_magic.end(), magic.begin())) {
        return fail<Checkpoint>(diagnostic_codes::malformed, "not an EAWRCKP checkpoint", path);
    }
    const auto version = in.u16();
    if (version != format_version) {
        return fail<Checkpoint>(diagnostic_codes::version, "checkpoint format version " + std::to_string(version)
            + (version > format_version ? " is newer than" : " is not") + " this build's version "
            + std::to_string(format_version), path);
    }
    Checkpoint result;
    auto& header = result.header;
    header.flags = in.u16();
    header.identities.rules_version = in.u32();
    header.identities.state_encoding_version = in.u32();
    const auto reserved = in.u32();
    header.identities.numeric_abi = in.digest();
    header.identities.lua_save = in.digest();
    header.identities.content = in.digest();
    header.identities.campaign = in.digest();
    header.identities.mod_list = in.digest();
    header.seed = in.u64();
    header.completed_tick = in.u64();
    header.galactic_frame = in.u64();
    header.frontier.tick = in.u64();
    header.frontier.seat = in.u32();
    const auto section_count = in.u32();
    header.frontier.sequence = in.u64();
    header.frontier.input_digest = in.digest();
    const auto payload_length = in.u64();
    const auto payload_digest = in.digest();
    if ((header.flags & ~known_flags) != 0U || reserved != 0U) {
        return fail<Checkpoint>(diagnostic_codes::malformed, "header holds an unknown flag or a nonzero reserved field", path);
    }
    if ((header.flags & flag_frontier) == 0U && header.frontier != InputFrontier{}) {
        return fail<Checkpoint>(diagnostic_codes::malformed, "header holds a frontier without the frontier flag", path);
    }
    if (payload_length != in.remaining()) {
        return fail<Checkpoint>(diagnostic_codes::malformed, "payload length " + std::to_string(payload_length) + " but "
            + std::to_string(in.remaining()) + " bytes follow the header (truncated or trailing data)", path);
    }
    const auto payload = in.take(in.remaining());
    if (core::sha256(payload) != payload_digest) {
        return fail<Checkpoint>(diagnostic_codes::integrity, "payload SHA-256 differs from the header: the checkpoint is damaged", path);
    }
    if (section_count > max_sections) {
        return fail<Checkpoint>(diagnostic_codes::resource_limit, std::to_string(section_count)
            + " sections exceed the limit of " + std::to_string(max_sections), path);
    }
    detail::Reader sections(payload);
    for (std::uint32_t index = 0; index < section_count; ++index) {
        Section section;
        section.id = sections.u16();
        section.version = sections.u16();
        const auto flags = sections.u16();
        const auto section_reserved = sections.u16();
        const auto length = sections.u64();
        const auto name = std::string(section_name(section.id));
        if (sections.failed()) {
            return fail<Checkpoint>(diagnostic_codes::malformed, "truncated section header " + std::to_string(index), path);
        }
        if ((flags & ~section_flag_required) != 0U || section_reserved != 0U) {
            return fail<Checkpoint>(diagnostic_codes::malformed, "section " + name + " holds an unknown flag", path);
        }
        if (length > sections.remaining()) {
            return fail<Checkpoint>(diagnostic_codes::malformed, "section " + name + " runs past the payload", path);
        }
        if (!result.sections.empty() && result.sections.back().id >= section.id) {
            return fail<Checkpoint>(diagnostic_codes::section_order, "section " + name + " ("
                + std::to_string(section.id) + ") does not follow its predecessor in ascending order", path);
        }
        section.required = (flags & section_flag_required) != 0U;
        const auto body = sections.take(static_cast<std::size_t>(length));
        if (!known_section(section.id) && section.required) {
            return fail<Checkpoint>(diagnostic_codes::unknown_required_section, "section " + std::to_string(section.id)
                + " is required but unknown to this build", path);
        }
        section.bytes.assign(body.begin(), body.end());
        result.sections.push_back(std::move(section));
    }
    if (sections.remaining() != 0U) {
        return fail<Checkpoint>(diagnostic_codes::malformed, std::to_string(sections.remaining())
            + " bytes follow the last section", path);
    }
    return core::Result<Checkpoint>::success(std::move(result));
}

core::Result<void> check_identities(const Header& header, const Identities& build, const std::string_view path) {
    const auto& saved = header.identities;
    const auto differs = [&](const std::string_view what) {
        return core::Result<void>::failure(diagnostic(diagnostic_codes::other_build,
            "saved by another build: the " + std::string(what) + " differs", path));
    };
    if (saved.rules_version != build.rules_version) return differs("rules version");
    if (saved.state_encoding_version != build.state_encoding_version) return differs("state encoding version");
    if (saved.numeric_abi != build.numeric_abi) return differs("numeric ABI");
    if (saved.lua_save != build.lua_save) return differs("Lua save identity");
    if (saved.content != build.content) return differs("content identity");
    if (saved.campaign != build.campaign) return differs("campaign identity");
    if (saved.mod_list != build.mod_list) return differs("mod list");
    return core::Result<void>::success();
}

} // namespace eawr::checkpoint
