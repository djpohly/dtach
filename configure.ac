# Process this file with autoconf to produce a configure script.
AC_INIT(dtach, 0.8, djpohly@gmail.com)
AC_PREREQ(2.59)
AC_CONFIG_SRCDIR(dtmaster.c)
AC_CONFIG_HEADER(config.h)

# Checks for programs.
AC_PROG_CC
AC_PROG_GCC_TRADITIONAL

if test "$GCC" = yes; then
	CFLAGS="$CFLAGS -W -Wall";
fi

# Checks for libraries.
AC_CHECK_LIB(util, openpty)
AC_CHECK_LIB(socket, socket)

# Checks for header files.
AC_CHECK_HEADERS(fcntl.h sys/select.h sys/socket.h sys/time.h)
AC_CHECK_HEADERS(sys/ioctl.h sys/resource.h pty.h termios.h util.h)
AC_CHECK_HEADERS(libutil.h stropts.h)
AC_HEADER_TIME

# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_TYPE_PID_T
AC_TYPE_SSIZE_T

# Checks for library functions.
AC_TYPE_SIGNAL
AC_CHECK_FUNCS(atexit dup2 memset)
AC_CHECK_FUNCS(select socket strerror)
AC_CHECK_FUNCS(openpty forkpty ptsname grantpt unlockpt)

AC_SUBST(ac_config_files)
AC_SUBST(BUILD_DATE, `date +%Y-%m-%d`)
AC_DEFINE_UNQUOTED(BUILD_DATE, "$BUILD_DATE", Build date)
AC_CONFIG_FILES(Makefile dtach, chmod +x dtach)
AC_OUTPUT
