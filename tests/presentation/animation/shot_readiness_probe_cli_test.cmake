# Command-line refusals of animation_shot_readiness_probe that need no private
# asset: usage errors (exit 2) and an unmountable installation (exit 1).
# No refusal may create the output file.
#
# With PYTHON and FIXTURE it also runs the success path on a synthetic
# installation written by shot_readiness_fixture.py.
#
#   cmake -DEXE=<animation_shot_readiness_probe> -DWORK=<scratch directory>
#         [-DPYTHON=<python> -DFIXTURE=<shot_readiness_fixture.py>] -P shot_readiness_probe_cli_test.cmake
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

set(base --profile remake --game-root "${game}" --mod-root "${mod}" --model data/art/models/x.alo
    --model-sha256 ${sha} --out "${output}")
set(clip --animation data/art/models/x_a.ala --animation-sha256 ${sha} --tick 9 --ticks-per-second 30 --seconds 0.3)

expect_refusal("no arguments" 2 "missing --profile")
expect_refusal("unknown flag" 2 "unknown argument" ${base} ${clip} --discover)
expect_refusal("repeated flag" 2 "repeated" ${base} ${clip} --tick 3)
expect_refusal("flag without value" 2 "needs a value" ${base} ${clip} --seconds)
expect_refusal("attach without value" 2 "--attach needs a value" ${base} --attach)
expect_refusal("unknown profile" 2 "--profile" --profile eaw --game-root "${game}" --model data/art/models/x.alo
    --model-sha256 ${sha} --out "${output}")
expect_refusal("remake without mod root" 2 "needs --mod-root" --profile remake --game-root "${game}"
    --model data/art/models/x.alo --model-sha256 ${sha} --out "${output}")
expect_refusal("foc with mod root" 2 "takes no --mod-root" --profile foc --game-root "${game}" --mod-root "${mod}"
    --model data/art/models/x.alo --model-sha256 ${sha} --out "${output}")
expect_refusal("no game root" 2 "missing --game-root" --profile foc --model data/art/models/x.alo
    --model-sha256 ${sha} --out "${output}")
expect_refusal("uppercase model hash" 2 "model-sha256" --profile foc --game-root "${game}" --model data/art/models/x.alo
    --model-sha256 ABCDEF0000000000000000000000000000000000000000000000000000000000 --out "${output}")
expect_refusal("partial clip flags" 2 "go together" ${base} --animation data/art/models/x_a.ala --animation-sha256 ${sha})
expect_refusal("short animation hash" 2 "animation-sha256" ${base} --animation data/art/models/x_a.ala
    --animation-sha256 abc --tick 9 --ticks-per-second 30 --seconds 0.3)
foreach(bad_tick -1 1.5 99999999999999999999999 x)
    expect_refusal("tick '${bad_tick}'" 2 "--tick" ${base} --animation data/art/models/x_a.ala --animation-sha256 ${sha}
        --tick "${bad_tick}" --ticks-per-second 30 --seconds 0.3)
endforeach()
expect_refusal("zero tick rate" 2 "--ticks-per-second" ${base} --animation data/art/models/x_a.ala
    --animation-sha256 ${sha} --tick 9 --ticks-per-second 0 --seconds 0.3)
foreach(bad_seconds nan inf -0.5 1e400 0.3s)
    expect_refusal("seconds '${bad_seconds}'" 2 "--seconds" ${base} --animation data/art/models/x_a.ala
        --animation-sha256 ${sha} --tick 9 --ticks-per-second 30 --seconds "${bad_seconds}")
endforeach()
expect_refusal("expect-moving without a clip" 2 "needs a clip" ${base} --expect-moving Head)
expect_refusal("mesh without submesh" 2 "go together" ${base} --mesh Head)
expect_refusal("negative submesh" 2 "submesh" ${base} --mesh Head --submesh -1)
expect_refusal("output inside mod root" 2 "inside an input root" --profile remake --game-root "${game}"
    --mod-root "${mod}" --model data/art/models/x.alo --model-sha256 ${sha} --out "${mod}/receipt.json")
