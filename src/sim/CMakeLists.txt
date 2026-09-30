add_subdirectory(
    "${PROJECT_SOURCE_DIR}/third_party/entt"
    "${PROJECT_BINARY_DIR}/third_party/entt"
    EXCLUDE_FROM_ALL
)

add_library(
    eawr_sim
    STATIC
        fog.cpp
        fog_sidecar.cpp
        replay.cpp
        snapshot.cpp
        state_hash.cpp
        tactical/abilities.cpp
        tactical/collection.cpp
        tactical/collision.cpp
        tactical/damage.cpp
        tactical/projectiles.cpp
        tactical/durability.cpp
        tactical/motion.cpp
        tactical/fighters.cpp
        tactical/death_spin.cpp
        tactical/combat.cpp
        tactical/orders.cpp
        tactical/formation.cpp
        tactical/pathfind.cpp
        tactical/encoding.cpp
        tactical/fog_cells.cpp
        tactical/ion.cpp
        tactical/replay.cpp
        tactical/session.cpp
        tactical/space.cpp
        tactical/victory.cpp
        tactical/visibility.cpp
        world.cpp
        math/fixed.cpp
        math/geometry.cpp
        math/trig.cpp
        math/wide.cpp
)
add_library(eawr::sim ALIAS eawr_sim)

target_include_directories(
    eawr_sim
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_link_libraries(eawr_sim PUBLIC eawr::core PRIVATE EnTT::EnTT)
target_compile_features(eawr_sim PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_sim)
