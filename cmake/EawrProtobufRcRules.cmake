# The pinned Protobuf copies C++ flags into CMAKE_RC_FLAGS under clang-cl.
# rc.exe cannot consume /bigobj, /utf-8 or C++ warning flags. Limit this
# override to Protobuf's RC rule; definitions/includes and C/C++ flags survive.
set(CMAKE_RC_COMPILE_OBJECT "<CMAKE_RC_COMPILER> <DEFINES> -DWIN32 <INCLUDES> /fo <OBJECT> <SOURCE>")
