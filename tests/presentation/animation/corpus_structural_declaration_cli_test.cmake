# Command-line refusals of animation_corpus_structural_declaration that happen
# before any asset is read, so they need no private asset:
#   - usage errors and output paths that alias a pinned input (exit 2);
#   - a missing, stale or malformed frozen manifest or R2 receipt (exit 4).
# The retained R2 receipt is private, so the authenticated-inputs path is
# covered by the synthetic CTest and the authentic runs, not here.
# Every refusal must leave the pinned bytes intact and create no output.
#
#   cmake -DEXE=<animation_corpus_structural_declaration> -DMANIFEST=<tracked manifest>
#         -DWORK=<scratch directory> -P corpus_structural_declaration_cli_test.cmake
cmake_minimum_required(VERSION 3.25)
foreach(required EXE MANIFEST WORK)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/out")
set(metadata "${WORK}/frozen-metadata.tsv")
set(receipt "${WORK}/structural-discovery.json")
# A copy, so that even a failed refusal could never touch the tracked file.
file(COPY_FILE "${MANIFEST}" "${metadata}")
# Not the retained R2 receipt: its SHA-256 is not the pinned one.
file(WRITE "${receipt}" "{\n  \"schema\": \"eawr.animation-shocktrooper-structural-discovery\",\n  \"schema_version\": 1\n}\n")
file(SHA256 "${metadata}" metadata_sha)
file(SHA256 "${receipt}" receipt_sha)
set(game "${WORK}/no-game")
set(mod "${WORK}/no-mod")
set(output "${WORK}/out/structural-declaration.json")
set(failures 0)

function(expect_refusal label code pattern)
    execute_process(COMMAND "${EXE}" ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    file(SHA256 "${metadata}" metadata_now)
    file(SHA256 "${receipt}" receipt_now)
    file(GLOB outputs "${WORK}/out/*")
    set(problems "")
    if(NOT result STREQUAL "${code}")
        string(APPEND problems " exit=${result} (expected ${code})")
    endif()
    if(NOT err MATCHES "${pattern}")
        string(APPEND problems " stderr='${err}' lacks '${pattern}'")
    endif()
    if(NOT metadata_now STREQUAL metadata_sha OR NOT receipt_now STREQUAL receipt_sha)
        string(APPEND problems " pinned bytes changed")
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

expect_refusal("too few arguments" 2 "usage" "${game}" "${mod}" "${metadata}" "${receipt}")
expect_refusal("too many arguments" 2 "usage" "${game}" "${mod}" "${metadata}" "${receipt}" "${output}" "${output}")
set(alias "name the same file")
expect_refusal("output = frozen metadata" 2 "${alias}" "${game}" "${mod}" "${metadata}" "${receipt}" "${metadata}")
expect_refusal("output = structural receipt" 2 "${alias}" "${game}" "${mod}" "${metadata}" "${receipt}" "${receipt}")
expect_refusal("receipt = frozen metadata" 2 "${alias}" "${game}" "${mod}" "${metadata}" "${metadata}" "${output}")

file(READ "${metadata}" manifest_text)
file(WRITE "${WORK}/stale-metadata.tsv" "${manifest_text}#")
expect_refusal("stale frozen metadata" 4 "frozen metadata is stale"
    "${game}" "${mod}" "${WORK}/stale-metadata.tsv" "${receipt}" "${output}")
expect_refusal("missing frozen metadata" 4 "frozen metadata is missing"
    "${game}" "${mod}" "${WORK}/absent.tsv" "${receipt}" "${output}")
# The frozen manifest authenticates and yields the lead; the receipt does not.
expect_refusal("stale structural receipt" 4 "structural receipt is stale"
    "${game}" "${mod}" "${metadata}" "${receipt}" "${output}")
expect_refusal("missing structural receipt" 4 "structural receipt is missing"
    "${game}" "${mod}" "${metadata}" "${WORK}/absent.json" "${output}")

if(failures GREATER 0)
    message(FATAL_ERROR "${failures} command-line refusal case(s) failed")
endif()
message(STATUS "animation structural-declaration command-line refusals passed")
