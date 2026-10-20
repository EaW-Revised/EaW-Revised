add_executable(net_transport_tests transport_tests.cpp)
target_link_libraries(net_transport_tests PRIVATE eawr::net)
eawr_target_project_warnings(net_transport_tests)
add_test(NAME net_transport_contracts COMMAND net_transport_tests)
add_executable(net_lockstep_tests
    lockstep_session_tests.cpp
    lockstep_session_agreement_tests.cpp
    lockstep_session_departure_tests.cpp)
target_link_libraries(net_lockstep_tests PRIVATE eawr::net eawr::platform eawr::ui_model)
eawr_target_project_warnings(net_lockstep_tests)
add_test(NAME net_lockstep_contracts COMMAND net_lockstep_tests)
add_executable(net_tcp_transport_tests tcp_transport_tests.cpp)
target_link_libraries(net_tcp_transport_tests PRIVATE eawr::net)
eawr_target_project_warnings(net_tcp_transport_tests)
add_test(NAME net_tcp_transport_contracts COMMAND net_tcp_transport_tests)
# LAN lobby presence and chat: the codec, the directory with a fake clock and two broadcasters over loopback UDP.
add_executable(net_lan_directory_tests lan_directory_tests.cpp)
target_link_libraries(net_lan_directory_tests PRIVATE eawr::net)
eawr_target_project_warnings(net_lan_directory_tests)
add_test(NAME net_lan_directory_contracts COMMAND net_lan_directory_tests)
add_executable(net_desync_monitor_tests desync_monitor_tests.cpp)
target_link_libraries(net_desync_monitor_tests PRIVATE eawr::net eawr::platform eawr::script_authoritative)
eawr_target_project_warnings(net_desync_monitor_tests)
add_test(NAME net_desync_monitor_contracts COMMAND net_desync_monitor_tests
    "${PROJECT_SOURCE_DIR}/tests/replay/fixtures/tactical-v2.eawr-replay"
    "${PROJECT_BINARY_DIR}/desync-artifacts")
add_executable(net_desync_session_tests desync_session_tests.cpp)
target_link_libraries(net_desync_session_tests PRIVATE eawr::net eawr::platform)
eawr_target_project_warnings(net_desync_session_tests)
add_test(NAME net_desync_session_contracts COMMAND net_desync_session_tests
    "${PROJECT_BINARY_DIR}/desync-session-artifacts")
add_executable(net_setup_tests setup_tests.cpp)
target_link_libraries(net_setup_tests PRIVATE eawr::net eawr::platform)
eawr_target_project_warnings(net_setup_tests)
add_test(NAME net_setup_contracts COMMAND net_setup_tests "${PROJECT_BINARY_DIR}/setup-artifacts")
add_executable(net_setup_tcp_tests setup_tcp_tests.cpp)
target_link_libraries(net_setup_tcp_tests PRIVATE eawr::net eawr::platform)
eawr_target_project_warnings(net_setup_tcp_tests)
add_test(NAME net_setup_tcp_contracts COMMAND net_setup_tcp_tests "${PROJECT_BINARY_DIR}/setup-tcp-artifacts")
# #3768: the galactic-campaign-v1 setup offer (roster codec, two-peer and eight-seat agreement, every refusal before a shared step).
add_executable(net_galactic_setup_tests galactic_setup_tests.cpp)
target_link_libraries(net_galactic_setup_tests PRIVATE eawr::net eawr::platform eawr::campaign)
eawr_target_project_warnings(net_galactic_setup_tests)
add_test(NAME net_galactic_setup_contracts COMMAND net_galactic_setup_tests)
add_executable(net_viewer_lan_tests
    viewer_lan_tests.cpp
    viewer_lan_mesh_tests.cpp
    viewer_lan_staging_tests.cpp
    viewer_lan_recovery_tests.cpp)
