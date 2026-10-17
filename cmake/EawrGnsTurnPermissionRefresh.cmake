include_guard(GLOBAL)

# Carried patch for the pinned GameNetworkingSockets native ICE client (#3912).
#
# Upstream's TURN maintenance (steamnetworkingsockets_ice_client.cpp, Think_TURNMaintenance) refreshes the allocation
# at half its lifetime but sends CreatePermission only when the set of permitted peer addresses changes. RFC 5766 gives
# a permission a fixed five-minute lifetime, so about 300 s after the first remote candidate a relayed connection stops
# forwarding in BOTH directions while GNS still reports Connected (the connection timeout is minutes). The patch re-sends
# CreatePermission for the whole permitted list every min(120 s, allocation lifetime / 2), which also lets a test server
# with a short lifetime exercise it. It also repeats a Refresh or CreatePermission that the server answers with an error
# carrying a fresh nonce (438 Stale Nonce, 401): upstream treats any reply with a header as success, so one stale nonce
# ended the refresh chain and the allocation lapsed after its lifetime. Upstream master (d534c19a, 2026-10-02) has no
# fix; drop this file when a pin bump contains one (docs/gns-transport.md, "TURN permission refresh").
#
# Applied after FetchContent_Populate. Every anchor must match exactly the expected number of times, so a changed pin
# fails the configure loudly instead of silently running unpatched.
set(EAWR_GNS_TURN_PERMISSION_PATCH_ID "tpr3")

function(_eawr_gns_count_occurrences text needle out_var)
    string(LENGTH "${needle}" _needle_length)
    set(_rest "${text}")
    set(_count 0)
    while(TRUE)
        string(FIND "${_rest}" "${needle}" _position)
        if(_position EQUAL -1)
            break()
        endif()
        math(EXPR _count "${_count} + 1")
        math(EXPR _next "${_position} + ${_needle_length}")
        string(SUBSTRING "${_rest}" ${_next} -1 _rest)
    endwhile()
    set(${out_var} ${_count} PARENT_SCOPE)
endfunction()

function(_eawr_gns_replace_anchor text_var file_label anchor replacement expected_count)
    set(_text "${${text_var}}")
    _eawr_gns_count_occurrences("${_text}" "${anchor}" _found)
    if(NOT _found EQUAL ${expected_count})
        message(FATAL_ERROR "GNS TURN permission patch: anchor in ${file_label} matched ${_found} time(s), expected "
            "${expected_count}: ${anchor}\nThe pinned GameNetworkingSockets changed; re-check the upstream TURN "
            "permission refresh and update or drop cmake/EawrGnsTurnPermissionRefresh.cmake.")
    endif()
    string(REPLACE "${anchor}" "${replacement}" _text "${_text}")
    set(${text_var} "${_text}" PARENT_SCOPE)
endfunction()

# A warm build tree keeps the fetched source (and the FetchContent stamps that say it was extracted) across configures,
# and the patch below marks its work with a marker that is the same in every revision. A source patched by an older
# revision (tpr1 had no TURN error helper) would otherwise stay on the old behaviour while the build prefix names the new
# id, or, once that is detected, fail every configure until someone cleans the tree. Called before the source is
# populated: when the source holds a complete patch of an older revision, remove it and the extract stamps, so the
# populate step extracts the cached archive again and the patches apply to a pristine tree. Anything else (a pristine
# source, the current revision, a partial patch) is left alone; the patch function still fails loudly on a partial one.
function(eawr_gns_discard_outdated_patched_source source_dir stamp_dir)
    set(_cpp_path "${source_dir}/src/steamnetworkingsockets/clientlib/steamnetworkingsockets_ice_client.cpp")
    set(_hdr_path "${source_dir}/src/steamnetworkingsockets/clientlib/steamnetworkingsockets_ice_client.h")
    if(NOT EXISTS "${_cpp_path}" OR NOT EXISTS "${_hdr_path}")
        return()
    endif()
    file(READ "${_cpp_path}" _cpp)
    file(READ "${_hdr_path}" _hdr)
    set(_marker "EAWR-PATCH turn-permission-refresh")
    string(FIND "${_cpp}" "${_marker}" _cpp_marked)
    string(FIND "${_hdr}" "${_marker}" _hdr_marked)
    string(FIND "${_cpp}" "EawrTURNErrorReply" _has_error_helper)
    if(_cpp_marked EQUAL -1 OR _hdr_marked EQUAL -1 OR NOT _has_error_helper EQUAL -1)
        return()
    endif()
    message(STATUS "GameNetworkingSockets: ${source_dir} was patched by an older revision of the TURN permission "
        "patch; extracting a pristine source again")
    file(REMOVE_RECURSE "${source_dir}" "${stamp_dir}")
