AC_INIT([fxcorr-x], [0.1.0], [kaichao])

AM_INIT_AUTOMAKE([foreign])

AC_PROG_CXX

PKG_CHECK_MODULES(FXCORRCOMMON, fxcorrcommon >= 0.1)
# fftw3f 头被 fxcorrcommon 的 architecture.h（GENERIC 分支）直接 include，必需
PKG_CHECK_MODULES(FFTW3F, fftw3f)

CXXFLAGS="${CXXFLAGS} ${FXCORRCOMMON_CFLAGS} ${FFTW3F_CFLAGS}"
LIBS="${FXCORRCOMMON_LIBS} ${FFTW3F_LIBS}"

AC_CONFIG_FILES([ \
	Makefile \
	src/Makefile \
])
AC_OUTPUT
