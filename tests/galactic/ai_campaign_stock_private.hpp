#pragma once

namespace galactic_campaign_stock_test {
void qualified_safety_contracts();
void no_source_transit_contracts();
void nested_actor_facts_contracts();
void actor_parent_depth_contracts();
void sparse_actor_change_contracts();
void free_fleet_signal_contracts();
void real_factory_contracts(bool configured_cadence = false, bool invalid_freestore = false);
void prepared_restore_contracts();
void authored_selection_contracts();
void mp_ai_uses_authored_faction_basic_ai();
void mp_ai_uses_agreed_difficulty_mapping();
void mp_ai_preserves_human_inactive_and_ambient_absence();
void mp_ai_refuses_unsupported_playable_inputs();
} // namespace galactic_campaign_stock_test
