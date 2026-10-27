#pragma once
#include "methods.hpp"
#include <functional>
#include <memory>
namespace eawr::mods_service {
class LoadCheck {
public:
    using Emit = std::function<void(std::string)>;
    explicit LoadCheck(Service&);
    ~LoadCheck();
    // nullopt means the response will arrive through Emit.
    std::optional<Reply> start(std::uint64_t, const mods::steam::JsonValue&);
    Reply cancel(const mods::steam::JsonValue&);
    Reply result(const mods::steam::JsonValue&);
    void stop();
    Emit emit;
    mods::steam::fs::path executable; // Optional explicit helper path, for synthetic contract tests.
private:
    struct State;
    std::unique_ptr<State> state_;
    Service& service_;
};
Reply method_loadcheck_start(Service&, const mods::steam::JsonValue&);
Reply method_loadcheck_cancel(Service&, const mods::steam::JsonValue&);
Reply method_loadcheck_result(Service&, const mods::steam::JsonValue&);
}