target_include_directories(net_viewer_lan_tests PRIVATE "${PROJECT_SOURCE_DIR}/apps/viewer/src")
target_link_libraries(net_viewer_lan_tests PRIVATE eawr::platform_net eawr::net eawr::platform eawr::skirmish eawr::ui_model eawr::scene)
eawr_target_project_warnings(net_viewer_lan_tests)
add_test(NAME net_viewer_lan_contracts COMMAND net_viewer_lan_tests)
add_test(NAME net_pending_input_contracts COMMAND net_viewer_lan_tests --pending-inputs)
add_executable(net_tcp_lockstep_tests tcp_lockstep_tests.cpp)
target_link_libraries(net_tcp_lockstep_tests PRIVATE eawr::net eawr::platform)
eawr_target_project_warnings(net_tcp_lockstep_tests)
add_test(NAME net_tcp_lockstep_battle COMMAND net_tcp_lockstep_tests)
add_executable(net_gns_dependency_smoke gns_dependency/smoke.cpp)
target_link_libraries(net_gns_dependency_smoke PRIVATE eawr::net)
target_compile_features(net_gns_dependency_smoke PRIVATE cxx_std_20)
eawr_target_project_warnings(net_gns_dependency_smoke)
add_test(NAME net_gns_dependency_smoke COMMAND net_gns_dependency_smoke)
set_tests_properties(net_gns_dependency_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)

find_package(Threads REQUIRED)
add_executable(net_gns_link_stats_tests gns_link_stats_tests.cpp)
target_link_libraries(net_gns_link_stats_tests PRIVATE eawr::net)
eawr_target_project_warnings(net_gns_link_stats_tests)
add_test(NAME net_gns_link_stats_contracts COMMAND net_gns_link_stats_tests)

add_executable(net_gns_frame_codec_tests gns_frame_codec_tests.cpp)
target_link_libraries(net_gns_frame_codec_tests PRIVATE eawr::net)
eawr_target_project_warnings(net_gns_frame_codec_tests)
add_test(NAME net_gns_frame_codec_contracts COMMAND net_gns_frame_codec_tests)

