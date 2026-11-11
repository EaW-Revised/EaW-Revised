# The one release version (config/release-version.json). Include this before project(): both the root project and
# the viewer project take their VERSION from it, so PROJECT_VERSION, the mods service version and the default build
# identity cannot disagree with the version a release or kit is cut as.
get_filename_component(EAWR_RELEASE_VERSION_FILE "${CMAKE_CURRENT_LIST_DIR}/../config/release-version.json" ABSOLUTE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${EAWR_RELEASE_VERSION_FILE}")
# A UTF-8 byte order mark (PowerShell writes one) is dropped, as release_version.py does.
file(READ "${EAWR_RELEASE_VERSION_FILE}" EAWR_RELEASE_VERSION_BOM LIMIT 3 HEX)
if(EAWR_RELEASE_VERSION_BOM STREQUAL "efbbbf")
    file(READ "${EAWR_RELEASE_VERSION_FILE}" EAWR_RELEASE_VERSION_JSON OFFSET 3)
else()
    file(READ "${EAWR_RELEASE_VERSION_FILE}" EAWR_RELEASE_VERSION_JSON)
endif()
string(JSON EAWR_RELEASE_VERSION ERROR_VARIABLE EAWR_RELEASE_VERSION_ERROR GET "${EAWR_RELEASE_VERSION_JSON}" version)
if(EAWR_RELEASE_VERSION_ERROR OR NOT EAWR_RELEASE_VERSION MATCHES "^[0-9]+\.[0-9]+\.[0-9]+$")
    message(FATAL_ERROR "config/release-version.json must hold {\"version\": \"<major>.<minor>.<patch>\"}")
endif()
