# Python source and synthetic evidence contracts.

# Each file runs its CPU contracts; graphical exercises retain their opt-in gates.
eawr_add_python_contract(python_validation_p1_capture_build_manifest tests.validation.p1_capture.test_build_manifest ENTRYPOINT NEEDS PIL)
eawr_add_python_contract(python_validation_p1_capture_windows_prototype_capture tests.validation.p1_capture.test_windows_prototype_capture ENTRYPOINT NEEDS PIL)

eawr_label_tests()
