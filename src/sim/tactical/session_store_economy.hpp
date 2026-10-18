#pragma once

#include "session_types.hpp"

namespace eawr::sim::tactical::session_detail {

// Ordered fragments preserve the session's original member lifetime order.
struct SessionStoreIncomeService {
    struct IncomeServiceRef {
        PlayerId owner{};
        EntityId object{};
        std::size_t slot{};
        const IncomeModifier* profile{};
        IncomeModifierState* state{};
        bool residual{};
    };
    std::vector<IncomeServiceRef> income_service_refs;
};

struct SessionStorePads {
    std::map<EntityId, PadState> pads;
    std::map<EntityId, ConstructionState> construction;
    ObjectStage<PadState> pad_stage;
    ObjectStage<ConstructionState> construction_stage;
    std::vector<CaptureJob> capture_jobs; // pad-sized, retained; no world-sized outputs
    std::vector<CaptureCandidate> capture_inputs;
    std::array<CaptureScratch, tick_partition_count> capture_scratch;
};

struct SessionStoreEconomy {
    std::vector<PlayerEconomy> ledgers;             // #530: per economy player, ascending ID
    std::map<EntityId, ArrivalState> arrivals;      // #530: arriving units (PU-35)
    std::map<EntityId, PopulationShare> shares;     // #530: reinforced units' population (PU-21)
    std::vector<std::pair<EntityId, PlayerId>> earners; // #530: live income stations, ascending ID
    std::vector<IncomePayment> income_payments; // retained partition outputs, never canonical state
    std::map<EntityId, RespawnBatch> respawns; // due frame -> creation requests, in death order
};

} // namespace eawr::sim::tactical::session_detail
