# - Try to find UDREG
# Once done this will define
#  UDREG_FOUND - System has UDREG
#  UDREG_INCLUDE_DIRS - The UDREG include directories
#  UDREG_LIBRARIES - The libraries needed to use UDREG

find_package(PkgConfig)
pkg_check_modules(PC_UDREG QUIET cray-udreg)

find_path(UDREG_INCLUDE_DIR udreg_pub.h
  HINTS ${PC_UDREG_INCLUDEDIR} ${PC_UDREG_INCLUDE_DIRS})

find_library(UDREG_LIBRARY NAMES udreg libudreg
  HINTS ${PC_UDREG_LIBDIR} ${PC_UDREG_LIBRARY_DIRS})

set(UDREG_LIBRARIES ${UDREG_LIBRARY})
set(UDREG_INCLUDE_DIRS ${UDREG_INCLUDE_DIR})
set(UDREG_LIBDIR ${PC_UDREG_LIBDIR})
include(FindPackageHandleStandardArgs)
# handle the QUIETLY and REQUIRED arguments and set UDREG_FOUND to TRUE
# if all listed variables are TRUE
find_package_handle_standard_args(UDREG DEFAULT_MSG
                                  UDREG_LIBRARY UDREG_INCLUDE_DIR)

mark_as_advanced(UDREG_INCLUDE_DIR UDREG_LIBRARY)
