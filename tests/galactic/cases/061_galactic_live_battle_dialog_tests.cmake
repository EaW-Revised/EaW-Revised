eawr_add_test_case(
    NAME galactic_live_battle_dialog_tests
    SOURCES live_battle_dialog_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    LIBS eawr::campaign eawr::platform eawr::presentation
    TESTS galactic_live_battle_dialog
)

# GC #4030: installed live campaign with the native AI; the local seat's battle dialog opens only for its own conflicts.
