#pragma once
#include "methods.hpp"
#include "eawr/mods/profile.hpp"
#include <functional>
#include <memory>
namespace eawr::mods_service {
class Records {
public:
    explicit Records(Service&);
    ~Records();
    std::optional<Reply> start(std::uint64_t, std::string, const mods::steam::JsonValue&);
    Reply cancel(const mods::steam::JsonValue&);
    void stop();
    void forget_profile(std::string_view);
    void invalidate_reuse(); // drop every reusable chain fingerprint (any mutating method)
    std::function<void(std::string)> emit;
    mods::steam::fs::path executable; // Synthetic helper seam, identical containment to production.
private:
    struct State;
    std::unique_ptr<State> state_;
    Service& service_;
};
Reply method_records_index(Service&, const mods::steam::JsonValue&);
Reply method_records_get(Service&, const mods::steam::JsonValue&);
Reply method_records_cancel(Service&, const mods::steam::JsonValue&);
// Reuse the model's guarded profile paths, current discovery and chain validation.
std::optional<mods::Profile> records_profile(Service&, const mods::steam::JsonValue&, Reply&);
}
