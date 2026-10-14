# The EAWRCKP checkpoint library (ADR-016, docs/checkpoint-format.md), shared by the
# root and live viewer owners. Paths are absolute so another project can include this list.

set(EAWR_CHECKPOINT_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/checkpoint.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/restore_continue.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tactical.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/game.cpp"
)
