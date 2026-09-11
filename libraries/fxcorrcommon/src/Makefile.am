AM_CPPFLAGS = -I$(top_srcdir) -I$(top_builddir)

# architecture.h is generated from architecture.h.in by configure
BUILT_SOURCES = architecture.h

h_sources = \
	configuration.h \
	pcal.h \
	mathutil.h \
	sysutil.h \
	mode.h \
	mk5mode.h \
	polyco.h \
	visibility.h \
	model.h \
	datamuxer.h \
	alert.h \
	architecture.h \
	mpifxcorr.h

internal_h_sources = \
	pcal_impl.h \
	fraction.h

cpp_sources = \
	configuration.cpp \
	pcal.cpp \
	mathutil.cpp \
	sysutil.cpp \
	mode.cpp \
	mk5mode.cpp \
	polyco.cpp \
	visibility.cpp \
	model.cpp \
	datamuxer.cpp \
	alert.cpp

# installed under $(includedir)/fxcorrcommon to avoid clashing with the
# same-named headers mpifxcorr installs into $(includedir) itself
library_includedir = $(includedir)/fxcorrcommon
library_include_HEADERS = $(h_sources)

lib_LTLIBRARIES = \
	libfxcorrcommon.la

libfxcorrcommon_la_SOURCES = $(h_sources) $(internal_h_sources) $(cpp_sources)
libfxcorrcommon_la_LDFLAGS = -version-info $(LIBRARY_VERSION)
libfxcorrcommon_la_LIBADD = ${mathlibs} ${difxlibs}
