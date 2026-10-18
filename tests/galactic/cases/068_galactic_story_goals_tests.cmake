eawr_add_test_case(
    NAME galactic_story_goals_tests
    SOURCES story_goals_tests.cpp
    LIBS eawr::sim_galactic
    TESTS galactic_story_goals_contracts
)

# WGA-57..60 supporting component contracts; installed stock proof stays open.
