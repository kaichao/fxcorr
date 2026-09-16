AC_INIT([fxcorrcommon],[0.1],[kaichao])

AC_CONFIG_HEADERS([config.h])

#shared library versioning
LIBRARY_VERSION=0:0:0
#               | | |
#        +------+ | +---+
#        |        |     |
#     current:revision:age
#        |        |     |
#        |        |     +- increment if interfaces have been added
#        |        |        set to zero if interfaces have been removed
#        |        |           or changed
#        |        +- increment if source code has changed
#        |           set to zero if current is incremented
#        +- increment if interfaces have been added, removed or changed

PACKAGE_VERSION=AC_PACKAGE_VERSION

AC_SUBST([PACKAGE_VERSION])
AC_SUBST([LIBRARY_VERSION])

AC_CONFIG_MACRO_DIR([m4])

AM_INIT_AUTOMAKE([foreign])

LT_INIT

AC_PROG_CXX
AX_OPENMP

AC_LANG(C++)

AC_ARG_ENABLE(quiet, [AS_HELP_STRING([--enable-quiet],[disable some verbiage])], [CFLAG_QUIET="-DQUIET"], [CFLAG_QUIET=" "])

dnl **********************************************************
dnl **** Set path for Intel Performance Primitive library ****
dnl **********************************************************

ipp_enabled=1
AC_ARG_ENABLE(ipp, [AS_HELP_STRING([--disable-ipp],[disables IPP acceleration])])
if test "x$enable_ipp" = xno; then
	ipp_enabled=0
fi
PKG_CHECK_MODULES(IPP, ipp, [AC_DEFINE([HAVE_IPP], 1, [1=has IPP])], ipp_enabled=0)
AC_SUBST(ipp_enabled)
dnl IPP headers are not on the default include path and libfxcorrcommon.a
dnl contains IPP calls: both the header path and the link flags must travel
dnl with the .pc file for downstream packages
IPP_PC_CFLAGS=""
IPP_PC_LIBS=""
if test ${ipp_enabled} = 1; then
	IPP_PC_CFLAGS="${IPP_CFLAGS}"
	IPP_PC_LIBS="${IPP_LIBS}"
fi
AC_SUBST(IPP_PC_CFLAGS)
AC_SUBST(IPP_PC_LIBS)
dnl **********************************************************

AC_CHECK_LIB(pthread, pthread_create,,[AC_MSG_ERROR("need libpthread")])

if test ${ipp_enabled} = 0; then
	PKG_CHECK_MODULES(FFTW3, fftw3, [hasfftw=true], [hasfftw=false])
	if test "$hasfftw" == "false"; then
		AC_MSG_ERROR([Error: fftw needs to be installed if IPP is not])
	fi
	PKG_CHECK_MODULES(FFTW3F, fftw3f, [hasfftwf=true], [hasfftwf=false])
	if test "$hasfftwf" == "false"; then
		AC_MSG_ERROR([Error: fftwf needs to be installed if IPP is not])
	fi
	MATH_LIBS="${FFTW3_LIBS} ${FFTW3F_LIBS}"
	MATH_CFLAGS="${FFTW3_CFLAGS} ${FFTW3F_CFLAGS}"
else
	MATH_LIBS="${IPP_LIBS} ${LIBS}"
	MATH_CFLAGS="${IPP_CFLAGS}"
fi

AC_SUBST(mathlibs, [${MATH_LIBS}])
AC_SUBST(mathcflags, [${MATH_CFLAGS}])

dnl **********************************************************
dnl **** Find difxmessage library                         ****
dnl **********************************************************
PKG_CHECK_MODULES(DIFXMESSAGE, [difxmessage >= 2.9.0])
CPPFLAGS="${CPPFLAGS} ${DIFXMESSAGE_CFLAGS}"

dnl **********************************************************
dnl **** Find vdifio library                              ****
dnl **********************************************************
PKG_CHECK_MODULES(VDIFIO, vdifio >= 1.6)
CPPFLAGS="${CPPFLAGS} ${VDIFIO_CFLAGS}"

dnl **********************************************************
dnl **** Find codifio library (CODIF_HEADER_BYTES)         ****
dnl **********************************************************
PKG_CHECK_MODULES(CODIFIO, codifio >= 0.2)
CPPFLAGS="${CPPFLAGS} ${CODIFIO_CFLAGS}"

dnl **********************************************************
dnl **** Find mark5access library                         ****
dnl **********************************************************
PKG_CHECK_MODULES(M5ACCESS, mark5access >= 1.7)

dnl for Mutex lock in datastream.cpp (kept for mode.cpp FFTinitMutex)
AC_CHECK_LIB(rt, clock_gettime)

AC_C_BIGENDIAN

CXXFLAGS="${CXXFLAGS} ${M5ACCESS_CFLAGS} ${VDIFIO_CFLAGS} ${CODIFIO_CFLAGS} ${MATH_CFLAGS} ${FFTW3_CFLAGS} ${DIFXMESSAGE_CFLAGS} ${OPENMP_CXXFLAGS} ${CFLAG_QUIET}"

LIBS="${M5ACCESS_LIBS} ${VDIFIO_LIBS} ${CODIFIO_LIBS} ${DIFXMESSAGE_LIBS} $LIBS"

dnl packaged for the .la LIBADD so libtool can resolve all symbols on
dnl platforms that require -no-undefined (macOS)
AC_SUBST(difxlibs, [${M5ACCESS_LIBS} ${VDIFIO_LIBS} ${CODIFIO_LIBS} ${DIFXMESSAGE_LIBS}])
AC_SUBST(difxcflags, [${M5ACCESS_CFLAGS} ${VDIFIO_CFLAGS} ${CODIFIO_CFLAGS} ${DIFXMESSAGE_CFLAGS}])

AC_CONFIG_FILES([
	Makefile \
	fxcorrcommon.pc \
	src/Makefile \
	src/architecture.h \
])

AC_OUTPUT
