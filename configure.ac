#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.67])
AC_INIT([codes-net], [0.4.0], [http://trac.mcs.anl.gov/projects/codes/newticket],[],[http://www.mcs.anl.gov/projects/codes])

AC_CANONICAL_TARGET
AC_CANONICAL_SYSTEM
AC_CANONICAL_HOST

AM_INIT_AUTOMAKE([foreign subdir-objects -Wall])

m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

AC_CONFIG_SRCDIR([doc/BUILD_STEPS])
AC_CONFIG_HEADERS([codes_net_config.h])

# Checks for programs.
AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_CXX
AC_PROG_CXXCPP
AC_PROG_RANLIB

PKG_PROG_PKG_CONFIG

# Check for C99 
AC_PROG_CC_C99

AC_REQUIRE_CPP

# Checks for header files.
AC_HEADER_STDC
AC_CHECK_HEADERS([stdlib.h string.h unistd.h execinfo.h pthread.h malloc.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_C_INLINE
AC_TYPE_INT8_T
AC_TYPE_INT16_T
AC_TYPE_INT32_T
AC_TYPE_INT64_T
AC_TYPE_UINT8_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T

# check for codes-base
PKG_CHECK_MODULES_STATIC([CODES_BASE], [codes-base], [],
                  [AC_MSG_ERROR([Could not find working codes-base installation via pkg-config])])

if test "x$srcdir" != "x."; then
    AC_CONFIG_LINKS([tests/conf:$srcdir/tests/conf])
fi
AC_CONFIG_FILES([Makefile])
AC_OUTPUT(maint/codes-net.pc)
