set(EAWR_RELAY_DISABLED_SOURCES "${CMAKE_CURRENT_LIST_DIR}/relay_disabled.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/online_lobby_source.cpp")
set(EAWR_RELAY_CLIENT_SOURCES "${CMAKE_CURRENT_LIST_DIR}/relay_client.cpp")
set(EAWR_RELAY_TLS_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/relay_stream.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/relay_prefix.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/relay_https.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/relay_tls.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/relay_tls_windows.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../platform/relay_credentials.cpp")
set(EAWR_RELAY_SERVICE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/relay_server.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/relay_directory.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/relay_password.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/relay_hash_worker.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/relay_cloudflare.cpp")
if(WIN32)
    list(APPEND EAWR_RELAY_TLS_SOURCES
        "${CMAKE_CURRENT_LIST_DIR}/platform_socket_win.cpp")
else()
    list(APPEND EAWR_RELAY_TLS_SOURCES
        "${CMAKE_CURRENT_LIST_DIR}/platform_socket_posix.cpp")
endif()