expect_refusal("output inside game root" 2 "inside an input root" --profile foc --game-root "${game}"
    --model data/art/models/x.alo --model-sha256 ${sha} --out "${game}/Data/r.json")
expect_refusal("no installation" 1 "." ${base} ${clip})

# An existing output is never overwritten.
file(WRITE "${WORK}/existing.json" "keep")
execute_process(COMMAND "${EXE}" --profile foc --game-root "${game}" --model data/art/models/x.alo
    --model-sha256 ${sha} --out "${WORK}/existing.json" RESULT_VARIABLE result ERROR_VARIABLE err)
file(READ "${WORK}/existing.json" kept)
if(NOT result STREQUAL "2" OR NOT err MATCHES "never overwrites" OR NOT kept STREQUAL "keep")
    message(SEND_ERROR "existing output: exit=${result} kept='${kept}' ${err}")
    math(EXPR failures "${failures} + 1")
else()
    message(STATUS "ok: existing output kept")
endif()


# Success path on a synthetic installation (bind-only, FoC profile): a receipt
# is written, it is byte-identical on a second run, and data refusals exit 4.
if(DEFINED PYTHON)
    set(install "${WORK}/install")
    execute_process(COMMAND "${PYTHON}" "${FIXTURE}" "${install}" RESULT_VARIABLE result OUTPUT_VARIABLE box_sha
        ERROR_VARIABLE err OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "fixture: ${err}")
    endif()
    set(box --profile foc --game-root "${install}" --model data/art/models/eawr_probe_box.alo --model-sha256 ${box_sha}
        --mesh Box0 --submesh 0 --attach Root)
    foreach(run 1 2)
        execute_process(COMMAND "${EXE}" ${box} --out "${WORK}/box-${run}.json" RESULT_VARIABLE result ERROR_VARIABLE err)
        if(NOT result STREQUAL "0")
            message(SEND_ERROR "synthetic success run ${run}: exit=${result} ${err}")
            math(EXPR failures "${failures} + 1")
        endif()
    endforeach()
    if(EXISTS "${WORK}/box-1.json" AND EXISTS "${WORK}/box-2.json")
        file(READ "${WORK}/box-1.json" first)
        file(READ "${WORK}/box-2.json" second)
        set(problems "")
        if(NOT first STREQUAL second)
            string(APPEND problems " receipts differ;")
        endif()
        foreach(fragment "\"schema\":\"eawr.animation-shot-readiness.v1\"" "\"layer\":\"base\"" "\"origin\":\"loose\""
                "\"route\":\"rigid\"" "\"name\":\"Root\"" "\"active_influences\":24" "\"draw_bound_static\":1"
                "\"animation\":null" "\"bone\":\"Root\"" "\"fixed_translation_delta\":0")
            string(FIND "${first}" "${fragment}" at)
            if(at EQUAL -1)
                string(APPEND problems " missing ${fragment};")
            endif()
        endforeach()
        if(problems)
            message(SEND_ERROR "synthetic receipt:${problems}")
            math(EXPR failures "${failures} + 1")
        else()
            message(STATUS "ok: synthetic receipt written, repeatable and complete")
        endif()
    endif()
    set(refusals_before ${failures})
    expect_refusal("pinned hash mismatch" 4 "does not match pinned" --profile foc --game-root "${install}"
        --model data/art/models/eawr_probe_box.alo --model-sha256 ${sha} --out "${output}")
    expect_refusal("absent mesh" 4 "mesh is absent" --profile foc --game-root "${install}"
        --model data/art/models/eawr_probe_box.alo --model-sha256 ${box_sha} --mesh box0 --submesh 0 --out "${output}")
    expect_refusal("absent attachment" 4 "attachment bone is absent" --profile foc --game-root "${install}"
        --model data/art/models/eawr_probe_box.alo --model-sha256 ${box_sha} --attach MuzzleA_00 --out "${output}")
    expect_refusal("absent model" 1 "." --profile foc --game-root "${install}"
        --model data/art/models/missing.alo --model-sha256 ${box_sha} --out "${output}")
endif()

if(failures GREATER 0)
    message(FATAL_ERROR "${failures} shot readiness probe CLI check(s) failed")
endif()
