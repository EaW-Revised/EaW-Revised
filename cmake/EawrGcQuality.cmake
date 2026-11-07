# Async galactic AI quality sweep (tools/gc_quality, tools/gc_quality/README.md).
#
# A pool job cannot hand arguments to a test, so each case of tools/gc_quality/sweep_matrix.json is its own registered
# CTest case: galactic_ai_quality_<profile>_<arm>_s<seed>_t<ticks>[_<probe>]. The cases run 5 minutes to hours each, so
# they are registered only when asked for: -DEAWR_GC_QUALITY_SWEEP=ON, or the marker file
# tools/gc_quality/sweep.enabled (the sweep runner adds it to the throwaway commit it submits). Ordinary and batch
# runs never see them.
function(eawr_register_gc_quality_cases matrix_file program)
    file(READ "${matrix_file}" matrix)
    string(JSON profile_count LENGTH "${matrix}" profiles)
    math(EXPR profile_last "${profile_count} - 1")
    foreach(profile_index RANGE ${profile_last})
        string(JSON profile MEMBER "${matrix}" profiles ${profile_index})
        string(JSON mods GET "${matrix}" profiles ${profile} mods)
        string(JSON campaign GET "${matrix}" profiles ${profile} campaign)
        if(mods STREQUAL "")
            set(mods_argument "-")
        else()
            set(mods_argument "${mods}")
        endif()
        string(JSON run_count LENGTH "${matrix}" profiles ${profile} runs)
        math(EXPR run_last "${run_count} - 1")
        foreach(run_index RANGE ${run_last})
            string(JSON ticks GET "${matrix}" profiles ${profile} runs ${run_index} ticks)
            string(JSON probe ERROR_VARIABLE probe_error GET "${matrix}" profiles ${profile} runs ${run_index} probe)
            if(NOT "${probe_error}" STREQUAL "NOTFOUND")
                set(probe "none")
            endif()
            string(JSON arm_count LENGTH "${matrix}" profiles ${profile} runs ${run_index} arms)
            string(JSON seed_count LENGTH "${matrix}" profiles ${profile} runs ${run_index} seeds)
            math(EXPR arm_last "${arm_count} - 1")
            math(EXPR seed_last "${seed_count} - 1")
            foreach(arm_index RANGE ${arm_last})
                string(JSON arm GET "${matrix}" profiles ${profile} runs ${run_index} arms ${arm_index})
                foreach(seed_index RANGE ${seed_last})
                    string(JSON seed GET "${matrix}" profiles ${profile} runs ${run_index} seeds ${seed_index})
                    set(name "galactic_ai_quality_${profile}_${arm}_s${seed}_t${ticks}")
                    if(NOT probe STREQUAL "none")
                        string(APPEND name "_${probe}")
                    endif()
                    # "-" as the directory: the decision log goes to the test log only, nothing is written on the host.
                    eawr_add_game_data_test(NAME "${name}" COMMAND "${program}" --quality-run - ${ticks} ${seed}
                        ${arm} ${probe} ${profile} ${mods_argument} ${campaign})
                    set_tests_properties("${name}" PROPERTIES LABELS "game-data;benchmark;batch-only;quality-sweep"
                        TIMEOUT 28800 RUN_SERIAL TRUE ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
                endforeach()
            endforeach()
        endforeach()
    endforeach()
endfunction()