endfunction()

function(eawr_gns_patch_turn_permission_refresh source_dir)
    set(_directory "${source_dir}/src/steamnetworkingsockets/clientlib")
    set(_cpp_path "${_directory}/steamnetworkingsockets_ice_client.cpp")
    set(_hdr_path "${_directory}/steamnetworkingsockets_ice_client.h")
    foreach(_path "${_cpp_path}" "${_hdr_path}")
        if(NOT EXISTS "${_path}")
            message(FATAL_ERROR "GNS TURN permission patch: ${_path} is missing from the pinned GameNetworkingSockets")
        endif()
    endforeach()
    file(READ "${_cpp_path}" _cpp)
    file(READ "${_hdr_path}" _hdr)
    set(_marker "EAWR-PATCH turn-permission-refresh")
    string(FIND "${_cpp}" "${_marker}" _cpp_marked)
    string(FIND "${_hdr}" "${_marker}" _hdr_marked)
    if(NOT _cpp_marked EQUAL -1 AND NOT _hdr_marked EQUAL -1)
        # The marker is the same in every revision of this patch: a source patched by an older one (tpr1 had no error
        # helper) would silently stay on the old behaviour while the build prefix names the new id.
        # EawrGns.cmake discards such a source before it is populated, so seeing one here means the source directory
        # was supplied from outside (FETCHCONTENT_SOURCE_DIR_EAWR_GNS_SOURCE) and cannot be replaced by us.
        string(FIND "${_cpp}" "EawrTURNErrorReply" _has_error_helper)
        if(_has_error_helper EQUAL -1)
            message(FATAL_ERROR "GNS TURN permission patch: ${source_dir} was patched by an older revision of "
                "cmake/EawrGnsTurnPermissionRefresh.cmake; use a pristine GameNetworkingSockets source directory")
        endif()
        return()
    endif()
    if(NOT _cpp_marked EQUAL -1 OR NOT _hdr_marked EQUAL -1)
        message(FATAL_ERROR "GNS TURN permission patch: ${source_dir} is only partly patched; delete the fetched "
            "GameNetworkingSockets source directory and configure again")
    endif()

    # The replacement texts are built with explicit \n escapes: a CRLF checkout of this file must not put CR into the
    # LF-only GNS sources.
    string(CONCAT _hdr_fields
        "int m_nTURNPermissionRevision = 0;\n"
        "\n"
        "        // ${_marker}: CreatePermission is repeated while the connection lives, because the server drops\n"
        "        // a permission 300 s after it was last created or refreshed (RFC 5766, section 8).\n"
        "        SteamNetworkingMicroseconds m_usecPermissionRefreshAfter = 0;\n"
        "        SteamNetworkingMicroseconds m_usecPermissionRefreshEvery = 120000000;\n"
        "        // ${_marker}: consecutive error replies answered by adopting the nonce they carried.\n"
        "        int m_nEawrTURNAuthRetries = 0;")
    _eawr_gns_replace_anchor(_hdr "ice_client.h" "int m_nTURNPermissionRevision = 0;" "${_hdr_fields}" 1)

    string(CONCAT _declaration
        "void STUNRequestCallback_CreatePermission( const RecvSTUNPktInfo_t &info );\n"
        "        // ${_marker}: 0 = success reply, 1 = error reply with a new nonce adopted (send again), 2 = other error.\n"
        "        int EawrTURNErrorReply( ICESessionInterface *pIntf, const RecvSTUNPktInfo_t &info );")
    _eawr_gns_replace_anchor(_hdr "ice_client.h"
        "void STUNRequestCallback_CreatePermission( const RecvSTUNPktInfo_t &info );" "${_declaration}" 1)

    # Both the Allocate and the Refresh response carry the allocation lifetime; derive the permission cadence from it.
    string(CONCAT _cadence
        "pIntf->m_usecRefreshAfter = info.m_usecNow + usecLifetime / 2;\n"
        "        {\n"
        "            // ${_marker}\n"
        "            SteamNetworkingMicroseconds usecEvery = usecLifetime / 2;\n"
        "            if ( usecEvery > 120000000 ) usecEvery = 120000000;\n"
        "            if ( usecEvery < 1000000 ) usecEvery = 1000000;\n"
        "            pIntf->m_usecPermissionRefreshEvery = usecEvery;\n"
        "        }")
    _eawr_gns_replace_anchor(_cpp "ice_client.cpp"
        "pIntf->m_usecRefreshAfter = info.m_usecNow + usecLifetime / 2;" "${_cadence}" 2)

    string(CONCAT _due
        "// ${_marker}: a due refresh makes the whole permitted list stale again.\n"
        "        if ( pIntf->m_usecPermissionRefreshAfter != 0 && usecNow >= pIntf->m_usecPermissionRefreshAfter )\n"
        "        {\n"
        "            pIntf->m_usecPermissionRefreshAfter = 0;\n"
        "            pIntf->m_nTURNPermissionRevision = 0;\n"
        "        }\n"
        "        if ( pIntf->m_nTURNPermissionRevision >= nSessionRevision || vecPermitted.empty() )")
    _eawr_gns_replace_anchor(_cpp "ice_client.cpp"
        "if ( pIntf->m_nTURNPermissionRevision >= nSessionRevision || vecPermitted.empty() )" "${_due}" 1)

    string(CONCAT _installed
        "pIntf->m_nTURNPermissionRevision = info.m_pRequest->m_nTURNPermissionRevision;\n"
        "    pIntf->m_usecPermissionRefreshAfter = info.m_usecNow + pIntf->m_usecPermissionRefreshEvery; // ${_marker}")
    _eawr_gns_replace_anchor(_cpp "ice_client.cpp"
        "pIntf->m_nTURNPermissionRevision = info.m_pRequest->m_nTURNPermissionRevision;" "${_installed}" 1)

    # A TURN server that rotates its nonce answers the next Refresh or CreatePermission with 438 Stale Nonce plus the new
    # one. Upstream's callbacks only look for a response header, so that error counted as a refreshed allocation and the
    # Refresh was never re-armed. The helper adopts the nonce (and a changed realm with its long-term key); the callbacks
    # then send the same request again at the next Think, at most five times in a row.
    string(CONCAT _helper
        "// ${_marker}: classify a TURN reply that has a header. 0 = success, 1 = 401/438 with a new nonce adopted, 2 = other error.\n"
        "int CSteamNetworkingICESession::EawrTURNErrorReply( ICESessionInterface *pIntf, const RecvSTUNPktInfo_t &info )\n"
        "{\n"
        "    const STUNAttribute *pErrorAttr = FindAttributeOfType( info.m_pAttributes, info.m_nAttributes, k_nSTUN_Attr_ErrorCode );\n"
        "    if ( pErrorAttr == nullptr || pErrorAttr->m_nLength < 4 )\n"
        "    {\n"
        "        pIntf->m_nEawrTURNAuthRetries = 0;\n"
        "        return 0;\n"
        "    }\n"
        "    const uint8 *pErrBytes = reinterpret_cast<const uint8*>( pErrorAttr->m_pData );\n"
        "    const int nErrorCode = pErrBytes[2] * 100 + pErrBytes[3];\n"
        "    const STUNAttribute *pRealmAttr = FindAttributeOfType( info.m_pAttributes, info.m_nAttributes, k_nSTUN_Attr_Realm );\n"
        "    const STUNAttribute *pNonceAttr = FindAttributeOfType( info.m_pAttributes, info.m_nAttributes, k_nSTUN_Attr_Nonce );\n"
        "    const int nSrvIdx = index_of( m_vecTURNServers, pIntf->m_addrTURNServer );\n"
        "    if ( ( nErrorCode == 438 || nErrorCode == 401 ) && pNonceAttr != nullptr && nSrvIdx >= 0 && !pIntf->m_strTURNRealm.empty()\n"
        "        && pIntf->m_nEawrTURNAuthRetries < 5 )\n"
        "    {\n"
        "        ++pIntf->m_nEawrTURNAuthRetries;\n"
        "        pIntf->m_strTURNNonce.assign( reinterpret_cast<const char*>( pNonceAttr->m_pData ), pNonceAttr->m_nLength );\n"
        "        if ( pRealmAttr != nullptr && pRealmAttr->m_nLength > 0\n"
        "            && pIntf->m_strTURNRealm != std::string( reinterpret_cast<const char*>( pRealmAttr->m_pData ), pRealmAttr->m_nLength ) )\n"
        "        {\n"
        "            const TURNCredentials &cred = m_vecTURNCredentials[ nSrvIdx ];\n"
        "            pIntf->m_strTURNRealm.assign( reinterpret_cast<const char*>( pRealmAttr->m_pData ), pRealmAttr->m_nLength );\n"
        "            std::string strKeyInput = cred.m_strUsername + \":\" + pIntf->m_strTURNRealm + \":\" + cred.m_strPassword;\n"
        "            CCrypto::GenerateMD5Digest( strKeyInput.c_str(), strKeyInput.size(), &pIntf->m_arrTURNKey );\n"
        "        }\n"
        "        SpewMsg( \"ICE: TURN server %s answered %d, repeating the request with its new nonce\\n\",\n"
        "            CUtlNetAdrRender( pIntf->m_addrTURNServer ).String(), nErrorCode );\n"
        "        return 1;\n"
        "    }\n"
        "    return 2;\n"
        "}\n"
        "\n"
        "void CSteamNetworkingICESession::STUNRequestCallback_RefreshAllocation( const RecvSTUNPktInfo_t &info )")
    _eawr_gns_replace_anchor(_cpp "ice_client.cpp"
        "void CSteamNetworkingICESession::STUNRequestCallback_RefreshAllocation( const RecvSTUNPktInfo_t &info )" "${_helper}" 1)

    string(CONCAT _refresh_error
        "// ${_marker}: an error reply is not a refreshed allocation. A fresh nonce means send the Refresh again at once;\n"
        "        // any other error tries again in 30 s instead of ending the refresh chain.\n"
        "        const int nEawrReply = EawrTURNErrorReply( pIntf, info );\n"
        "        if ( nEawrReply != 0 )\n"
        "        {\n"
        "            pIntf->m_usecRefreshAfter = info.m_usecNow + ( nEawrReply == 1 ? 0 : 30000000 );\n"
        "            return;\n"
        "        }\n"
        "        // Refresh succeeded -- parse the returned LIFETIME and schedule the next one.")
    _eawr_gns_replace_anchor(_cpp "ice_client.cpp"
        "// Refresh succeeded -- parse the returned LIFETIME and schedule the next one." "${_refresh_error}" 1)

    string(CONCAT _permission_error
        "// ${_marker}: a stale nonce leaves the revision behind, so the next Think sends CreatePermission again.\n"
        "    if ( EawrTURNErrorReply( pIntf, info ) == 1 )\n"
        "        return;\n"
        "\n"
        "    // Advance to the revision that was current when we sent this request.")
    _eawr_gns_replace_anchor(_cpp "ice_client.cpp"
        "// Advance to the revision that was current when we sent this request." "${_permission_error}" 1)

    file(WRITE "${_cpp_path}" "${_cpp}")
    file(WRITE "${_hdr_path}" "${_hdr}")
    message(STATUS "GameNetworkingSockets: carried TURN permission refresh patch (${EAWR_GNS_TURN_PERMISSION_PATCH_ID}) applied")
