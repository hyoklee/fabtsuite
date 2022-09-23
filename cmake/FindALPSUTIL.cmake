# - Try to find ALPSUTIL
# Once done this will define
#  ALPSUTIL_FOUND - System has ALPSUTIL
#  ALPSUTIL_INCLUDE_DIRS - The ALPSUTIL include directories
#  ALPSUTIL_LIBRARIES - The libraries needed to use ALPSUTIL

find_package(PkgConfig)
pkg_check_modules(PC_ALPSUTIL QUIET cray-alpsutil)

find_path(ALPSUTIL_INCLUDE_DIR alps/libalpsutil.h
  HINTS ${PC_ALPSUTIL_INCLUDEDIR} ${PC_ALPSUTIL_INCLUDE_DIRS})

find_library(ALPSUTIL_LIBRARY NAMES alps libalps
  HINTS ${PC_ALPSUTIL_LIBDIR} ${PC_ALPSUTIL_LIBRARY_DIRS})

set(ALPSUTIL_LIBRARIES ${ALPSUTIL_LIBRARY})
set(ALPSUTIL_INCLUDE_DIRS ${ALPSUTIL_INCLUDE_DIR})
set(ALPSUTIL_LIBDIR ${PC_ALPSUTIL_LIBDIR})
include(FindPackageHandleStandardArgs)
# handle the QUIETLY and REQUIRED arguments and set ALPSUTIL_FOUND to TRUE
# if all listed variables are TRUE
find_package_handle_standard_args(ALPSUTIL DEFAULT_MSG
                                  ALPSUTIL_LIBRARY ALPSUTIL_INCLUDE_DIR)

mark_as_advanced(ALPSUTIL_INCLUDE_DIR ALPSUTIL_LIBRARY)
