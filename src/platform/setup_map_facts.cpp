#include "eawr/platform/setup_map_facts.hpp"

#include "eawr/platform/model_facts.hpp"

#include <array>

namespace eawr::platform {
namespace {

constexpr std::string_view key_domain = "eawr-setup-map-facts-key/v1";

class DerivedSetupFacts final : public skirmish::SetupMapFactsStore {
public:
    explicit DerivedSetupFacts(std::shared_ptr<DerivedStore> store) : store_(std::move(store)) {}

    std::optional<std::vector<std::uint8_t>> lookup(const std::string_view kind, const std::string_view context,
        const std::string_view path, const skirmish::SetupFactsStamp& stamp) override {
        return store_->lookup(setup_map_facts_kind, key(kind, context, path), derived(stamp));
    }
    void put(const std::string_view kind, const std::string_view context, const std::string_view path,
        const skirmish::SetupFactsStamp& stamp, const std::span<const std::uint8_t> body) override {
        store_->put(setup_map_facts_kind, key(kind, context, path), derived(stamp), body);
    }
    void flush() override { store_->flush(); }

private:
    static DerivedKey key(const std::string_view kind, const std::string_view context, const std::string_view path) {
        const std::array<std::string_view, 3> parts{kind, context, path};
        return derived_key(key_domain, parts);
    }
    static DerivedStamp derived(const skirmish::SetupFactsStamp& stamp) {
        return DerivedStamp{stamp.container_size, stamp.container_mtime, stamp.entry_offset, stamp.entry_size};
    }
    std::shared_ptr<DerivedStore> store_;
};

} // namespace

std::shared_ptr<skirmish::SetupMapFactsStore> make_setup_map_facts_store(std::shared_ptr<DerivedStore> store) {
    if (!store) return nullptr;
    return std::make_shared<DerivedSetupFacts>(std::move(store));
}

std::shared_ptr<skirmish::SetupMapFactsStore> process_setup_map_facts_store() {
    return make_setup_map_facts_store(model_facts_store());
}

} // namespace eawr::platform
