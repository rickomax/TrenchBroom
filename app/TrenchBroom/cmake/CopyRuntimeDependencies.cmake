# Resolves the full transitive runtime-DLL closure of an executable and copies the
# resolved DLLs next to it, so the build tree is runnable in place without installing.
#
# This inspects the actual PE import tables (recursively), so it also catches DLLs that
# are imported deep inside a dependency rather than linked directly by the executable --
# e.g. libraw (rawd.dll), which freeimage.dll imports but which is not a CMake target
# TrenchBroom links against, so $<TARGET_RUNTIME_DLLS> cannot see it. This mirrors what
# the install / CPack step does via file(GET_RUNTIME_DEPENDENCIES).
#
# Expected cache variables (passed with -D on the command line):
#   TB_EXECUTABLE  - the executable to resolve dependencies for
#   TB_DEST_DIR    - the directory to copy resolved DLLs into
#   TB_SEARCH_DIRS - directories to search for dependency DLLs (e.g. the vcpkg bin dir
#                    for the current configuration); a |-separated list, because a
#                    ;-separated list would be split into separate arguments by the
#                    Visual Studio generator's command scripts

string(REPLACE "|" ";" TB_SEARCH_DIRS "${TB_SEARCH_DIRS}")

# The regexes are a workaround for https://gitlab.kitware.com/cmake/cmake/-/issues/22431
file(
  GET_RUNTIME_DEPENDENCIES
  RESOLVED_DEPENDENCIES_VAR TB_RESOLVED_DLLS
  UNRESOLVED_DEPENDENCIES_VAR TB_UNRESOLVED_DLLS
  EXECUTABLES "${TB_EXECUTABLE}"
  DIRECTORIES ${TB_SEARCH_DIRS}
  PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*" "hvsifiletrust.dll"
  POST_EXCLUDE_REGEXES ".*[Ss][Yy][Ss][Tt][Ee][Mm]32.*")

foreach(TB_DLL ${TB_RESOLVED_DLLS})
  file(COPY "${TB_DLL}" DESTINATION "${TB_DEST_DIR}")
endforeach()

# Unresolved dependencies are expected: they are system libraries (and Qt DLLs, which
# windeployqt deploys separately). Report them at a low level for diagnostics only.
if(TB_UNRESOLVED_DLLS)
  message(
    STATUS
    "TrenchBroom: assuming these runtime dependencies are provided by the system or "
    "deployed separately: ${TB_UNRESOLVED_DLLS}")
endif()