endfunction()

# #4021: the patch above is silent about a TURN reply that is not a stale nonce, so the first silence of a relayed pair
# was unexplained. This second, separate patch adds one SpewMsg per Refresh or CreatePermission reply (success or error
# code) in the helper both callbacks already call, and one for a CreatePermission that times out. It applies on top of tpr2 whether the fetched
# source was patched by an earlier configure or not, and has its own marker so a reused build tree picks it up.
function(eawr_gns_patch_turn_diagnostics source_dir)
    set(_cpp_path "${source_dir}/src/steamnetworkingsockets/clientlib/steamnetworkingsockets_ice_client.cpp")
    if(NOT EXISTS "${_cpp_path}")
        message(FATAL_ERROR "GNS TURN diagnostics patch: ${_cpp_path} is missing from the pinned GameNetworkingSockets")
    endif()
    file(READ "${_cpp_path}" _cpp)
    set(_marker "EAWR-PATCH turn-diagnostics")
    string(FIND "${_cpp}" "${_marker}" _marked)
    if(NOT _marked EQUAL -1)
        return()
    endif()

    # Single-line anchors only: a fetched source with CRLF line endings must match them too.
    string(CONCAT _success
        "pIntf->m_nEawrTURNAuthRetries = 0;\n"
        "        // ${_marker}: one line per reply (0x0104 Refresh, 0x0108 CreatePermission success).\n"
        "        SpewMsg( \"ICE: TURN server %s accepted a request (message 0x%x)\\n\", CUtlNetAdrRender( pIntf->m_addrTURNServer ).String(),\n"
        "            info.m_pHeader != nullptr ? (unsigned)info.m_pHeader->m_nMessageType : 0u );")
    _eawr_gns_replace_anchor(_cpp "ice_client.cpp" "pIntf->m_nEawrTURNAuthRetries = 0;" "${_success}" 1)

    string(CONCAT _error
        "const int nErrorCode = pErrBytes[2] * 100 + pErrBytes[3];\n"
        "    // ${_marker}: every error reply (stale nonces are then repeated by the lines below).\n"
        "    SpewMsg( \"ICE: TURN server %s answered error %d (message 0x%x)\\n\",\n"
        "        CUtlNetAdrRender( pIntf->m_addrTURNServer ).String(), nErrorCode, (unsigned)info.m_pHeader->m_nMessageType );")
    _eawr_gns_replace_anchor(_cpp "ice_client.cpp" "const int nErrorCode = pErrBytes[2] * 100 + pErrBytes[3];" "${_error}" 1)

    # Upstream logs a Refresh that times out ("TURN Refresh timed out"), but a CreatePermission that times out returns
    # without a word (its STUN-level line is verbose and off by default).
    string(CONCAT _timeout
        "SpewMsg( \"ICE: TURN server %s did not answer CreatePermission\\n\", CUtlNetAdrRender( pIntf->m_addrTURNServer ).String() ); // ${_marker}\n"
        "        // Timed out.  m_nTURNPermissionRevision is still stale, so Think_TURNMaintenance")
    _eawr_gns_replace_anchor(_cpp "ice_client.cpp"
        "// Timed out.  m_nTURNPermissionRevision is still stale, so Think_TURNMaintenance" "${_timeout}" 1)

    file(WRITE "${_cpp_path}" "${_cpp}")
    message(STATUS "GameNetworkingSockets: carried TURN diagnostics patch applied")
endfunction()
