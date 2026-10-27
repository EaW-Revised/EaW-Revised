# Shared translation units for the root libraries and the viewer.
# Paths are absolute so both projects can include this list directly.

set(EAWR_SCRIPT_NUMERIC_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/numeric/binary64.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/numeric/decimal.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/numeric/decimal_parse.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/numeric/decimal_format.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/numeric/q24_boundary.cpp"
)

set(EAWR_SFLUA_HOOKS_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/sflua/sflua_hooks.cpp"
)

set(EAWR_SFLUA_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/sflua/sflua_state.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/sflua_sandbox.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/sflua_persist.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/sflua_sandbox_values.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/sflua_sandbox_calls.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/sflua_persist_graph.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/sflua_persist_encode.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/sflua_persist_decode.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/upstream/lapi.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/upstream/lcode.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/upstream/ldebug.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/upstream/ldo.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/upstream/ldump.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/upstream/lfunc.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/upstream/lgc.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/upstream/llex.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/upstream/lmem.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/upstream/lobject.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/upstream/lopcodes.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/upstream/lparser.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/upstream/lstate.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/upstream/lstring.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/upstream/ltable.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/upstream/ltm.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/upstream/lundump.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/upstream/lvm.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/upstream/lzio.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/upstream/lauxlib.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/upstream/lbaselib.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/upstream/lstrlib.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sflua/upstream/ltablib.cpp"
)

set(EAWR_SCRIPT_AUTHORITATIVE_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/authoritative/bindings.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/authoritative/clock_rng.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/authoritative/persistence.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/authoritative/scheduler.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/authoritative/tactical_bridge.cpp"
)

set(EAWR_SCRIPT_GALACTIC_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/authoritative/galactic_bridge.cpp"
)

set(EAWR_SCRIPT_FOC_SOURCES
    # WGA-56/139: lane A lifecycle contributor; uses the shared native envelope.
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_lifecycle_state.cpp"
    # WGA-56: live goal identities and exact native Engine continuation.
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_goal_runtime.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_goal_state.cpp"
    # WGA-56: canonical native AI state and full retained funding DTO.
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_state.cpp"
    # WGA-57..60: galactic goal/producer/selector components.
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_goals.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_estimates.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_selection.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_selector_runtime.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_assembly.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_selection_content.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_plan_materialization.cpp"
    # WGA-25/38/42: stock plan definition and pacing data.
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_plans.cpp"
    # WGA-01..05/09: lane A galactic manager, validated data and templates.
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_loading.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_templates.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_funding.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/tactical_ai.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/tactical_ai_bindings.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/tactical_ai_orders.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/tactical_ai_loading.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/ai_equations.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/ai_data.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/ai_mode.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/ai_perception.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/ai_script_evaluators.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/ai_engine.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/ai_service.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/ai_taskforces.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/ai_goals.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/ai_selection.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/ai_execution.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/ai_plans.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/plan_bindings.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_execution.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_actions.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_economic_replay.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_freestore.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_execution_state.cpp"
    # WGA-06/62/65/80 and FT-05/08: copied campaign perception providers.
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_reader.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_world.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_economy.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_perception.cpp"
    # WGA-181..195: force tokens and owned perception state contributor.
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_forces.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_perception_state.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_markup.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_markup_state.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_markup_bindings.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_reach.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/galactic_ai_context.cpp"
)
