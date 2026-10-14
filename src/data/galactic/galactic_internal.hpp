#pragma once

#include "eawr/data/galactic/galactic_data.hpp"
#include "eawr/data/retail_values.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::data::galactic::detail {

[[nodiscard]] std::string trimmed(std::string_view text);
[[nodiscard]] std::string lower(std::string_view text);
// Tokens of a list-shaped value: split on commas and whitespace, empties dropped.
[[nodiscard]] std::vector<std::string> tokens(std::string_view text);

// One record's layers, most-derived first, and the issues its reads raise.
class Record final {
public:
    Record(std::string record_owner, std::vector<const Definition*> record_layers, std::vector<Issue>& issues);
    Record(std::string record_owner, const XmlNode& root, std::string source_file, std::vector<Issue>& issues);
    Record(const Record&) = delete;
    Record& operator=(const Record&) = delete;

    [[nodiscard]] const std::string& owner() const noexcept { return owner_; }
    [[nodiscard]] const std::string& file() const noexcept { return file_; }

    // AU-80/81: the last non-placeholder value of the most-derived layer that has one.
    [[nodiscard]] const XmlNode* node(std::string_view tag) const;
    [[nodiscard]] std::string text(std::string_view tag) const;
    [[nodiscard]] std::optional<bool> boolean(std::string_view tag) const;
    // AU-82: the leading number; trailing text is an issue row, no number at all a bad_value row.
    [[nodiscard]] std::optional<Fixed> number(std::string_view tag) const;
    [[nodiscard]] std::optional<std::int64_t> whole(std::string_view tag) const;
    [[nodiscard]] std::optional<Position> position(std::string_view tag) const;
    // Every non-placeholder occurrence, base layer first, each one a record.
    [[nodiscard]] std::vector<const XmlNode*> records(std::string_view tag) const;
    // A list-shaped single value split into its names.
    [[nodiscard]] std::vector<std::string> names(std::string_view tag) const;

    // `<Tag> Faction, value </Tag>` records; a malformed record is an issue and is skipped.
    [[nodiscard]] std::vector<FactionNumber> faction_numbers(std::string_view tag) const;
    [[nodiscard]] std::vector<FactionName> faction_names(std::string_view tag) const;
    [[nodiscard]] std::vector<PlacedType> placed_types(std::string_view tag) const;

    // Issues on the record's own definition: tags its class does not know.
    void unknown_tags() const;

    void issue(IssueKind kind, const XmlNode* at, std::string tag, std::string value, std::string reason) const;

private:
    [[nodiscard]] std::optional<Fixed> parse_number(const XmlNode& at, std::string_view raw, std::string_view tag) const;

    std::string owner_;
    std::string file_;
    std::vector<const Definition*> layers_;
    std::optional<Definition> own_; // a record read from a plain document
    std::vector<Issue>* issues_;
};

// The definition's layers through the catalog, most-derived first; a broken variant chain is an
// issue and leaves the definition alone.
[[nodiscard]] std::vector<const Definition*> layers_of(const Catalog& catalog, const Definition& definition,
    std::vector<Issue>& issues);

void record_input(GalacticData& data, const SourceLocation& source, std::string sha256);
// record_input for a catalog definition's file, with the digest the catalog took of it.
void catalog_input(const Catalog& catalog, GalacticData& data, const SourceLocation& source);

void load_campaigns(const Catalog& catalog, GalacticData& data);
void load_planets(const Catalog& catalog, GalacticData& data);
void load_factions(const Catalog& catalog, GalacticData& data);
[[nodiscard]] core::Result<bool> load_trade_routes(const vfs::Vfs& filesystem, GalacticData& data);
void load_constants(const vfs::Vfs& filesystem, GalacticData& data);
// Cross-record references: campaign locations, routes and records against the loaded tables.
void check_references(const Catalog& catalog, GalacticData& data);

} // namespace eawr::data::galactic::detail
