# Python source and synthetic evidence contracts.

# Each file runs its CPU contracts; graphical exercises retain their opt-in gates.
eawr_add_python_contract(python_presentation_godot_contract tests.presentation.godot.test_contract)
eawr_add_python_contract(python_presentation_godot_export_package tests.presentation.godot.test_export_package)
eawr_add_python_contract(python_presentation_godot_qualify_package_runtime tests.presentation.godot.test_qualify_package_runtime)

eawr_label_tests()