add_executable(net_gns_introduced_tests gns_introduced_tests.cpp)
target_link_libraries(net_gns_introduced_tests PRIVATE eawr::net)
eawr_target_project_warnings(net_gns_introduced_tests)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME net_gns_introduced_paths COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/gns_introduced_processes.py" "$<TARGET_FILE:net_gns_introduced_tests>")
set_tests_properties(net_gns_introduced_paths PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

# #3912: a relayed pair through a TURN server whose permissions and allocations expire after a few seconds. GNS must
# keep refreshing its permission; before the carried patch (cmake/EawrGnsTurnPermissionRefresh.cmake) the relay went
# silent after one permission lifetime while the connection still reported Connected.
add_test(NAME net_gns_turn_permission COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/gns_turn_processes.py" "$<TARGET_FILE:net_gns_introduced_tests>" permission)
set_tests_properties(net_gns_turn_permission PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 150)
# The same server turns into a dead path: the link must name it (EAWR-NET-1507) long before GNS's own timeout.
add_test(NAME net_gns_turn_silent COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/gns_turn_processes.py" "$<TARGET_FILE:net_gns_introduced_tests>" silent)
set_tests_properties(net_gns_turn_silent PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 150)
# The server rotates its nonce: every Refresh and CreatePermission is answered with 438 Stale Nonce first and must be
# repeated with the new one (the pinned client treated that reply as success and never refreshed again).
add_test(NAME net_gns_turn_nonce COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/gns_turn_processes.py" "$<TARGET_FILE:net_gns_introduced_tests>" nonce)
set_tests_properties(net_gns_turn_nonce PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 150)

# N-peer room mesh (N2): member processes meshed through a loopback room broker. The nine-member case skips (77)
# unless EAWR_GNS_MESH_NINE=1; it runs once per slice at its final head, not per PR.
add_executable(net_gns_mesh_tests gns_mesh_tests.cpp)
target_link_libraries(net_gns_mesh_tests PRIVATE eawr::net eawr::platform Threads::Threads)
eawr_target_project_warnings(net_gns_mesh_tests)
add_test(NAME net_gns_mesh_paths COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/gns_mesh_processes.py" "$<TARGET_FILE:net_gns_mesh_tests>")
set_tests_properties(net_gns_mesh_paths PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
add_test(NAME net_gns_lobby_retry COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/gns_mesh_processes.py" "$<TARGET_FILE:net_gns_mesh_tests>" lobby_retry)
set_tests_properties(net_gns_lobby_retry PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
add_test(NAME net_gns_mesh_nine COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/gns_mesh_processes.py" "$<TARGET_FILE:net_gns_mesh_tests>" nine)
set_tests_properties(net_gns_mesh_nine PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
# #4021: six members, a pair silent twice 36 s apart (repaired both times), and the same with seat 5 unable to re-attach.
add_test(NAME net_gns_mesh_twice COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/gns_mesh_processes.py" "$<TARGET_FILE:net_gns_mesh_tests>" twice)
set_tests_properties(net_gns_mesh_twice PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
# The second acceptor arms its repair 3 s after the dialer has dialled with the new epoch's material.
add_test(NAME net_gns_mesh_twice_late COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/gns_mesh_processes.py" "$<TARGET_FILE:net_gns_mesh_tests>" twice_late)
set_tests_properties(net_gns_mesh_twice_late PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
# A pair the acceptor gave up on is not revived by the dialer's late request; only one of its two seats leaves.
add_test(NAME net_gns_mesh_gave_up COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/gns_mesh_processes.py" "$<TARGET_FILE:net_gns_mesh_tests>" gave_up)
set_tests_properties(net_gns_mesh_gave_up PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
# Explicit final-head qualification; ordinary ^net_ selections keep the env-gated test above.
add_test(NAME gns_mesh_nine_process COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/gns_mesh_processes.py" "$<TARGET_FILE:net_gns_mesh_tests>" nine)
set_tests_properties(gns_mesh_nine_process PROPERTIES ENVIRONMENT "EAWR_GNS_MESH_NINE=1" SKIP_RETURN_CODE 77 TIMEOUT 600)

add_executable(net_gns_signalling_tests gns_signalling_tests.cpp)
target_link_libraries(net_gns_signalling_tests PRIVATE eawr::net Threads::Threads)
eawr_target_project_warnings(net_gns_signalling_tests)
add_test(NAME net_gns_signalling_contracts COMMAND net_gns_signalling_tests)

add_executable(net_gns_transport_tests gns_transport_tests.cpp)
target_link_libraries(net_gns_transport_tests PRIVATE eawr::net)
eawr_target_project_warnings(net_gns_transport_tests)
add_test(NAME net_gns_transport_contracts COMMAND net_gns_transport_tests)
set_tests_properties(net_gns_transport_contracts PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
add_test(NAME net_gns_turn_refusal_contracts COMMAND net_gns_transport_tests --turn-refusals)
set_tests_properties(net_gns_turn_refusal_contracts PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)

# Reuse the same assertions and fixtures over real GNS loopback UDP.
foreach(case setup lockstep desync)
    if(case STREQUAL "setup")
        set(source setup_tcp_tests.cpp)
        set(argument "${PROJECT_BINARY_DIR}/setup-gns-artifacts")
    elseif(case STREQUAL "lockstep")
        set(source tcp_lockstep_tests.cpp)
        set(argument "")
    else()
        set(source desync_session_tests.cpp)
        set(argument "${PROJECT_BINARY_DIR}/desync-gns-artifacts")
    endif()
    add_executable(net_gns_${case}_tests ${source})
    target_compile_definitions(net_gns_${case}_tests PRIVATE EAWR_TEST_GNS=1)
    target_link_libraries(net_gns_${case}_tests PRIVATE eawr::net eawr::platform)
    eawr_target_project_warnings(net_gns_${case}_tests)
    add_test(NAME net_gns_${case}_contracts COMMAND net_gns_${case}_tests ${argument})
    set_tests_properties(net_gns_${case}_contracts PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
endforeach()

add_executable(net_mesh_tests mesh_tests.cpp)
target_link_libraries(net_mesh_tests PRIVATE eawr::net eawr::platform)
eawr_target_project_warnings(net_mesh_tests)
add_test(NAME net_mesh_contracts COMMAND net_mesh_tests)
add_executable(net_mesh_policy_tests mesh_policy_tests.cpp)
target_link_libraries(net_mesh_policy_tests PRIVATE eawr::net Threads::Threads)
eawr_target_project_warnings(net_mesh_policy_tests)
add_test(NAME net_mesh_policy_contracts COMMAND net_mesh_policy_tests)
# N-peer pair-loss agreement (N4): the incident driver, then LockstepSession with cuttable pairs, then the same over the real
# tactical simulation with 1/2/4/8 sim workers. None needs GNS or a service.
add_executable(net_mesh_incident_tests mesh_incident_tests.cpp)
target_link_libraries(net_mesh_incident_tests PRIVATE eawr::net Threads::Threads)
eawr_target_project_warnings(net_mesh_incident_tests)
add_test(NAME net_mesh_incident_contracts COMMAND net_mesh_incident_tests)
set_tests_properties(net_mesh_incident_contracts PROPERTIES TIMEOUT 300)
# Relay churn at six and nine seats (#4021): silent pairs during a pending restoration, churn that outlasts a window, a dead pair
# amid churn, and seeded cascades. In-process, no GNS.
add_executable(net_mesh_cascade_tests mesh_cascade_tests.cpp)
target_link_libraries(net_mesh_cascade_tests PRIVATE eawr::net Threads::Threads)
eawr_target_project_warnings(net_mesh_cascade_tests)
add_test(NAME net_mesh_cascade_contracts COMMAND net_mesh_cascade_tests)
add_test(NAME net_mesh_silent_seat_contracts COMMAND net_mesh_cascade_tests silent-seat)
set_tests_properties(net_mesh_silent_seat_contracts PROPERTIES TIMEOUT 600)
set_tests_properties(net_mesh_cascade_contracts PROPERTIES TIMEOUT 600)
add_executable(net_mesh_session_tests mesh_session_tests.cpp)
target_link_libraries(net_mesh_session_tests PRIVATE eawr::net eawr::platform Threads::Threads)
eawr_target_project_warnings(net_mesh_session_tests)
add_test(NAME net_mesh_session_contracts COMMAND net_mesh_session_tests)
set_tests_properties(net_mesh_session_contracts PROPERTIES TIMEOUT 600)
add_executable(net_mesh_session_workers_tests mesh_session_workers_tests.cpp)
target_link_libraries(net_mesh_session_workers_tests PRIVATE eawr::net eawr::platform Threads::Threads)
eawr_target_project_warnings(net_mesh_session_workers_tests)
add_test(NAME net_mesh_session_worker_contracts COMMAND net_mesh_session_workers_tests)
set_tests_properties(net_mesh_session_worker_contracts PROPERTIES TIMEOUT 300)
add_executable(net_port_mapping_tests port_mapping_tests.cpp)
target_link_libraries(net_port_mapping_tests PRIVATE eawr::net)
eawr_target_project_warnings(net_port_mapping_tests)
add_test(NAME net_port_mapping_contracts COMMAND net_port_mapping_tests)
add_executable(net_reconnect_tests reconnect_tests.cpp)
target_link_libraries(net_reconnect_tests PRIVATE eawr::net eawr::platform)
eawr_target_project_warnings(net_reconnect_tests)
add_test(NAME net_reconnect_contracts COMMAND net_reconnect_tests)
add_executable(net_online_lobby_source_tests online_lobby_source_tests.cpp)
target_include_directories(net_online_lobby_source_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/net")
target_link_libraries(net_online_lobby_source_tests PRIVATE eawr::relay)
eawr_target_project_warnings(net_online_lobby_source_tests)
add_test(NAME net_online_lobby_source_contracts COMMAND net_online_lobby_source_tests)
set_tests_properties(net_online_lobby_source_contracts PROPERTIES TIMEOUT 30)
add_executable(net_relay_disabled_tests relay_disabled_tests.cpp)
target_link_libraries(net_relay_disabled_tests PRIVATE eawr::relay)
eawr_target_project_warnings(net_relay_disabled_tests)
add_test(NAME net_relay_disabled_contracts COMMAND net_relay_disabled_tests)
if(EAWR_ENABLE_RELAY)
    add_executable(net_viewer_online_tests viewer_online_tests.cpp)
    target_include_directories(net_viewer_online_tests PRIVATE "${PROJECT_SOURCE_DIR}/apps/viewer/src")
    target_link_libraries(net_viewer_online_tests PRIVATE eawr::platform_net eawr::net eawr::platform eawr::skirmish eawr::ui_model)
    eawr_target_project_warnings(net_viewer_online_tests)
    # The process harness must launch the relay built from this same checkout,
    # including when an offload job selects only this test target.
    add_dependencies(net_viewer_online_tests eawr_relay_server)
    add_test(NAME net_viewer_online_contracts COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/viewer_online_processes.py" "$<TARGET_FILE:net_viewer_online_tests>"
        "$<TARGET_FILE:eawr_relay_server>" "${PROJECT_BINARY_DIR}/online-bridge-artifacts")
    set_tests_properties(net_viewer_online_contracts PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 150)
    add_executable(net_relay_server_tests relay_server_tests.cpp)
    target_include_directories(net_relay_server_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/net")
    target_link_libraries(net_relay_server_tests PRIVATE eawr_relay_service Threads::Threads)
    eawr_target_project_warnings(net_relay_server_tests)
    add_test(NAME net_relay_server_contracts COMMAND net_relay_server_tests "${PROJECT_BINARY_DIR}/relay-test-artifacts")
    set_tests_properties(net_relay_server_contracts PROPERTIES TIMEOUT 180)
    add_executable(net_relay_cloudflare_tests relay_cloudflare_tests.cpp)
    target_include_directories(net_relay_cloudflare_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/net")
    target_link_libraries(net_relay_cloudflare_tests PRIVATE eawr_relay_service Threads::Threads)
    eawr_target_project_warnings(net_relay_cloudflare_tests)
    add_test(NAME net_relay_cloudflare_contracts COMMAND net_relay_cloudflare_tests "${PROJECT_BINARY_DIR}/relay-cloudflare-artifacts")
    set_tests_properties(net_relay_cloudflare_contracts PROPERTIES TIMEOUT 90)
    add_executable(net_relay_room_tests relay_room_tests.cpp)
    target_include_directories(net_relay_room_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/net")
    target_link_libraries(net_relay_room_tests PRIVATE eawr_relay_service Threads::Threads)
    eawr_target_project_warnings(net_relay_room_tests)
    add_test(NAME net_relay_room_contracts COMMAND net_relay_room_tests "${PROJECT_BINARY_DIR}/relay-room-artifacts")
    set_tests_properties(net_relay_room_contracts PROPERTIES TIMEOUT 180)
    add_test(NAME net_relay_server_process COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/relay_server_processes.py" "$<TARGET_FILE:eawr_relay_server>"
        "${PROJECT_BINARY_DIR}/relay-process-artifacts")
    set_tests_properties(net_relay_server_process PROPERTIES TIMEOUT 60)
    # Stages only the relay Dockerfile's build-stage COPY inputs and runs its cmake steps there (Linux; skips elsewhere).
    add_test(NAME net_relay_dockerfile_stage COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/relay_dockerfile_stage.py" "${PROJECT_SOURCE_DIR}")
    set_tests_properties(net_relay_dockerfile_stage PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
endif()
add_executable(net_lan_credentials_tests lan_credentials_tests.cpp)
target_link_libraries(net_lan_credentials_tests PRIVATE eawr::net eawr::platform)
eawr_target_project_warnings(net_lan_credentials_tests)
add_test(NAME net_lan_credentials_contracts COMMAND net_lan_credentials_tests)
add_executable(net_lobby_session_tests lobby_session_tests.cpp)
target_link_libraries(net_lobby_session_tests PRIVATE eawr::net eawr::platform)
eawr_target_project_warnings(net_lobby_session_tests)
add_test(NAME net_lobby_session_contracts COMMAND net_lobby_session_tests)

add_executable(net_galactic_adapter_tests galactic_adapter_tests.cpp)
target_link_libraries(net_galactic_adapter_tests PRIVATE eawr::net_galactic eawr::platform)
eawr_target_project_warnings(net_galactic_adapter_tests)
add_test(NAME net_galactic_adapter_contracts COMMAND net_galactic_adapter_tests)
set_tests_properties(net_galactic_adapter_contracts PROPERTIES TIMEOUT 600)

add_executable(net_galactic_live_driver_tests galactic_live_driver_tests.cpp)
target_link_libraries(net_galactic_live_driver_tests PRIVATE eawr::platform_net_galactic eawr::net_galactic eawr::platform)
eawr_target_project_warnings(net_galactic_live_driver_tests)
add_test(NAME net_galactic_live_driver_contracts COMMAND net_galactic_live_driver_tests)
set_tests_properties(net_galactic_live_driver_contracts PROPERTIES TIMEOUT 600)

eawr_label_tests()
