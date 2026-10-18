eawr_add_test_case(
    NAME galactic_ai_interface_tests
    SOURCES ai_interface_tests.cpp
    LIBS eawr::script_authoritative eawr::sim_galactic
    TESTS galactic_ai_interface_contracts
)

# WGA-01..05/09/102..110: supporting lifecycle fixtures and real stock loading.
# The composed live-opponent primary proof remains a separate integration seam.
