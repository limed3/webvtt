#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.67])
AC_INIT([webvtt], [0.0.3])
AC_CANONICAL_HOST

linux="no"
osx="no"
win32="no"

AS_CASE([$host],
  [*-*-linux*],[linux=true gtk=true],
  [*-*-darwin*],[osx=true qt=true],
  [*-mingw*|*-winnt*],[win32=true])

AM_CONDITIONAL(LINUX,test x$linux = xtrue)
AM_CONDITIONAL(OSX,test x$osx = xtrue)
AM_CONDITIONAL(WIN32,test x$win32 = xtrue)

AC_CONFIG_MACRO_DIR([build/autoconf])
AM_INIT_AUTOMAKE([1.11 -Wall -Wno-extra-portability foreign subdir-objects])
AC_CONFIG_SRCDIR([src/libwebvtt/alloc.c])
AC_CONFIG_HEADERS([config.h])

LT_INIT([win32-dll])

# Checks for programs.
AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_CXX
AC_PROG_SED
AC_PROG_INSTALL
AC_PROG_MKDIR_P

# Check for valgrind, which we run with tests if possible
AC_PATH_PROG(VALGRIND, valgrind, no)
AM_CONDITIONAL(HAVE_VALGRIND, test "x$VALGRIND" != "xno")

# Enable debugging (currently only gdb)
AC_ARG_ENABLE([debug],
    [  --enable-debug           build with additional debugging code],
    [CFLAGS='-g'; AC_DEFINE([DEBUG],[1],[Defined if debugging is enabled])])
# Enable profiling (currently only gdb)
AC_ARG_ENABLE([gprof],
	[ --enable-gprof            build with gprof profiling engine],
	[CFLAGS='-pg'; AC_DEFINE([GPROF],[1],[Defined if using gprof profiling engine])])

# Checks for libraries.
# * Check for pthreads library.
ACX_PTHREAD([have_pthread=yes], [heve_pthread=no])

# The google test framework uses 'nanosleep' if using pthreads, and on mingw
# nanosleep does not seem to be provided, even though pthreads is. So, if we
# can't find nanosleep, we don't want to define GTEST_HAS_PTHREADS=1
AC_CHECK_FUNC([nanosleep],[HAVE_NANOSLEEP="yes"],[HAVE_NANOSLEEP="no"])

# Define variables for unit test using gtest
GTEST_VERSION="1.6.0"
if [test "x$have_pthread" = "xyes"] && [test "$HAVE_NANOSLEEP" = "yes"]; then
# gtest can safely use the PTHREAD code
  GTEST_CPPFLAGS="-DGTEST_HAS_PTHREAD=1 $PTHREAD_CFLAGS"
  GTEST_LIBS="$PTHREAD_LIBS"

# If we're using g++ or other compilers which use -pthread rather than -lpthread,
# we likely need to add it as a linker flag as well, in order to prevent linker
# errors, on some platforms. NOTE: this seems to cause cygwin g++ to warn about -pthread
# but other than warning, it breaks nothing, and without it, Linux fails.
  ifelse([index(["$PTHREAD_CFLAGS"],["-pthread"])],[-1],[],[GTEST_LIBS="-pthread $GTEST_LIBS"])

else
  GTEST_CPPFLAGS=
  GTEST_LIBS=
fi
AC_SUBST([GTEST_VERSION])
AC_SUBST([GTEST_CPPFLAGS])
AC_SUBST([GTEST_LIBS])

# Check for valgrind if configuring with --enable-valgrind-testing.
# NOTE: on OS X we also have to pass --dsymutil in order to get line number info.
VALGRIND_ENVIRONMENT=
ac_enable_valgrind=no
AC_ARG_ENABLE(valgrind-testing,
     [  --enable-valgrind-testing     enable running tests under valgrind],
     [ ac_enable_valgrind=yes ],
     [ ac_enable_valgrind=no] )

if [ test "x${ac_enable_valgrind}" = xyes ]; then
  AC_CHECK_PROG(HAVE_VALGRIND, valgrind, yes, no)
  if [ test "x$HAVE_VALGRIND" = xyes ]; then
    VALGRIND_TESTING="1"
    if [ test -n $OSX ]; then
      VALGRIND_ENVIRONMENT="valgrind -q --leak-check=full --track-origins=yes --num-callers=50 --error-exitcode=1 --dsymutil=yes"
    else
      VALGRIND_ENVIRONMENT="valgrind -q --leak-check=full --read-var-info=yes --track-origins=yes --num-callers=50 --error-exitcode=1"
    fi
  fi
fi
AC_SUBST([VALGRIND_ENVIRONMENT])
AM_CONDITIONAL([VALGRIND_TESTING], [test -n "$VALGRIND_ENVIRONMENT"])

# Checks for header files.
# 	We do not want to die if stdint.h is not present, there is logic
# 	to account for this in <webvtt/util.h>. When we know the toolchain
# 	does not provide the C99 header, we should come up with alternatives,
# 	if possible, and this can be done in <webvtt/util.h>
AC_CHECK_HEADER([stdint.h],[],[])

# 	We do however require some standard C headers
AC_CHECK_HEADERS([stdlib.h string.h])

# Checks for library functions.
AC_FUNC_ERROR_AT_LINE
AC_FUNC_MALLOC
AC_CHECK_FUNC([memset],[],[AC_ERROR([missing function 'memset'])])
AC_CHECK_FUNC([malloc],[],[AC_ERROR([missing function 'malloc'])])
AC_CHECK_FUNC([free],[],[AC_ERROR([missing function 'free'])])
AC_CHECK_FUNC([strerror],[AC_DEFINE([HAVE_STRERROR],[1],[Define if strerror() is available])])

# Generate Makefiles
AC_CONFIG_FILES([
  Makefile
  include/Makefile
  src/Makefile
  src/libwebvtt/Makefile
  src/libwebvttxx/Makefile
  src/parsevtt/Makefile
  test/Makefile
  test/gtest/Makefile
  test/unit/Makefile
])

AC_OUTPUT
