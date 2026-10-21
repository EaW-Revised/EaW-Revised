include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")
add_library(eawr_platform_net STATIC ${EAWR_PLATFORM_NET_SOURCES})
add_library(eawr::platform_net ALIAS eawr_platform_net)
target_include_directories(eawr_platform_net PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(eawr_platform_net PUBLIC eawr::net eawr::platform eawr::skirmish eawr::ui_model eawr::scene eawr::campaign)
target_compile_features(eawr_platform_net PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_platform_net)

# GCMP-03 (#4359): the galaxy LAN lockstep driver for LiveGameSession. Kept out of EAWR_PLATFORM_NET_SOURCES (the viewer
# compiles that list) until the GC lobby and screens (GCMP-07/08) consume it.
add_library(eawr_platform_net_galactic STATIC "${CMAKE_CURRENT_LIST_DIR}/galactic_lan_match.cpp")
add_library(eawr::platform_net_galactic ALIAS eawr_platform_net_galactic)
target_include_directories(eawr_platform_net_galactic PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(eawr_platform_net_galactic PUBLIC eawr::net_galactic eawr::net eawr::platform)
target_compile_features(eawr_platform_net_galactic PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_platform_net_galactic)

# The lobby launch retains/adopts the galactic driver.
target_link_libraries(eawr_platform_net PUBLIC eawr::platform_net_galactic)
