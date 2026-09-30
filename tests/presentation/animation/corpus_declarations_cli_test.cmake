# Command-line refusals of animation_corpus_declarations that happen before any
# asset is read, so they need no private asset:
#   - usage errors and output paths that alias a pinned input (exit 2);
#   - a missing, stale or malformed pinned input (exit 4);
#   - authenticated inputs but no mountable game (exit 1).
# Every refusal must leave the pinned bytes intact and create no output.
#
#   cmake -DEXE=<animation_corpus_declarations> -DMANIFEST=<tracked manifest>
#         -DPAIRS=<tracked pair list> -DWORK=<scratch directory> -P corpus_declarations_cli_test.cmake
cmake_minimum_required(VERSION 3.25)
foreach(required EXE MANIFEST PAIRS WORK)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/out")
set(metadata "${WORK}/frozen-metadata.tsv")
set(pairs "${WORK}/pairs.tsv")
# Copies, so that even a failed refusal could never touch the tracked files.
file(COPY_FILE "${MANIFEST}" "${metadata}")
file(COPY_FILE "${PAIRS}" "${pairs}")
file(SHA256 "${metadata}" metadata_sha)
file(SHA256 "${pairs}" pairs_sha)
set(game "${WORK}/no-game")
set(mod "${WORK}/no-mod")
set(output "${WORK}/out/declarations.json")
set(failures 0)

function(expect_refusal label code pattern)
    execute_process(COMMAND "${EXE}" ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    file(SHA256 "${metadata}" metadata_now)
    file(SHA256 "${pairs}" pairs_now)
    file(GLOB outputs "${WORK}/out/*")
    set(problems "")
    if(NOT result STREQUAL "${code}")
        string(APPEND problems " exit=${result} (expected ${code})")
    endif()
    if(NOT err MATCHES "${pattern}")
        string(APPEND problems " stderr='${err}' lacks '${pattern}'")
    endif()
    if(NOT metadata_now STREQUAL metadata_sha OR NOT pairs_now STREQUAL pairs_sha)
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

expect_refusal("too few arguments" 2 "usage" "${game}" "${mod}" "${metadata}" "${pairs}")
set(alias "name the same file")
expect_refusal("output = pinned pairs" 2 "${alias}" "${game}" "${mod}" "${metadata}" "${pairs}" "${pairs}")
expect_refusal("output = frozen metadata" 2 "${alias}" "${game}" "${mod}" "${metadata}" "${pairs}" "${metadata}")
expect_refusal("pairs = frozen metadata" 2 "${alias}" "${game}" "${mod}" "${metadata}" "${metadata}" "${output}")

file(READ "${pairs}" pairs_text)
file(WRITE "${WORK}/stale-pairs.tsv" "${pairs_text}#")
expect_refusal("stale pinned pairs" 4 "pinned pairs are stale"
    "${game}" "${mod}" "${metadata}" "${WORK}/stale-pairs.tsv" "${output}")
expect_refusal("missing pinned pairs" 4 "pinned pairs are missing"
    "${game}" "${mod}" "${metadata}" "${WORK}/absent.tsv" "${output}")
file(READ "${metadata}" manifest_text)
file(WRITE "${WORK}/stale-metadata.tsv" "${manifest_text}#")
expect_refusal("stale frozen metadata" 4 "frozen metadata is stale"
    "${game}" "${mod}" "${WORK}/stale-metadata.tsv" "${pairs}" "${output}")
# Both inputs authenticate; the absent installation is what fails.
expect_refusal("no installation to mount" 1 "."
    "${game}" "${mod}" "${metadata}" "${pairs}" "${output}")

if(failures GREATER 0)
    message(FATAL_ERROR "${failures} command-line refusal case(s) failed")
endif()
message(STATUS "animation declaration probe command-line refusals passed")
