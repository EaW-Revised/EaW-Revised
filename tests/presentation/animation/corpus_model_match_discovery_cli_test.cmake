# Command-line refusals of animation_corpus_model_match_discovery that happen
# before any asset is read, so they need no private asset:
#   - usage errors and an output (or its .partial staging file) that aliases
#     the frozen input (exit 2);
#   - a missing, stale or malformed frozen input (exit 4);
#   - an authenticated input but no mountable game (exit 1).
# Every refusal must leave the frozen bytes and an existing output intact and
# create no other file.
#
#   cmake -DEXE=<animation_corpus_model_match_discovery> -DMANIFEST=<tracked manifest>
#         -DWORK=<scratch directory> -P corpus_model_match_discovery_cli_test.cmake
cmake_minimum_required(VERSION 3.25)
foreach(required EXE MANIFEST WORK)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/out")
set(metadata "${WORK}/frozen-metadata.tsv")
# A copy, so that even a failed refusal could never touch the tracked file.
file(COPY_FILE "${MANIFEST}" "${metadata}")
file(SHA256 "${metadata}" metadata_sha)
# The staging-alias case needs the frozen input under a .partial name.
set(staged_metadata "${WORK}/out/receipt.json.partial")
file(COPY_FILE "${MANIFEST}" "${staged_metadata}")
set(existing "${WORK}/out/existing.json")
file(WRITE "${existing}" "{\"previous\": \"receipt\"}\n")
file(SHA256 "${existing}" existing_sha)
set(game "${WORK}/no-game")
set(mod "${WORK}/no-mod")
set(fresh "${WORK}/out/fresh.json")
set(failures 0)

function(expect_refusal label code pattern)
    execute_process(COMMAND "${EXE}" ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    file(SHA256 "${metadata}" metadata_now)
    file(SHA256 "${staged_metadata}" staged_now)
    file(SHA256 "${existing}" existing_now)
    file(GLOB outputs "${WORK}/out/*")
    list(REMOVE_ITEM outputs "${existing}" "${staged_metadata}")
    set(problems "")
    if(NOT result STREQUAL "${code}")
        string(APPEND problems " exit=${result} (expected ${code})")
    endif()
    if(NOT err MATCHES "${pattern}")
        string(APPEND problems " stderr='${err}' lacks '${pattern}'")
    endif()
    if(NOT metadata_now STREQUAL metadata_sha OR NOT staged_now STREQUAL metadata_sha)
        string(APPEND problems " frozen bytes changed")
    endif()
    if(NOT existing_now STREQUAL existing_sha)
        string(APPEND problems " existing output changed")
    endif()
    if(outputs)
        string(APPEND problems " outputs created: ${outputs}")
        file(REMOVE ${outputs})
    endif()
    if(problems)
        message(SEND_ERROR "FAILED ${label}:${problems}")
        math(EXPR count "${failures} + 1")
        set(failures ${count} PARENT_SCOPE)
    else()
        message(STATUS "refused as expected: ${label}")
    endif()
endfunction()

expect_refusal("no arguments" 2 "usage")
expect_refusal("too few arguments" 2 "usage" "${game}" "${mod}" "${metadata}")
expect_refusal("too many arguments" 2 "usage" "${game}" "${mod}" "${metadata}" "${existing}" "${existing}")
expect_refusal("output = frozen metadata" 2 "name the same file" "${game}" "${mod}" "${metadata}" "${metadata}")
expect_refusal("output = frozen metadata via a dot segment" 2 "name the same file"
    "${game}" "${mod}" "${metadata}" "${WORK}/./out/../frozen-metadata.tsv")
expect_refusal("staging file = frozen metadata" 2 "name the same file"
    "${game}" "${mod}" "${staged_metadata}" "${WORK}/out/receipt.json")

file(READ "${metadata}" manifest_text)
file(WRITE "${WORK}/stale-metadata.tsv" "${manifest_text}#")
foreach(output IN ITEMS "${existing}" "${fresh}")
    expect_refusal("stale frozen metadata -> ${output}" 4 "frozen metadata is stale"
        "${game}" "${mod}" "${WORK}/stale-metadata.tsv" "${output}")
    expect_refusal("missing frozen metadata -> ${output}" 4 "frozen metadata is missing"
        "${game}" "${mod}" "${WORK}/absent.tsv" "${output}")
    # The input authenticates and selects the 22 rows; the absent installation fails.
    expect_refusal("no installation to mount -> ${output}" 1 "."
        "${game}" "${mod}" "${metadata}" "${output}")
endforeach()

if(failures GREATER 0)
    message(FATAL_ERROR "${failures} command-line refusal case(s) failed")
endif()
message(STATUS "animation model-match discovery command-line refusals passed")
