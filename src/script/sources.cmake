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

set(EAWR_SCRIPT_FOC_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/foc/tactical_ai.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/tactical_ai_bindings.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/tactical_ai_orders.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/tactical_ai_loading.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/ai_equations.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/ai_data.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/ai_perception.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/ai_engine.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/ai_service.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/ai_taskforces.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/ai_goals.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/ai_selection.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/ai_execution.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/ai_plans.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/foc/plan_bindings.cpp"
)
