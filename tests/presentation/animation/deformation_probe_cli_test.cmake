# Command-line refusals of animation_deformation_probe that need no private
# asset: usage errors (exit 2) and an unmountable installation (exit 1).
# No refusal may create the output file.
#
#   cmake -DEXE=<animation_deformation_probe> -DWORK=<scratch directory> -P deformation_probe_cli_test.cmake
cmake_minimum_required(VERSION 3.25)
foreach(required EXE WORK)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/out" "${WORK}/game" "${WORK}/mod")
set(game "${WORK}/game")
set(mod "${WORK}/mod")
set(output "${WORK}/out/receipt.json")
set(sha "0000000000000000000000000000000000000000000000000000000000000000")
set(failures 0)

function(expect_refusal label code pattern)
    execute_process(COMMAND "${EXE}" ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(problems "")
    if(NOT result STREQUAL "${code}")
        string(APPEND problems " exit=${result} (expected ${code})")
    endif()
    if(NOT err MATCHES "${pattern}")
        string(APPEND problems " stderr does not match '${pattern}': ${err}")
    endif()
    file(GLOB outputs "${WORK}/out/*" "${game}/*" "${mod}/*")
    if(outputs)
        string(APPEND problems " created ${outputs}")
    endif()
    if(problems)
        message(SEND_ERROR "${label}:${problems}")
        math(EXPR count "${failures} + 1")
        set(failures ${count} PARENT_SCOPE)
    else()
        message(STATUS "ok: ${label}")
    endif()
endfunction()

set(base --game-root "${game}" --mod-root "${mod}" --model data/art/models/x.alo --model-sha256 ${sha}
    --mesh Head --submesh 0 --out "${output}")
set(clip --animation data/art/models/x_a.ala --animation-sha256 ${sha} --time 0.3 --mode loop)

expect_refusal("no arguments" 2 "missing --game-root")
expect_refusal("unknown flag" 2 "unknown argument" ${base} ${clip} --discover)
expect_refusal("repeated flag" 2 "repeated" ${base} ${clip} --mesh Head)
expect_refusal("flag without value" 2 "needs a value" ${base} ${clip} --time)
expect_refusal("neither clip nor bind-only" 2 "or pass --bind-only" ${base})
expect_refusal("bind-only with a clip" 2 "excludes" ${base} ${clip} --bind-only)
expect_refusal("clip without hash" 2 "missing --animation-sha256" ${base}
    --animation data/art/models/x_a.ala --time 0.3 --mode loop)
expect_refusal("uppercase model hash" 2 "model-sha256" --game-root "${game}" --mod-root "${mod}"
    --model data/art/models/x.alo --model-sha256 ABCDEF0000000000000000000000000000000000000000000000000000000000
    --mesh Head --submesh 0 --out "${output}" --bind-only)
expect_refusal("short animation hash" 2 "animation-sha256" ${base}
    --animation data/art/models/x_a.ala --animation-sha256 abc --time 0.3 --mode loop)
expect_refusal("negative submesh" 2 "submesh" --game-root "${game}" --mod-root "${mod}" --model data/art/models/x.alo
    --model-sha256 ${sha} --mesh Head --submesh -1 --out "${output}" --bind-only)
expect_refusal("overflowing submesh" 2 "submesh" --game-root "${game}" --mod-root "${mod}" --model data/art/models/x.alo
    --model-sha256 ${sha} --mesh Head --submesh 99999999999999999999999 --out "${output}" --bind-only)
foreach(bad_time nan inf -0.5 1e400 0.3s)
    expect_refusal("time '${bad_time}'" 2 "--time" ${base}
        --animation data/art/models/x_a.ala --animation-sha256 ${sha} --time "${bad_time}" --mode loop)
endforeach()
expect_refusal("unknown mode" 2 "--mode" ${base}
    --animation data/art/models/x_a.ala --animation-sha256 ${sha} --time 0.3 --mode pingpong)
expect_refusal("output inside mod root" 2 "inside the game or mod root" --game-root "${game}" --mod-root "${mod}"
    --model data/art/models/x.alo --model-sha256 ${sha} --mesh Head --submesh 0 --out "${mod}/receipt.json" --bind-only)
expect_refusal("output inside game root" 2 "inside the game or mod root" --game-root "${game}" --mod-root "${mod}"
    --model data/art/models/x.alo --model-sha256 ${sha} --mesh Head --submesh 0 --out "${game}/Data/r.json" --bind-only)
expect_refusal("no installation" 1 "." ${base} ${clip})

# An existing output is never overwritten.
file(WRITE "${WORK}/existing.json" "keep")
execute_process(COMMAND "${EXE}" --game-root "${game}" --mod-root "${mod}" --model data/art/models/x.alo
    --model-sha256 ${sha} --mesh Head --submesh 0 --out "${WORK}/existing.json" --bind-only
    RESULT_VARIABLE result ERROR_VARIABLE err)
file(READ "${WORK}/existing.json" kept)
if(NOT result STREQUAL "2" OR NOT err MATCHES "never overwrites" OR NOT kept STREQUAL "keep")
    message(SEND_ERROR "existing output: exit=${result} kept='${kept}' ${err}")
    math(EXPR failures "${failures} + 1")
else()
    message(STATUS "ok: existing output kept")
endif()

if(failures GREATER 0)
    message(FATAL_ERROR "${failures} deformation probe CLI refusal(s) failed")
endif()
