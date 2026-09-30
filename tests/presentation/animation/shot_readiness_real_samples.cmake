# Opt-in real-sample loading for the three #24 units: infantry BI_Hunter with
# its attack clip, vehicle ATPT with its attack clip, and the capital
# Empire_Victory_Star_Destroyer (bind pose; it has no bindable clip). Inputs
# are the Remake mod-loose samples, pinned by the SHA-256 arguments below.
# Each run must bind strictly,
# census and palette the named submesh and resolve the named attachment bones
# at bind, clip start and 0.300 s (tick 9 at 30 Hz); the Hunter's tracked
# B_Head and B_Chest attachments must move with the animated hierarchy. ATPT
# Part04's motion is small (P1-03: <0.005 units), so it is recorded, not gated.
#
# Skips unless EAWR_EAW_GAME_ROOT and EAWR_REMAKE_MOD_ROOT name the read-only
# installation and Remake workshop item. Receipts stay under WORK.
#
#   cmake -DEXE=<animation_shot_readiness_probe> -DWORK=<scratch directory> -P shot_readiness_real_samples.cmake
cmake_minimum_required(VERSION 3.25)
foreach(required EXE WORK)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()
if("$ENV{EAWR_EAW_GAME_ROOT}" STREQUAL "" OR "$ENV{EAWR_REMAKE_MOD_ROOT}" STREQUAL "")
    message("SKIPPED: set EAWR_EAW_GAME_ROOT and EAWR_REMAKE_MOD_ROOT to load the real infantry, vehicle and capital samples")
    return()
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(roots --profile remake --game-root "$ENV{EAWR_EAW_GAME_ROOT}" --mod-root "$ENV{EAWR_REMAKE_MOD_ROOT}")
set(fixed --tick 9 --ticks-per-second 30 --seconds 0.3)
set(failures 0)

function(sample label)
    execute_process(COMMAND "${EXE}" ${roots} ${ARGN} --out "${WORK}/${label}.json"
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result STREQUAL "0" OR NOT EXISTS "${WORK}/${label}.json")
        message(SEND_ERROR "${label}: exit=${result} ${err}")
        math(EXPR count "${failures} + 1")
        set(failures ${count} PARENT_SCOPE)
    else()
        message(STATUS "ok: ${label}: ${out}")
    endif()
endfunction()

sample(infantry_bi_hunter
    --model data/art/models/bi_hunter.alo
    --model-sha256 b3c3164a8f323965c456755e220b87da2c493097aac5e0fda147e7e7bfbc4cd1
    --animation data/art/models/bi_hunter_attack_00.ala
    --animation-sha256 489dc6fad0953cb3c2405c885f524a3001f9e3bf151f7798faa289815de91279
    ${fixed} --mesh Head --submesh 0 --expect-moving B_Head --expect-moving B_Chest)
sample(vehicle_atpt
    --model data/art/models/atpt.alo
    --model-sha256 1312a51108fdf3a4a04de590ee2f0aa42d08fb7038cb661d0bf758b5889d4294
    --animation data/art/models/atpt_attack_00.ala
    --animation-sha256 592df3f7d7139a36a137f3a24d42ffe89274c6678a2544fa3eca9168234a499f
    ${fixed} --mesh Part04 --submesh 0 --attach Part04)
sample(capital_victory
    --model data/art/models/empire_victory_star_destroyer.alo
    --model-sha256 cbc2f9d4810fae35b68596958d778138589f247969ce37a81b405006f085252a
    --mesh Superstructure --submesh 0 --attach Superstructure)

if(failures GREATER 0)
    message(FATAL_ERROR "${failures} real-sample load(s) failed")
endif()
