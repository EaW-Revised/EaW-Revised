# Visual Studio generators compile the files of a project one at a time unless cl runs with /MP.
# The offload build hosts (tools/offload/windows_remote_build.ps1, #570) set EAWR_MSVC_MP to the compiler
# processes each project may use; 0 (the default) leaves it off, and other generators ignore it. It is
# appended to the flags here rather than passed as CMAKE_CXX_FLAGS so a cache that already exists
# picks it up and the default flags stay.
set(EAWR_MSVC_MP 0 CACHE STRING "Visual Studio generators: /MP<n> compiler processes per project, 0 for none")
if(EAWR_MSVC_MP GREATER 0 AND CMAKE_GENERATOR MATCHES "^Visual Studio")
    string(APPEND CMAKE_CXX_FLAGS " /MP${EAWR_MSVC_MP}")
    string(APPEND CMAKE_C_FLAGS " /MP${EAWR_MSVC_MP}")
endif()
