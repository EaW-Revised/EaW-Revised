# Command-line refusals of animation_corpus_validation that happen before any
# asset is mounted, so they need no private asset:
#   - output paths that alias a frozen input or another output (exit 2);
#   - a missing, stale or unpinned frozen input (exit 4).
# Every refusal must leave the frozen bytes intact and create no output.
#
#   cmake -DEXE=<animation_corpus_validation> -DMANIFEST=<tracked manifest>
#         -DWORK=<scratch directory> -P corpus_frozen_cli_test.cmake
cmake_minimum_required(VERSION 3.25)
foreach(required EXE MANIFEST WORK)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/sub" "${WORK}/out")
set(ledger "${WORK}/frozen-ledger.json")
set(metadata "${WORK}/frozen-metadata.tsv")
file(WRITE "${ledger}" "synthetic frozen ledger, not the pinned v1 ledger\n")
# A copy, so that even a failed refusal could never touch the tracked file.
file(COPY_FILE "${MANIFEST}" "${metadata}")
file(SHA256 "${ledger}" ledger_sha)
file(SHA256 "${metadata}" metadata_sha)
set(game "${WORK}/no-game")
set(mod "${WORK}/no-mod")
set(report "${WORK}/out/report.json")
set(diagnostics "${WORK}/out/diagnostics.json")
set(audit "${WORK}/out/audit.json")
set(failures 0)

function(expect_refusal label code pattern)
    execute_process(COMMAND "${EXE}" ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    file(SHA256 "${ledger}" ledger_now)
    file(SHA256 "${metadata}" metadata_now)
    file(GLOB outputs "${WORK}/out/*")
    set(problems "")
    if(NOT result STREQUAL "${code}")
        string(APPEND problems " exit=${result} (expected ${code})")
    endif()
    if(NOT err MATCHES "${pattern}")
        string(APPEND problems " stderr='${err}' lacks '${pattern}'")
    endif()
    if(NOT ledger_now STREQUAL ledger_sha OR NOT metadata_now STREQUAL metadata_sha)
        string(APPEND problems " frozen bytes changed")
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

set(alias "name the same file")
expect_refusal("audit = frozen ledger" 2 "${alias}"
    "${game}" "${mod}" "${report}" "${diagnostics}" --association-audit "${ledger}" "${metadata}" "${ledger}")
expect_refusal("report = frozen ledger" 2 "${alias}"
    "${game}" "${mod}" "${ledger}" "${diagnostics}" --association-audit "${ledger}" "${metadata}" "${audit}")
expect_refusal("diagnostics = frozen metadata via .." 2 "${alias}"
    "${game}" "${mod}" "${report}" "${WORK}/sub/../frozen-metadata.tsv" --association-audit "${ledger}" "${metadata}" "${audit}")
expect_refusal("audit = report" 2 "${alias}"
    "${game}" "${mod}" "${report}" "${diagnostics}" --association-audit "${ledger}" "${metadata}" "${WORK}/out/./report.json")
expect_refusal("audit = diagnostics" 2 "${alias}"
    "${game}" "${mod}" "${report}" "${diagnostics}" --association-audit "${ledger}" "${metadata}" "${diagnostics}")
expect_refusal("frozen ledger = frozen metadata" 2 "${alias}"
    "${game}" "${mod}" "${report}" "${diagnostics}" --association-audit "${metadata}" "${metadata}" "${audit}")
expect_refusal("report = diagnostics without audit" 2 "${alias}"
    "${game}" "${mod}" "${report}" "${report}")
if(WIN32)
    string(TOUPPER "${ledger}" upper_ledger)
    expect_refusal("audit = frozen ledger, case variant" 2 "${alias}"
        "${game}" "${mod}" "${report}" "${diagnostics}" --association-audit "${ledger}" "${metadata}" "${upper_ledger}")
    string(REPLACE "/" "\\" backslash_metadata "${metadata}")
    expect_refusal("report = frozen metadata, backslashes" 2 "${alias}"
        "${game}" "${mod}" "${backslash_metadata}" "${diagnostics}" --association-audit "${ledger}" "${metadata}" "${audit}")
    expect_refusal("audit = frozen ledger, trailing dot" 2 "${alias}"
        "${game}" "${mod}" "${report}" "${diagnostics}" --association-audit "${ledger}" "${metadata}" "${ledger}.")
endif()
file(CREATE_LINK "${ledger}" "${WORK}/hard-link.json" RESULT link_result)
if(link_result STREQUAL "0")
    expect_refusal("audit = hard link to frozen ledger" 2 "${alias}"
        "${game}" "${mod}" "${report}" "${diagnostics}" --association-audit "${ledger}" "${metadata}" "${WORK}/hard-link.json")
else()
    message(STATUS "hard link unavailable: ${link_result}")
endif()

# Frozen-input authentication happens before the run.
file(READ "${metadata}" manifest_text)
file(WRITE "${WORK}/stale-metadata.tsv" "${manifest_text}#")
expect_refusal("stale frozen metadata" 4 "frozen metadata is stale"
    "${game}" "${mod}" "${report}" "${diagnostics}" --association-audit "${ledger}" "${WORK}/stale-metadata.tsv" "${audit}")
expect_refusal("missing frozen metadata" 4 "frozen metadata is missing"
    "${game}" "${mod}" "${report}" "${diagnostics}" --association-audit "${ledger}" "${WORK}/absent.tsv" "${audit}")
# The tracked manifest authenticates, so the synthetic ledger is what refuses.
expect_refusal("stale frozen ledger" 4 "frozen ledger is stale"
    "${game}" "${mod}" "${report}" "${diagnostics}" --association-audit "${ledger}" "${metadata}" "${audit}")
expect_refusal("missing frozen ledger" 4 "frozen ledger is missing"
    "${game}" "${mod}" "${report}" "${diagnostics}" --association-audit "${WORK}/absent.json" "${metadata}" "${audit}")
expect_refusal("audit flag with too few paths" 2 "usage"
    "${game}" "${mod}" "${report}" --association-audit "${ledger}" "${audit}")
expect_refusal("audit flag twice" 2 "usage"
    "${game}" "${mod}" "${report}" --association-audit "${ledger}" "${metadata}" "${audit}"
    --association-audit "${ledger}" "${metadata}" "${WORK}/out/audit-2.json")

if(failures GREATER 0)
    message(FATAL_ERROR "${failures} command-line refusal case(s) failed")
endif()
message(STATUS "animation corpus frozen-input command-line refusals passed")
