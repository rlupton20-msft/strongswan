AM_CPPFLAGS = \
	-I$(top_srcdir)/src/libstrongswan

AM_CFLAGS = \
	-rdynamic

if MONOLITHIC
noinst_LTLIBRARIES = libstrongswan-revocation.la
else
plugin_LTLIBRARIES = libstrongswan-revocation.la
endif

libstrongswan_revocation_la_SOURCES = \
	revocation_plugin.h revocation_plugin.c \
	revocation_validator.h revocation_validator.c

libstrongswan_revocation_la_LDFLAGS = -no-undefined -module -avoid-version
libstrongswan_revocation_la_LIBADD = $(top_builddir)/src/libstrongswan/libstrongswan.la
