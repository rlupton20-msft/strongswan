AM_CPPFLAGS = \
	-I$(top_srcdir)/src/libstrongswan

AM_CFLAGS = \
	-rdynamic

if MONOLITHIC
noinst_LTLIBRARIES = libstrongswan-pem.la
else
plugin_LTLIBRARIES = libstrongswan-pem.la
endif

libstrongswan_pem_la_SOURCES = \
	pem_plugin.h pem_plugin.c \
	pem_builder.c pem_builder.h \
	pem_encoder.c pem_encoder.h

libstrongswan_pem_la_LDFLAGS = -no-undefined -module -avoid-version
libstrongswan_pem_la_LIBADD = $(top_builddir)/src/libstrongswan/libstrongswan.la
