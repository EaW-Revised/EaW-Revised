#pragma once
#include "reference_pages.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/data/ui/dialog_catalog.hpp"
#include <functional>
#include <utility>

namespace eawr::viewer::galactic {
// The reference publication owns its immutable data independently of startup
// services. Unresolved types return absence, never default metadata.
class ReferenceCatalog final {
public:
    using Text = std::function<std::string(std::string_view)>;
    ReferenceCatalog(data::Catalog catalog, Text text) : catalog_(std::move(catalog)), text_(std::move(text)) {}
    [[nodiscard]] std::optional<ReferenceType> metadata(std::string_view name) const;
    [[nodiscard]] std::vector<TreeSlot> tree_slots(const data::ui::Dialog* dialog, std::string_view faction,
        const std::map<std::uint64_t, std::string>& type_names) const;
private:
    const data::Catalog catalog_;
    Text text_;
};
}
