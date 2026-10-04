# Python source and synthetic evidence contracts.

# Each file runs its CPU contracts; graphical exercises retain their opt-in gates.
eawr_add_python_contract(python_presentation_p1_capture_compare tests.presentation.p1_capture.test_compare NEEDS PIL)
eawr_add_python_contract(python_presentation_p1_capture_migration_pair tests.presentation.p1_capture.test_migration_pair NEEDS PIL)

eawr_label_tests()
