#pragma once
#include "methods.hpp"
namespace eawr::mods_service {
Reply method_game_find(Service&, const mods::steam::JsonValue&);
Reply method_mods_discover(Service&, const mods::steam::JsonValue&);
Reply method_mods_changes(Service&, const mods::steam::JsonValue&);
Reply method_chain_check(Service&, const mods::steam::JsonValue&);
Reply method_chain_resolve(Service&, const mods::steam::JsonValue&);
Reply method_profile_list(Service&, const mods::steam::JsonValue&);
Reply method_profile_load(Service&, const mods::steam::JsonValue&);
Reply method_profile_save(Service&, const mods::steam::JsonValue&);
Reply method_profile_delete(Service&, const mods::steam::JsonValue&);
}
