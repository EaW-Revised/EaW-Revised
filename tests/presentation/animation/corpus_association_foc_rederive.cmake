# Opt-in (#363 review 1): re-derives corpus_association_foc.tsv from the installed
# game with animation_corpus_association_foc and requires the committed file to
# be exactly what the tool writes, receipt included.
#
# Skips unless EAWR_EAW_GAME_ROOT names the read-only installation. The
# re-derived file stays under WORK.
#
#   cmake -DEXE=<animation_corpus_association_foc> -DFROZEN=<corpus_association_frozen.tsv>
#         -DCOMMITTED=<corpus_association_foc.tsv> -DWORK=<scratch directory> -P corpus_association_foc_rederive.cmake
cmake_minimum_required(VERSION 3.25)
foreach(required EXE FROZEN COMMITTED WORK)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()
if("$ENV{EAWR_EAW_GAME_ROOT}" STREQUAL "")
    message("SKIPPED: set EAWR_EAW_GAME_ROOT to re-derive the FoC association dispositions")
    return()
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
execute_process(COMMAND "${EXE}" "$ENV{EAWR_EAW_GAME_ROOT}" "${FROZEN}" "${WORK}/foc.tsv"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "animation_corpus_association_foc exited ${result}: ${output}${error}")
endif()
# Line endings aside (a Windows checkout may convert them), the files must be identical.
file(READ "${WORK}/foc.tsv" derived)
file(READ "${COMMITTED}" committed)
string(REPLACE "\r\n" "\n" derived "${derived}")
string(REPLACE "\r\n" "\n" committed "${committed}")
if(NOT derived STREQUAL committed)
    message(FATAL_ERROR "the committed ${COMMITTED} is not what the game gives; see ${WORK}/foc.tsv")
endif()
message(STATUS "re-derived FoC association dispositions match the committed file")
