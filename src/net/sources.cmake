# Shared translation units; absolute paths also support the viewer build.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/EawrRelay.cmake")
set(EAWR_NET_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/transport.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/lockstep_session.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/lockstep_session_input.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/lockstep_session_timing.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/lockstep_session_departure.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/lockstep_session_mesh.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/lockstep_session_reconnect.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/lockstep_session_resync.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/lockstep_session_digest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tactical_adapter.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tcp_transport.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/gns_transport.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/gns_transport_impl.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/gns_transport_mesh.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/gns_transport_formation.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/gns_transport_signalling.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/gns_signalling.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/gns_room_fixture.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/mesh_policy.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/mesh_incident.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/gns_frame_codec.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/desync_monitor.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/setup.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/galactic_setup.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/galactic_lobby.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/lan_credentials.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/lobby_session.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/lan_directory.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/lan_broadcast.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/port_mapping.cpp"
    # Native router discovery belongs to the platform I/O boundary.
    "${CMAKE_CURRENT_LIST_DIR}/../platform/port_mapping_io.cpp"
)

if(WIN32)
    list(APPEND EAWR_NET_SOURCES "${CMAKE_CURRENT_LIST_DIR}/platform_socket_win.cpp")
    set(EAWR_NET_PLATFORM_LIBRARIES ws2_32 iphlpapi)
else()
    list(APPEND EAWR_NET_SOURCES "${CMAKE_CURRENT_LIST_DIR}/platform_socket_posix.cpp")
    set(EAWR_NET_PLATFORM_LIBRARIES "")
endif()
