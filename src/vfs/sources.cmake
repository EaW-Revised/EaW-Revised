# Shared translation units for the root libraries and the viewer.
# Paths are absolute so both projects can include this list directly.

set(EAWR_VFS_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/vfs.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/vfs_archive.cpp"
)
