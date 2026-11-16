# The code id of the model facts store: a hash of the files that decide what a stored record holds and whether it is valid.
# It goes into the store's epoch (platform::model_facts_epoch_identity), so a development tree edited after its last
# configure, or two dirty trees on one commit, never read each other's records. (A build from a clean commit also carries its
# commit in the build identity, so only trees whose identity does not tell them apart need this.)
#
# The hashed set is whole directories, not a hand-kept list of files: everything under the asset readers and codec (src/assets,
# include/eawr/assets), the unit tables and their model projection (src/units, include/eawr/units), the VFS whose stamp and key
# a record carries (src/vfs, include/eawr/vfs), the store and the providers (src/platform/derived_store.cpp, model_facts.cpp,
# setup_map_facts.cpp and their headers) and the setup map facts (setup_facts.*). A header the decoder includes therefore
# cannot be missing from the set. The set is globbed at configure time; a new file in these directories is added to a
# sources.cmake, which re-runs configure. Records of other kinds depend on more than this (the catalog and the text database,
# which their own context fingerprint covers): bump `assets::model_facts_format` for a change outside it.
#
# Built, not configured: a custom command hashes the files whenever one of them changes and writes a one-function source
# (platform::model_facts_code_id). (Listing the files as CMAKE_CONFIGURE_DEPENDS makes Ninja define each as a phony output,
# which collides with the files that are also compiled.)
get_filename_component(EAWR_FACTS_CODE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(EAWR_FACTS_CODE_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/EawrFactsCodeIdScript.cmake")
set(EAWR_FACTS_CODE_DIRECTORIES src/assets include/eawr/assets src/units include/eawr/units src/vfs include/eawr/vfs)
set(EAWR_FACTS_CODE_PATTERNS
    src/platform/derived_store.cpp include/eawr/platform/derived_store.hpp
    src/platform/model_facts.cpp include/eawr/platform/model_facts.hpp
    src/platform/setup_map_facts.cpp include/eawr/platform/setup_map_facts.hpp
    src/skirmish/setup_facts.cpp src/skirmish/setup_facts.hpp include/eawr/skirmish/setup_facts.hpp)

# Returns, in OUT_VAR, the generated source to add to the target that compiles src/platform/model_facts.cpp.
function(eawr_facts_code_id_source OUT_VAR)
    set(files "")
    foreach(directory IN LISTS EAWR_FACTS_CODE_DIRECTORIES)
        file(GLOB_RECURSE found LIST_DIRECTORIES false "${EAWR_FACTS_CODE_ROOT}/${directory}/*")
        list(APPEND files ${found})
    endforeach()
    foreach(relative IN LISTS EAWR_FACTS_CODE_PATTERNS)
        if(EXISTS "${EAWR_FACTS_CODE_ROOT}/${relative}")
            list(APPEND files "${EAWR_FACTS_CODE_ROOT}/${relative}")
        endif()
    endforeach()
    # Build files do not decide a record; the generated hash must not move for them.
    list(FILTER files EXCLUDE REGEX "(CMakeLists\\.txt|\\.cmake)$")
    list(REMOVE_DUPLICATES files)
    list(SORT files)
    if(NOT files)
        message(FATAL_ERROR "EawrFactsCodeId: no files to hash under ${EAWR_FACTS_CODE_ROOT}")
    endif()
    set(list_file "${CMAKE_CURRENT_BINARY_DIR}/facts_code_files.txt")
    string(REPLACE ";" "\n" list_text "${files}")
    file(WRITE "${list_file}" "${list_text}\n")
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/facts_code_id.cpp")
    add_custom_command(
        OUTPUT "${generated}"
        COMMAND "${CMAKE_COMMAND}" "-DLIST_FILE=${list_file}" "-DOUT=${generated}" -P "${EAWR_FACTS_CODE_SCRIPT}"
        DEPENDS ${files} "${EAWR_FACTS_CODE_SCRIPT}" "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
        COMMENT "Hashing the model facts code files"
        VERBATIM)
    set(${OUT_VAR} "${generated}" PARENT_SCOPE)
endfunction()
