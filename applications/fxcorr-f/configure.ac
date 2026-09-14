AC_INIT([fxcorr-f], [0.1.0], [kaichao])

AM_INIT_AUTOMAKE([foreign])

AC_PROG_CXX

PKG_CHECK_MODULES(FXCORRCOMMON, fxcorrcommon >= 0.1)
# fftw3f 头被 fxcorrcommon 的 architecture.h（GENERIC 分支）直接 include，必需
PKG_CHECK_MODULES(FFTW3F, fftw3f)
# P10: datareader 直接调用 mark5access（mark5bfix/summarizemark5bfile/mark5_stream）
PKG_CHECK_MODULES(MARK5ACCESS, mark5access >= 1.7)

CXXFLAGS="${CXXFLAGS} ${FXCORRCOMMON_CFLAGS} ${FFTW3F_CFLAGS} ${MARK5ACCESS_CFLAGS}"
LIBS="${FXCORRCOMMON_LIBS} ${FFTW3F_LIBS} ${MARK5ACCESS_LIBS}"

AC_CONFIG_FILES([ \
	Makefile \
	src/Makefile \
])
AC_OUTPUT
