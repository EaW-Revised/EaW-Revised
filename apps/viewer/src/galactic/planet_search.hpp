#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eawr::viewer::galactic {

// PS-02..04. Callers supply localized display names and their Unicode search
// keys and observer discovery flags. Hidden rows expose names only; callers gate
// selection and art on discovery. No authoritative state lives here.
struct PlanetSearchEntry {
    std::uint64_t id{};
    std::string name, folded;
    bool discovered{};
    friend bool operator==(const PlanetSearchEntry&, const PlanetSearchEntry&) = default;
};
// Hidden HUD facts intentionally omit the caption. The finder exposes only the
// authored localized name (with the map's internal-name fallback), never other facts.
[[nodiscard]] inline std::string planet_search_name(bool discovered, std::string_view hud_name,
    std::string_view authored_label, std::string_view internal_name) {
    if (discovered || !hud_name.empty()) return std::string(hud_name);
    return std::string(authored_label.empty() ? internal_name : authored_label);
}
class PlanetSearchIndex {
public:
    template <typename Less>
    bool update(std::vector<PlanetSearchEntry> entries, Less less) {
        std::erase_if(entries, [](const auto& row) {
            return row.name.find_first_not_of(" \t\r\n") == std::string::npos;
        });
        if (entries == source_) return false;
        source_ = std::move(entries);
        sorted_ = source_;
        // Stable order preserves campaign order for equal localized names.
        std::stable_sort(sorted_.begin(), sorted_.end(), [&](const auto& a, const auto& b) { return less(a.name, b.name); });
        return true;
    }
    [[nodiscard]] std::vector<std::uint64_t> filter(std::string_view folded) const {
        std::vector<std::uint64_t> result;
        for (const auto& row : sorted_) if (row.folded.find(folded) != std::string::npos) result.push_back(row.id);
        return result;
    }
    [[nodiscard]] const std::vector<PlanetSearchEntry>& entries() const { return sorted_; }
private:
    std::vector<PlanetSearchEntry> source_, sorted_;
};

} // namespace eawr::viewer::galactic
