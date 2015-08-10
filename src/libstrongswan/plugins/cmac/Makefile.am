AM_CPPFLAGS = \
	-I$(top_srcdir)/src/libstrongswan

AM_CFLAGS = \
	-rdynamic

if MONOLITHIC
noinst_LTLIBRARIES = libstrongswan-cmac.la
else
plugin_LTLIBRARIES = libstrongswan-cmac.la
endif

libstrongswan_cmac_la_SOURCES = \
	cmac_plugin.h cmac_plugin.c cmac.h cmac.c

libstrongswan_cmac_la_LDFLAGS = -no-undefined -module -avoid-version
libstrongswan_cmac_la_LIBADD = $(top_builddir)/src/libstrongswan/libstrongswan.la