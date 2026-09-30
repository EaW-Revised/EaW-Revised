if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.25)
    project(eawr_model_preview_tests LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    get_filename_component(EAWR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
    # The viewer's engine-independent selection and framing rules (no Godot).
    add_executable(model_preview_tests
        main.cpp
        "${EAWR_ROOT}/apps/viewer/src/model_preview.cpp"
    )
    target_include_directories(model_preview_tests PRIVATE
        "${EAWR_ROOT}/include" "${EAWR_ROOT}/apps/viewer/src")
    if(MSVC)
        target_compile_options(model_preview_tests PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(model_preview_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
else()
    set(EAWR_ROOT "${PROJECT_SOURCE_DIR}")
    add_executable(model_preview_tests
        main.cpp
        "${EAWR_ROOT}/apps/viewer/src/model_preview.cpp"
    )
    target_include_directories(model_preview_tests PRIVATE "${EAWR_ROOT}/apps/viewer/src")
    target_link_libraries(model_preview_tests PRIVATE eawr::presentation)
    target_compile_features(model_preview_tests PRIVATE cxx_std_20)
    eawr_target_project_warnings(model_preview_tests)
endif()

enable_testing()
add_test(NAME model_preview_contracts COMMAND model_preview_tests)

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
